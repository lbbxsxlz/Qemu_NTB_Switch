// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define DEV_NAME "ntb_idt_sisci"

#define NTB_IDT_SISCI_CMD_SEGID 0x53434944U
#define NTB_IDT_SISCI_CMD_SIZE 0x53434953U
#define NTB_IDT_SISCI_CMD_XLAT 0x53434958U
#define NTB_IDT_SISCI_CMD_AVAIL 0x53434941U
#define NTB_IDT_SISCI_CMD_UNAVAIL 0x53434955U

#define NTB_IDT_SISCI_CMD_REG 0
#define NTB_IDT_SISCI_DATA_LOW_REG 1
#define NTB_IDT_SISCI_DATA_HIGH_REG 2

#define NTB_IDT_SISCI_MMAP_LOCAL 0
#define NTB_IDT_SISCI_MMAP_REMOTE 1

#define NTB_IDT_SISCI_IOC_MAGIC 'S'
#define NTB_IDT_SISCI_INFO_F_LINK_UP 1U
#define NTB_IDT_SISCI_INFO_F_LOCAL_AVAILABLE 2U
#define NTB_IDT_SISCI_INFO_F_REMOTE_CONNECTED 4U

struct ntb_idt_sisci_ioc_info {
	__u32 local_node_id;
	__u32 peer_node_id;
	__u32 local_segment_id;
	__u32 remote_segment_id;
	__u64 local_segment_size;
	__u64 remote_segment_size;
	__u32 max_segments;
	__u32 flags;
};

struct ntb_idt_sisci_ioc_segment {
	__u32 segment_id;
	__u32 adapter_no;
	__u64 size;
};

struct ntb_idt_sisci_ioc_remote_segment {
	__u32 remote_node_id;
	__u32 segment_id;
	__u32 adapter_no;
	__u32 reserved;
	__u64 timeout_ms;
	__u64 size;
};

struct ntb_idt_sisci_ioc_barrier {
	__u32 flags;
	__u32 reserved;
};

#define NTB_IDT_SISCI_IOC_GET_INFO _IOR(NTB_IDT_SISCI_IOC_MAGIC, 0x01, struct ntb_idt_sisci_ioc_info)
#define NTB_IDT_SISCI_IOC_CREATE_SEGMENT _IOWR(NTB_IDT_SISCI_IOC_MAGIC, 0x02, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_PREPARE_SEGMENT _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x03, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_SET_AVAILABLE _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x04, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_SET_UNAVAILABLE _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x05, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_CONNECT_SEGMENT _IOWR(NTB_IDT_SISCI_IOC_MAGIC, 0x06, struct ntb_idt_sisci_ioc_remote_segment)
#define NTB_IDT_SISCI_IOC_STORE_BARRIER _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x07, struct ntb_idt_sisci_ioc_barrier)
#define NTB_IDT_SISCI_IOC_REMOVE_SEGMENT _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x08, struct ntb_idt_sisci_ioc_segment)

struct ntb_idt_sisci_dev {
	struct ntb_dev *ntb;
	struct device *dma_dev;
	struct miscdevice miscdev;
	struct work_struct service_work;
	int peer_idx;
	int mw_idx;
	struct mutex lock;
	wait_queue_head_t connect_wq;
	wait_queue_head_t poll_wq;
	bool removed;
	bool link_up;
	bool local_available;
	bool local_published;
	bool remote_announced;
	bool peer_ready;
	void *local_buf;
	dma_addr_t local_dma;
	resource_size_t local_size;
	u32 local_segment_id;
	void __iomem *peer_io;
	phys_addr_t peer_phys;
	resource_size_t peer_map_size;
	resource_size_t peer_usable_size;
	u64 remote_xlat;
	resource_size_t remote_size;
	u32 remote_segment_id;
};

static unsigned int buffer_size = 4 * 1024 * 1024;
module_param(buffer_size, uint, 0644);
MODULE_PARM_DESC(buffer_size, "Default SISCI local segment size in bytes");

static int peer_index;
module_param(peer_index, int, 0644);
MODULE_PARM_DESC(peer_index, "NTB peer index to use");

static int mw_index;
module_param(mw_index, int, 0644);
MODULE_PARM_DESC(mw_index, "NTB memory-window index to use");

static struct ntb_idt_sisci_dev *global_sisci;

static bool ntb_idt_sisci_has_peer_segment(struct ntb_idt_sisci_dev *app,
						   u32 segment_id)
{
	return READ_ONCE(app->removed) ||
		(READ_ONCE(app->peer_ready) &&
		 READ_ONCE(app->remote_segment_id) == segment_id);
}

static bool ntb_idt_sisci_can_poll_write(struct ntb_idt_sisci_dev *app)
{
	return READ_ONCE(app->removed) || READ_ONCE(app->peer_ready);
}

static void ntb_idt_sisci_set_link(struct ntb_idt_sisci_dev *app, bool link_up)
{
	WRITE_ONCE(app->link_up, link_up);
	if (!link_up) {
		WRITE_ONCE(app->peer_ready, false);
		WRITE_ONCE(app->remote_announced, false);
		WRITE_ONCE(app->local_published, false);
	}
	wake_up_interruptible(&app->connect_wq);
	wake_up_interruptible(&app->poll_wq);
}

static void ntb_idt_sisci_set_removed(struct ntb_idt_sisci_dev *app)
{
	WRITE_ONCE(app->removed, true);
	WRITE_ONCE(app->peer_ready, false);
	WRITE_ONCE(app->remote_announced, false);
	WRITE_ONCE(app->local_available, false);
	WRITE_ONCE(app->local_published, false);
	wake_up_interruptible(&app->connect_wq);
	wake_up_interruptible(&app->poll_wq);
}

static void ntb_idt_sisci_mark_peer_unready_locked(struct ntb_idt_sisci_dev *app)
{
	if (app->peer_io) {
		iounmap(app->peer_io);
		app->peer_io = NULL;
	}
	ntb_peer_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
	app->peer_phys = 0;
	app->peer_map_size = 0;
	app->peer_usable_size = 0;
	WRITE_ONCE(app->peer_ready, false);
	wake_up_interruptible(&app->connect_wq);
	wake_up_interruptible(&app->poll_wq);
}

static void ntb_idt_sisci_free_local_segment_locked(struct ntb_idt_sisci_dev *app)
{
	if (app->local_buf) {
		ntb_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
		dma_free_coherent(app->dma_dev, app->local_size, app->local_buf,
				  app->local_dma);
		app->local_buf = NULL;
	}
	app->local_dma = 0;
	app->local_size = 0;
	app->local_segment_id = 0;
	WRITE_ONCE(app->local_available, false);
	WRITE_ONCE(app->local_published, false);
}

static int ntb_idt_sisci_send_cmd(struct ntb_idt_sisci_dev *app, u32 cmd,
					  u64 data)
{
	u64 outbits = ntb_msg_outbits(app->ntb);
	int try;
	int ret;

	for (try = 0; try < 100; try++) {
		if (!(ntb_link_is_up(app->ntb, NULL, NULL) & BIT_ULL(app->peer_idx)))
			return -ENOLINK;
		ret = ntb_msg_clear_sts(app->ntb, outbits);
		if (ret)
			return ret;
		ret = ntb_peer_msg_write(app->ntb, app->peer_idx,
					 NTB_IDT_SISCI_DATA_LOW_REG,
					 lower_32_bits(data));
		if (ret)
			return ret;
		if (ntb_msg_read_sts(app->ntb) & outbits) {
			usleep_range(1000, 2000);
			continue;
		}
		ret = ntb_peer_msg_write(app->ntb, app->peer_idx,
					 NTB_IDT_SISCI_DATA_HIGH_REG,
					 upper_32_bits(data));
		if (ret)
			return ret;
		if (ntb_msg_read_sts(app->ntb) & outbits) {
			usleep_range(1000, 2000);
			continue;
		}
		ret = ntb_peer_msg_write(app->ntb, app->peer_idx,
					 NTB_IDT_SISCI_CMD_REG, cmd);
		if (ret)
			return ret;
		if (ntb_msg_read_sts(app->ntb) & outbits) {
			usleep_range(1000, 2000);
			continue;
		}
		return 0;
	}
	return -EAGAIN;
}

static int ntb_idt_sisci_publish_available_locked(struct ntb_idt_sisci_dev *app)
{
	int ret;

	if (!READ_ONCE(app->link_up) || !READ_ONCE(app->local_available) ||
	    !app->local_buf || READ_ONCE(app->local_published))
		return 0;
	ret = ntb_idt_sisci_send_cmd(app, NTB_IDT_SISCI_CMD_SEGID,
				       app->local_segment_id);
	if (ret)
		return ret;
	ret = ntb_idt_sisci_send_cmd(app, NTB_IDT_SISCI_CMD_SIZE,
				       app->local_size);
	if (ret)
		return ret;
	ret = ntb_idt_sisci_send_cmd(app, NTB_IDT_SISCI_CMD_XLAT,
				       app->local_dma);
	if (ret)
		return ret;
	ret = ntb_idt_sisci_send_cmd(app, NTB_IDT_SISCI_CMD_AVAIL,
				       app->local_segment_id);
	if (ret)
		return ret;
	WRITE_ONCE(app->local_published, true);
	return 0;
}

static int ntb_idt_sisci_publish_unavailable_locked(struct ntb_idt_sisci_dev *app,
						    u32 segment_id)
{
	bool should_publish = READ_ONCE(app->link_up) &&
		READ_ONCE(app->local_published);

	WRITE_ONCE(app->local_available, false);
	WRITE_ONCE(app->local_published, false);
	if (!should_publish)
		return 0;
	return ntb_idt_sisci_send_cmd(app, NTB_IDT_SISCI_CMD_UNAVAIL,
				       segment_id);
}

static int ntb_idt_sisci_recv_cmd(struct ntb_idt_sisci_dev *app, u32 *cmd,
					  u64 *data)
{
	u64 inbits = ntb_msg_inbits(app->ntb);
	u64 sts = ntb_msg_read_sts(app->ntb) & inbits;
	u32 low;
	u32 high;
	int pidx;

	if (hweight64(sts) < 3)
		return -ENODATA;
	*cmd = ntb_msg_read(app->ntb, &pidx, NTB_IDT_SISCI_CMD_REG);
	low = ntb_msg_read(app->ntb, &pidx, NTB_IDT_SISCI_DATA_LOW_REG);
	high = ntb_msg_read(app->ntb, &pidx, NTB_IDT_SISCI_DATA_HIGH_REG);
	*data = low | ((u64)high << 32);
	ntb_msg_clear_sts(app->ntb, inbits);
	return 0;
}

static int ntb_idt_sisci_setup_peer_mw_locked(struct ntb_idt_sisci_dev *app)
{
	resource_size_t usable_size;
	phys_addr_t phys_addr;
	resource_size_t map_size;
	int ret;

	if (app->peer_io)
		return 0;
	if (!READ_ONCE(app->remote_announced) || !app->remote_xlat ||
	    !app->remote_size)
		return 0;
	ret = ntb_peer_mw_get_addr(app->ntb, app->mw_idx, &phys_addr, &map_size);
	if (ret)
		return ret;
	ret = ntb_peer_mw_set_trans(app->ntb, app->peer_idx, app->mw_idx,
				      app->remote_xlat, app->remote_size);
	if (ret)
		return ret;
	app->peer_io = ioremap_wc(phys_addr, map_size);
	if (!app->peer_io) {
		ntb_peer_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
		return -ENOMEM;
	}
	usable_size = min(app->remote_size, map_size);
	app->peer_phys = phys_addr;
	app->peer_map_size = map_size;
	app->peer_usable_size = usable_size;
	WRITE_ONCE(app->peer_ready, true);
	wake_up_interruptible(&app->connect_wq);
	wake_up_interruptible(&app->poll_wq);
	dev_info(&app->ntb->dev,
		 "SISCI remote segment %u: xlat=%#llx usable=%pa map=%pa\n",
		 app->remote_segment_id, app->remote_xlat,
		 &app->peer_usable_size, &app->peer_map_size);
	return 0;
}

static void ntb_idt_sisci_service_work(struct work_struct *work)
{
	struct ntb_idt_sisci_dev *app = container_of(work,
		struct ntb_idt_sisci_dev, service_work);
	bool link_up;
	u32 cmd;
	u64 data;
	int ret;

	mutex_lock(&app->lock);
	if (READ_ONCE(app->removed))
		goto out;
	link_up = ntb_link_is_up(app->ntb, NULL, NULL) & BIT_ULL(app->peer_idx);
	ntb_idt_sisci_set_link(app, link_up);
	if (!link_up) {
		ntb_idt_sisci_mark_peer_unready_locked(app);
		goto out;
	}
	while (ntb_idt_sisci_recv_cmd(app, &cmd, &data) == 0) {
		switch (cmd) {
		case NTB_IDT_SISCI_CMD_SEGID:
			ntb_idt_sisci_mark_peer_unready_locked(app);
			WRITE_ONCE(app->remote_segment_id, (u32)data);
			WRITE_ONCE(app->remote_announced, false);
			break;
		case NTB_IDT_SISCI_CMD_SIZE:
			app->remote_size = data;
			break;
		case NTB_IDT_SISCI_CMD_XLAT:
			app->remote_xlat = data;
			break;
		case NTB_IDT_SISCI_CMD_AVAIL:
			WRITE_ONCE(app->remote_segment_id, (u32)data);
			WRITE_ONCE(app->remote_announced, true);
			ret = ntb_idt_sisci_setup_peer_mw_locked(app);
			if (ret)
				dev_err(&app->ntb->dev,
					"failed to setup SISCI peer MW: %d\n", ret);
			break;
		case NTB_IDT_SISCI_CMD_UNAVAIL:
			if (READ_ONCE(app->remote_segment_id) == (u32)data) {
				WRITE_ONCE(app->remote_announced, false);
				ntb_idt_sisci_mark_peer_unready_locked(app);
			}
			break;
		default:
			dev_dbg(&app->ntb->dev, "ignore SISCI command %#x data %#llx\n",
				cmd, data);
			break;
		}
	}
	ret = ntb_idt_sisci_publish_available_locked(app);
	if (ret)
		dev_dbg(&app->ntb->dev, "SISCI publish failed: %d\n", ret);
out:
	mutex_unlock(&app->lock);
}

static void ntb_idt_sisci_link_event(void *ctx)
{
	struct ntb_idt_sisci_dev *app = ctx;

	schedule_work(&app->service_work);
}

static void ntb_idt_sisci_msg_event(void *ctx)
{
	struct ntb_idt_sisci_dev *app = ctx;

	schedule_work(&app->service_work);
}

static const struct ntb_ctx_ops ntb_idt_sisci_ctx_ops = {
	.link_event = ntb_idt_sisci_link_event,
	.msg_event = ntb_idt_sisci_msg_event,
};

static int ntb_idt_sisci_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct ntb_idt_sisci_dev *app = container_of(miscdev,
						    struct ntb_idt_sisci_dev,
						    miscdev);

	file->private_data = app;
	return nonseekable_open(inode, file);
}

static int ntb_idt_sisci_alloc_local_segment_locked(struct ntb_idt_sisci_dev *app,
						    u32 segment_id, u64 size)
{
	resource_size_t addr_align = 1;
	resource_size_t size_align = 1;
	resource_size_t size_max = 0;
	resource_size_t req_size;
	int ret;

	if (READ_ONCE(app->removed))
		return -ENODEV;
	if (!size)
		size = buffer_size;
	if (size > SIZE_MAX)
		return -EMSGSIZE;
	if (app->local_buf) {
		if (app->local_segment_id == segment_id && app->local_size >= size)
			return 0;
		if (READ_ONCE(app->local_available))
			return -EBUSY;
		ntb_idt_sisci_free_local_segment_locked(app);
	}
	ret = ntb_mw_get_align(app->ntb, app->peer_idx, app->mw_idx,
			       &addr_align, &size_align, &size_max);
	if (ret)
		return ret;
	req_size = size;
	if (size_align > 1) {
		if (req_size > (resource_size_t)-1 - (size_align - 1))
			return -EMSGSIZE;
		req_size = ALIGN(req_size, size_align);
	}
	if (size_max && req_size > size_max)
		return -EMSGSIZE;
	if (!req_size)
		return -EINVAL;
	if (req_size > SIZE_MAX)
		return -EMSGSIZE;
	app->local_buf = dma_alloc_coherent(app->dma_dev, (size_t)req_size,
					  &app->local_dma, GFP_KERNEL);
	if (!app->local_buf)
		return -ENOMEM;
	if (addr_align > 1 && !IS_ALIGNED(app->local_dma, addr_align)) {
		ret = -EINVAL;
		goto err_free;
	}
	ret = ntb_mw_set_trans(app->ntb, app->peer_idx, app->mw_idx,
			       app->local_dma, req_size);
	if (ret)
		goto err_free;
	app->local_size = req_size;
	app->local_segment_id = segment_id;
	WRITE_ONCE(app->local_available, false);
	WRITE_ONCE(app->local_published, false);
	dev_info(&app->ntb->dev, "SISCI local segment %u: dma=%pad size=%pa\n",
		 segment_id, &app->local_dma, &app->local_size);
	return 0;
err_free:
	dma_free_coherent(app->dma_dev, (size_t)req_size, app->local_buf,
			  app->local_dma);
	app->local_buf = NULL;
	app->local_dma = 0;
	return ret;
}

static long ntb_idt_sisci_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	struct ntb_idt_sisci_dev *app = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret;

	switch (cmd) {
	case NTB_IDT_SISCI_IOC_GET_INFO: {
		struct ntb_idt_sisci_ioc_info info;

		memset(&info, 0, sizeof(info));
		mutex_lock(&app->lock);
		if (READ_ONCE(app->removed)) {
			mutex_unlock(&app->lock);
			return -ENODEV;
		}
		info.peer_node_id = app->peer_idx;
		info.local_segment_id = app->local_segment_id;
		info.remote_segment_id = READ_ONCE(app->remote_segment_id);
		info.local_segment_size = app->local_size;
		info.remote_segment_size = app->peer_usable_size;
		info.max_segments = 1;
		if (READ_ONCE(app->link_up))
			info.flags |= NTB_IDT_SISCI_INFO_F_LINK_UP;
		if (READ_ONCE(app->local_available))
			info.flags |= NTB_IDT_SISCI_INFO_F_LOCAL_AVAILABLE;
		if (READ_ONCE(app->peer_ready))
			info.flags |= NTB_IDT_SISCI_INFO_F_REMOTE_CONNECTED;
		mutex_unlock(&app->lock);
		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case NTB_IDT_SISCI_IOC_CREATE_SEGMENT: {
		struct ntb_idt_sisci_ioc_segment segment;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		mutex_lock(&app->lock);
		ret = ntb_idt_sisci_alloc_local_segment_locked(app,
				segment.segment_id, segment.size);
		if (!ret)
			segment.size = app->local_size;
		mutex_unlock(&app->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &segment, sizeof(segment)))
			return -EFAULT;
		return 0;
	}
	case NTB_IDT_SISCI_IOC_PREPARE_SEGMENT: {
		struct ntb_idt_sisci_ioc_segment segment;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		mutex_lock(&app->lock);
		ret = READ_ONCE(app->removed) ? -ENODEV : 0;
		if (!ret && (!app->local_buf ||
		    app->local_segment_id != segment.segment_id))
			ret = -ENOENT;
		mutex_unlock(&app->lock);
		return ret;
	}
	case NTB_IDT_SISCI_IOC_SET_AVAILABLE: {
		struct ntb_idt_sisci_ioc_segment segment;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		mutex_lock(&app->lock);
		if (READ_ONCE(app->removed))
			ret = -ENODEV;
		else if (!app->local_buf || app->local_segment_id != segment.segment_id)
			ret = -ENOENT;
		else {
			WRITE_ONCE(app->local_available, true);
			WRITE_ONCE(app->local_published, false);
			ret = ntb_idt_sisci_publish_available_locked(app);
		}
		mutex_unlock(&app->lock);
		return ret;
	}
	case NTB_IDT_SISCI_IOC_SET_UNAVAILABLE: {
		struct ntb_idt_sisci_ioc_segment segment;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		mutex_lock(&app->lock);
		if (READ_ONCE(app->removed))
			ret = -ENODEV;
		else if (!app->local_buf || app->local_segment_id != segment.segment_id)
			ret = -ENOENT;
		else
			ret = ntb_idt_sisci_publish_unavailable_locked(app,
					segment.segment_id);
		mutex_unlock(&app->lock);
		return ret;
	}
	case NTB_IDT_SISCI_IOC_REMOVE_SEGMENT: {
		struct ntb_idt_sisci_ioc_segment segment;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		mutex_lock(&app->lock);
		if (READ_ONCE(app->removed))
			ret = -ENODEV;
		else if (!app->local_buf || app->local_segment_id != segment.segment_id)
			ret = -ENOENT;
		else {
			ret = ntb_idt_sisci_publish_unavailable_locked(app,
					segment.segment_id);
			if (!ret)
				ntb_idt_sisci_free_local_segment_locked(app);
		}
		mutex_unlock(&app->lock);
		return ret;
	}
	case NTB_IDT_SISCI_IOC_CONNECT_SEGMENT: {
		struct ntb_idt_sisci_ioc_remote_segment segment;
		long wait_ret;

		if (copy_from_user(&segment, argp, sizeof(segment)))
			return -EFAULT;
		if (file->f_flags & O_NONBLOCK) {
			if (!ntb_idt_sisci_has_peer_segment(app, segment.segment_id))
				return -EAGAIN;
		} else if (segment.timeout_ms == U64_MAX) {
			ret = wait_event_interruptible(app->connect_wq,
				ntb_idt_sisci_has_peer_segment(app, segment.segment_id));
			if (ret)
				return ret;
		} else {
			wait_ret = wait_event_interruptible_timeout(app->connect_wq,
				ntb_idt_sisci_has_peer_segment(app, segment.segment_id),
				msecs_to_jiffies(segment.timeout_ms));
			if (wait_ret < 0)
				return wait_ret;
			if (wait_ret == 0)
				return -ETIMEDOUT;
		}
		mutex_lock(&app->lock);
		if (READ_ONCE(app->removed))
			ret = -ENODEV;
		else if (!READ_ONCE(app->peer_ready) ||
			 READ_ONCE(app->remote_segment_id) != segment.segment_id)
			ret = -ENOLINK;
		else {
			segment.size = app->peer_usable_size;
			ret = 0;
		}
		mutex_unlock(&app->lock);
		if (ret)
			return ret;
		if (copy_to_user(argp, &segment, sizeof(segment)))
			return -EFAULT;
		return 0;
	}
	case NTB_IDT_SISCI_IOC_STORE_BARRIER: {
		struct ntb_idt_sisci_ioc_barrier barrier;

		if (copy_from_user(&barrier, argp, sizeof(barrier)))
			return -EFAULT;
		wmb();
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int ntb_idt_sisci_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ntb_idt_sisci_dev *app = file->private_data;
	unsigned long region = vma->vm_pgoff;
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret;

	mutex_lock(&app->lock);
	if (READ_ONCE(app->removed)) {
		ret = -ENODEV;
		goto out;
	}
	switch (region) {
	case NTB_IDT_SISCI_MMAP_LOCAL:
		if (!app->local_buf || size > PAGE_ALIGN(app->local_size)) {
			ret = -EINVAL;
			goto out;
		}
		ret = dma_mmap_coherent(app->dma_dev, vma, app->local_buf,
					 app->local_dma, app->local_size);
		break;
	case NTB_IDT_SISCI_MMAP_REMOTE:
		if (!READ_ONCE(app->peer_ready) || !app->peer_io ||
		    !PAGE_ALIGNED(app->peer_phys) ||
		    size > PAGE_ALIGN(app->peer_usable_size)) {
			ret = -EINVAL;
			goto out;
		}
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		ret = io_remap_pfn_range(vma, vma->vm_start,
					 app->peer_phys >> PAGE_SHIFT, size,
					 vma->vm_page_prot);
		break;
	default:
		ret = -EINVAL;
		break;
	}
out:
	mutex_unlock(&app->lock);
	return ret;
}

static __poll_t ntb_idt_sisci_poll(struct file *file, poll_table *wait)
{
	struct ntb_idt_sisci_dev *app = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &app->connect_wq, wait);
	poll_wait(file, &app->poll_wq, wait);
	if (ntb_idt_sisci_can_poll_write(app))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (READ_ONCE(app->removed))
		mask |= EPOLLERR | EPOLLHUP;
	return mask;
}

static const struct file_operations ntb_idt_sisci_fops = {
	.owner = THIS_MODULE,
	.open = ntb_idt_sisci_open,
	.unlocked_ioctl = ntb_idt_sisci_ioctl,
	.mmap = ntb_idt_sisci_mmap,
	.poll = ntb_idt_sisci_poll,
	.llseek = no_llseek,
};

static int ntb_idt_sisci_enable(struct ntb_idt_sisci_dev *app)
{
	u64 inbits;
	u64 outbits;
	int ret;

	ret = ntb_set_ctx(app->ntb, app, &ntb_idt_sisci_ctx_ops);
	if (ret)
		return ret;
	inbits = ntb_msg_inbits(app->ntb);
	outbits = ntb_msg_outbits(app->ntb);
	if (!inbits || !outbits) {
		ret = -EOPNOTSUPP;
		goto err_clear_ctx;
	}
	ntb_msg_set_mask(app->ntb, inbits | outbits);
	ntb_msg_clear_sts(app->ntb, inbits | outbits);
	ret = ntb_msg_clear_mask(app->ntb, BIT_ULL(__ffs64(inbits)));
	if (ret)
		goto err_clear_ctx;
	ret = ntb_link_enable(app->ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);
	if (ret)
		goto err_clear_ctx;
	ntb_link_event(app->ntb);
	return 0;
err_clear_ctx:
	ntb_clear_ctx(app->ntb);
	return ret;
}

static void ntb_idt_sisci_disable(struct ntb_idt_sisci_dev *app)
{
	ntb_link_disable(app->ntb);
	ntb_clear_ctx(app->ntb);
}

static int ntb_idt_sisci_probe(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct ntb_idt_sisci_dev *app;
	int ret;

	if (global_sisci)
		return -EBUSY;
	if (ntb_peer_port_count(ntb) <= peer_index)
		return -ENODEV;
	if (ntb_peer_mw_count(ntb) <= mw_index)
		return -ENODEV;
	app = kzalloc(sizeof(*app), GFP_KERNEL);
	if (!app)
		return -ENOMEM;
	app->ntb = ntb;
	app->dma_dev = &ntb->pdev->dev;
	app->peer_idx = peer_index;
	app->mw_idx = mw_index;
	mutex_init(&app->lock);
	init_waitqueue_head(&app->connect_wq);
	init_waitqueue_head(&app->poll_wq);
	INIT_WORK(&app->service_work, ntb_idt_sisci_service_work);
	app->miscdev.minor = MISC_DYNAMIC_MINOR;
	app->miscdev.name = DEV_NAME;
	app->miscdev.fops = &ntb_idt_sisci_fops;
	ret = misc_register(&app->miscdev);
	if (ret)
		goto err_free;
	global_sisci = app;
	ret = ntb_idt_sisci_enable(app);
	if (ret)
		goto err_misc;
	dev_info(&ntb->dev, "registered /dev/%s peer=%d mw=%d default_segment=%u\n",
		 DEV_NAME, app->peer_idx, app->mw_idx, buffer_size);
	return 0;
err_misc:
	global_sisci = NULL;
	misc_deregister(&app->miscdev);
err_free:
	kfree(app);
	return ret;
}

static void ntb_idt_sisci_remove(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct ntb_idt_sisci_dev *app = global_sisci;

	if (!app || app->ntb != ntb)
		return;
	ntb_idt_sisci_set_removed(app);
	ntb_idt_sisci_disable(app);
	cancel_work_sync(&app->service_work);
	misc_deregister(&app->miscdev);
	mutex_lock(&app->lock);
	ntb_idt_sisci_mark_peer_unready_locked(app);
	ntb_idt_sisci_free_local_segment_locked(app);
	mutex_unlock(&app->lock);
	global_sisci = NULL;
	kfree(app);
}

static struct ntb_client ntb_idt_sisci_client = {
	.ops = {
		.probe = ntb_idt_sisci_probe,
		.remove = ntb_idt_sisci_remove,
	},
};

static int __init ntb_idt_sisci_init(void)
{
	return ntb_register_client(&ntb_idt_sisci_client);
}

static void __exit ntb_idt_sisci_exit(void)
{
	ntb_unregister_client(&ntb_idt_sisci_client);
}

module_init(ntb_idt_sisci_init);
module_exit(ntb_idt_sisci_exit);

MODULE_DESCRIPTION("IDT NTB SISCI-like shared-memory segment channel");
MODULE_LICENSE("GPL");
