// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/byteorder/generic.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define DRV_NAME "ntb_idt_app"
#define DEV_NAME "ntb_idt_app"

#define NTB_IDT_APP_FRAME_MAGIC 0x49544150U
#define NTB_IDT_APP_CMD_SIZE 0x49544153U
#define NTB_IDT_APP_CMD_XLAT 0x49544158U
#define NTB_IDT_APP_CMD_KICK 0x4954414bU

#define NTB_IDT_APP_CMD_REG 0
#define NTB_IDT_APP_DATA_LOW_REG 1
#define NTB_IDT_APP_DATA_HIGH_REG 2

#define NTB_IDT_APP_MMAP_RX 0
#define NTB_IDT_APP_MMAP_TX 1

#define NTB_IDT_APP_IOC_MAGIC 'N'

struct ntb_idt_app_ioc_info {
	__u64 rx_map_size;
	__u64 tx_map_size;
	__u64 rx_payload_size;
	__u64 tx_payload_size;
	__u32 frame_header_size;
	__u32 reserved;
};

struct ntb_idt_app_ioc_tx {
	__u64 len;
};

struct ntb_idt_app_ioc_rx {
	__u64 len;
	__u32 seq;
	__u32 reserved;
};

#define NTB_IDT_APP_IOC_GET_INFO _IOR(NTB_IDT_APP_IOC_MAGIC, 0x01, struct ntb_idt_app_ioc_info)
#define NTB_IDT_APP_IOC_TX_COMMIT _IOW(NTB_IDT_APP_IOC_MAGIC, 0x02, struct ntb_idt_app_ioc_tx)
#define NTB_IDT_APP_IOC_RX_WAIT _IOR(NTB_IDT_APP_IOC_MAGIC, 0x03, struct ntb_idt_app_ioc_rx)

struct ntb_idt_app_frame {
	__le32 magic;
	__le32 len;
	__le32 seq;
	__le32 flags;
};

struct ntb_idt_app_dev {
	struct ntb_dev *ntb;
	struct device *dma_dev;
	struct miscdevice miscdev;
	struct work_struct service_work;

	int peer_idx;
	int mw_idx;

	struct mutex lock;
	spinlock_t state_lock;
	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;

	bool removed;
	bool link_up;
	bool peer_ready;
	bool rx_avail;

	void *rx_buf;
	dma_addr_t rx_dma;
	resource_size_t rx_size;
	size_t rx_len;

	void __iomem *peer_io;
	phys_addr_t peer_phys;
	resource_size_t peer_map_size;
	resource_size_t peer_size;
	u64 peer_xlat;

	u32 tx_seq;
	u32 rx_seq;
};

static unsigned int buffer_size = 4 * 1024 * 1024;
module_param(buffer_size, uint, 0644);
MODULE_PARM_DESC(buffer_size, "Inbound shared memory window size in bytes");

static int peer_index;
module_param(peer_index, int, 0644);
MODULE_PARM_DESC(peer_index, "NTB peer index to use");

static int mw_index;
module_param(mw_index, int, 0644);
MODULE_PARM_DESC(mw_index, "NTB memory-window index to use");

static struct ntb_idt_app_dev *global_app;

static bool ntb_idt_app_can_read(struct ntb_idt_app_dev *app)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&app->state_lock, flags);
	ready = app->rx_avail || app->removed;
	spin_unlock_irqrestore(&app->state_lock, flags);

	return ready;
}

static bool ntb_idt_app_can_write(struct ntb_idt_app_dev *app)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&app->state_lock, flags);
	ready = (app->peer_ready && app->link_up) || app->removed;
	spin_unlock_irqrestore(&app->state_lock, flags);

	return ready;
}

static bool ntb_idt_app_peer_ready(struct ntb_idt_app_dev *app)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&app->state_lock, flags);
	ready = app->peer_ready;
	spin_unlock_irqrestore(&app->state_lock, flags);

	return ready;
}

static void ntb_idt_app_set_link(struct ntb_idt_app_dev *app, bool link_up)
{
	unsigned long flags;

	spin_lock_irqsave(&app->state_lock, flags);
	app->link_up = link_up;
	if (!link_up)
		app->peer_ready = false;
	spin_unlock_irqrestore(&app->state_lock, flags);

	wake_up_interruptible(&app->write_wq);
}

static void ntb_idt_app_set_removed(struct ntb_idt_app_dev *app)
{
	unsigned long flags;

	spin_lock_irqsave(&app->state_lock, flags);
	app->removed = true;
	app->peer_ready = false;
	spin_unlock_irqrestore(&app->state_lock, flags);

	wake_up_interruptible(&app->read_wq);
	wake_up_interruptible(&app->write_wq);
}

static void ntb_idt_app_free_peer_mw(struct ntb_idt_app_dev *app)
{
	if (app->peer_io) {
		iounmap(app->peer_io);
		app->peer_io = NULL;
	}

	ntb_peer_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
	app->peer_phys = 0;
	app->peer_map_size = 0;
	app->peer_size = 0;
	app->peer_xlat = 0;
}

static void ntb_idt_app_free_inbound_mw(struct ntb_idt_app_dev *app)
{
	if (app->rx_buf) {
		ntb_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
		dma_free_coherent(app->dma_dev, app->rx_size, app->rx_buf,
				  app->rx_dma);
		app->rx_buf = NULL;
	}

	app->rx_dma = 0;
	app->rx_size = 0;
}

static int ntb_idt_app_setup_inbound_mw(struct ntb_idt_app_dev *app)
{
	resource_size_t addr_align = 1;
	resource_size_t size_align = 1;
	resource_size_t size_max = 0;
	resource_size_t req_size;
	int ret;

	if (app->rx_buf)
		return 0;

	ret = ntb_mw_get_align(app->ntb, app->peer_idx, app->mw_idx,
				 &addr_align, &size_align, &size_max);
	if (ret)
		return ret;

	req_size = buffer_size;
	if (size_max && req_size > size_max)
		req_size = size_max;
	if (size_align > 1)
		req_size = round_down(req_size, size_align);
	if (req_size <= sizeof(struct ntb_idt_app_frame))
		return -EINVAL;

	app->rx_buf = dma_alloc_coherent(app->dma_dev, req_size, &app->rx_dma,
					  GFP_KERNEL);
	if (!app->rx_buf)
		return -ENOMEM;
	if (addr_align > 1 && !IS_ALIGNED(app->rx_dma, addr_align)) {
		ret = -EINVAL;
		goto err_free;
	}

	ret = ntb_mw_set_trans(app->ntb, app->peer_idx, app->mw_idx,
				app->rx_dma, req_size);
	if (ret)
		goto err_free;

	app->rx_size = req_size;
	dev_info(&app->ntb->dev, "inbound MW%d: dma=%pad size=%pa\n",
		 app->mw_idx, &app->rx_dma, &app->rx_size);

	return 0;

err_free:
	dma_free_coherent(app->dma_dev, req_size, app->rx_buf, app->rx_dma);
	app->rx_buf = NULL;
	app->rx_dma = 0;
	return ret;
}

static int ntb_idt_app_setup_peer_mw(struct ntb_idt_app_dev *app)
{
	u64 peer_xlat = app->peer_xlat;
	resource_size_t peer_size = app->peer_size;
	resource_size_t usable_size;
	phys_addr_t phys_addr;
	resource_size_t map_size;
	int ret;

	if (!peer_xlat || peer_size <= sizeof(struct ntb_idt_app_frame))
		return -ENODATA;

	ntb_idt_app_free_peer_mw(app);

	ret = ntb_peer_mw_get_addr(app->ntb, app->mw_idx, &phys_addr, &map_size);
	if (ret)
		return ret;

	ret = ntb_peer_mw_set_trans(app->ntb, app->peer_idx, app->mw_idx,
				      peer_xlat, peer_size);
	if (ret)
		return ret;

	app->peer_io = ioremap_wc(phys_addr, map_size);
	if (!app->peer_io) {
		ntb_peer_mw_clear_trans(app->ntb, app->peer_idx, app->mw_idx);
		return -ENOMEM;
	}

	usable_size = min(peer_size, map_size);
	app->peer_phys = phys_addr;
	app->peer_map_size = map_size;
	app->peer_xlat = peer_xlat;
	app->peer_size = usable_size;

	dev_info(&app->ntb->dev,
		 "outbound MW%d: peer_xlat=%#llx usable=%pa map=%pa\n",
		 app->mw_idx, app->peer_xlat, &app->peer_size, &app->peer_map_size);

	return 0;

}

static int ntb_idt_app_send_cmd(struct ntb_idt_app_dev *app, u32 cmd, u64 data)
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
					 NTB_IDT_APP_DATA_LOW_REG, lower_32_bits(data));
		if (ret)
			return ret;
		if (ntb_msg_read_sts(app->ntb) & outbits) {
			usleep_range(1000, 2000);
			continue;
		}

		ret = ntb_peer_msg_write(app->ntb, app->peer_idx,
					 NTB_IDT_APP_DATA_HIGH_REG, upper_32_bits(data));
		if (ret)
			return ret;
		if (ntb_msg_read_sts(app->ntb) & outbits) {
			usleep_range(1000, 2000);
			continue;
		}

		ret = ntb_peer_msg_write(app->ntb, app->peer_idx,
					 NTB_IDT_APP_CMD_REG, cmd);
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

static int ntb_idt_app_publish_ready(struct ntb_idt_app_dev *app)
{
	int ret;

	if (!app->rx_buf)
		return -ENODEV;

	ret = ntb_idt_app_send_cmd(app, NTB_IDT_APP_CMD_SIZE, app->rx_size);
	if (ret)
		return ret;

	return ntb_idt_app_send_cmd(app, NTB_IDT_APP_CMD_XLAT, app->rx_dma);
}

static int ntb_idt_app_recv_cmd(struct ntb_idt_app_dev *app, u32 *cmd, u64 *data)
{
	u64 inbits = ntb_msg_inbits(app->ntb);
	u64 sts = ntb_msg_read_sts(app->ntb) & inbits;
	u32 low;
	u32 high;
	int pidx;

	if (hweight64(sts) < 3)
		return -ENODATA;

	*cmd = ntb_msg_read(app->ntb, &pidx, NTB_IDT_APP_CMD_REG);
	low = ntb_msg_read(app->ntb, &pidx, NTB_IDT_APP_DATA_LOW_REG);
	high = ntb_msg_read(app->ntb, &pidx, NTB_IDT_APP_DATA_HIGH_REG);
	*data = low | ((u64)high << 32);

	ntb_msg_clear_sts(app->ntb, inbits);

	return 0;
}

static void ntb_idt_app_complete_rx(struct ntb_idt_app_dev *app, u64 data)
{
	struct ntb_idt_app_frame *hdr = app->rx_buf;
	unsigned long flags;
	size_t len;
	u32 magic;

	dma_rmb();
	magic = le32_to_cpu(hdr->magic);
	len = le32_to_cpu(hdr->len);
	if (magic != NTB_IDT_APP_FRAME_MAGIC || len != data ||
	    len > app->rx_size - sizeof(*hdr)) {
		dev_warn(&app->ntb->dev,
			 "drop invalid frame: magic=%#x len=%zu data=%llu\n",
			 magic, len, data);
		return;
	}

	spin_lock_irqsave(&app->state_lock, flags);
	app->rx_len = len;
	app->rx_seq = le32_to_cpu(hdr->seq);
	app->rx_avail = true;
	spin_unlock_irqrestore(&app->state_lock, flags);

	wake_up_interruptible(&app->read_wq);
}

static void ntb_idt_app_try_setup_peer_locked(struct ntb_idt_app_dev *app)
{
	unsigned long flags;
	int ret;

	if (!app->peer_xlat || !app->peer_size)
		return;

	ret = ntb_idt_app_setup_peer_mw(app);
	if (ret) {
		dev_err(&app->ntb->dev, "failed to setup peer MW: %d\n", ret);
		return;
	}

	spin_lock_irqsave(&app->state_lock, flags);
	app->peer_ready = true;
	spin_unlock_irqrestore(&app->state_lock, flags);
	wake_up_interruptible(&app->write_wq);
}

static void ntb_idt_app_service_work(struct work_struct *work)
{
	struct ntb_idt_app_dev *app = container_of(work, struct ntb_idt_app_dev,
						 service_work);
	bool link_up;
	u32 cmd;
	u64 data;
	int ret;

	mutex_lock(&app->lock);

	if (app->removed)
		goto out;

	link_up = ntb_link_is_up(app->ntb, NULL, NULL) & BIT_ULL(app->peer_idx);
	ntb_idt_app_set_link(app, link_up);
	if (!link_up)
		goto out;

	ret = ntb_idt_app_setup_inbound_mw(app);
	if (ret) {
		dev_err(&app->ntb->dev, "failed to setup inbound MW: %d\n", ret);
		goto out;
	}

	while (ntb_idt_app_recv_cmd(app, &cmd, &data) == 0) {
		switch (cmd) {
		case NTB_IDT_APP_CMD_SIZE:
			app->peer_size = data;
			ntb_idt_app_try_setup_peer_locked(app);
			break;
		case NTB_IDT_APP_CMD_XLAT:
			app->peer_xlat = data;
			ntb_idt_app_try_setup_peer_locked(app);
			break;
		case NTB_IDT_APP_CMD_KICK:
			ntb_idt_app_complete_rx(app, data);
			break;
		default:
			dev_dbg(&app->ntb->dev, "ignore command %#x data %#llx\n",
				cmd, data);
			break;
		}
	}

	if (!ntb_idt_app_peer_ready(app)) {
		ret = ntb_idt_app_publish_ready(app);
		if (ret)
			dev_dbg(&app->ntb->dev, "ready publish failed: %d\n", ret);
	}

out:
	mutex_unlock(&app->lock);
}

static void ntb_idt_app_link_event(void *ctx)
{
	struct ntb_idt_app_dev *app = ctx;

	schedule_work(&app->service_work);
}

static void ntb_idt_app_msg_event(void *ctx)
{
	struct ntb_idt_app_dev *app = ctx;

	schedule_work(&app->service_work);
}

static const struct ntb_ctx_ops ntb_idt_app_ctx_ops = {
	.link_event = ntb_idt_app_link_event,
	.msg_event = ntb_idt_app_msg_event,
};

static int ntb_idt_app_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct ntb_idt_app_dev *app = container_of(miscdev,
						    struct ntb_idt_app_dev, miscdev);

	file->private_data = app;
	return nonseekable_open(inode, file);
}

static size_t ntb_idt_app_rx_payload_size(struct ntb_idt_app_dev *app)
{
	if (app->rx_size <= sizeof(struct ntb_idt_app_frame))
		return 0;

	return app->rx_size - sizeof(struct ntb_idt_app_frame);
}

static size_t ntb_idt_app_tx_payload_size(struct ntb_idt_app_dev *app)
{
	if (app->peer_size <= sizeof(struct ntb_idt_app_frame))
		return 0;

	return app->peer_size - sizeof(struct ntb_idt_app_frame);
}

static int ntb_idt_app_tx_commit_locked(struct ntb_idt_app_dev *app, size_t len)
{
	struct ntb_idt_app_frame hdr;
	int ret;

	if (app->removed)
		return -ENODEV;
	if (!app->peer_ready || !app->peer_io)
		return -ENOLINK;
	if (len > U32_MAX || len > ntb_idt_app_tx_payload_size(app))
		return -EMSGSIZE;

	app->tx_seq++;
	hdr.magic = cpu_to_le32(NTB_IDT_APP_FRAME_MAGIC);
	hdr.len = cpu_to_le32((u32)len);
	hdr.seq = cpu_to_le32(app->tx_seq);
	hdr.flags = 0;

	memcpy_toio(app->peer_io, &hdr, sizeof(hdr));
	wmb();

	ret = ntb_idt_app_send_cmd(app, NTB_IDT_APP_CMD_KICK, len);
	return ret;
}

static ssize_t ntb_idt_app_read(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct ntb_idt_app_dev *app = file->private_data;
	unsigned long flags;
	size_t len;
	int ret;

	if (!count)
		return 0;

	if (file->f_flags & O_NONBLOCK) {
		if (!ntb_idt_app_can_read(app))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(app->read_wq,
						   ntb_idt_app_can_read(app));
		if (ret)
			return ret;
	}

	spin_lock_irqsave(&app->state_lock, flags);
	if (app->removed) {
		spin_unlock_irqrestore(&app->state_lock, flags);
		return -ENODEV;
	}
	if (!app->rx_avail) {
		spin_unlock_irqrestore(&app->state_lock, flags);
		return -EAGAIN;
	}
	len = app->rx_len;
	spin_unlock_irqrestore(&app->state_lock, flags);

	if (count < len)
		return -EMSGSIZE;

	dma_rmb();
	if (copy_to_user(buf, (u8 *)app->rx_buf + sizeof(struct ntb_idt_app_frame), len))
		return -EFAULT;

	spin_lock_irqsave(&app->state_lock, flags);
	app->rx_avail = false;
	app->rx_len = 0;
	spin_unlock_irqrestore(&app->state_lock, flags);

	return len;
}

static ssize_t ntb_idt_app_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	struct ntb_idt_app_dev *app = file->private_data;
	u8 __iomem *payload;
	ssize_t ret;

	if (!count)
		return 0;

	if (file->f_flags & O_NONBLOCK) {
		if (!ntb_idt_app_can_write(app))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(app->write_wq,
						   ntb_idt_app_can_write(app));
		if (ret)
			return ret;
	}

	mutex_lock(&app->lock);

	if (app->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (!app->peer_ready || !app->peer_io) {
		ret = -ENOLINK;
		goto out;
	}
	if (count > U32_MAX) {
		ret = -EMSGSIZE;
		goto out;
	}
	if (count > ntb_idt_app_tx_payload_size(app)) {
		ret = -EMSGSIZE;
		goto out;
	}

	payload = (u8 __iomem *)app->peer_io + sizeof(struct ntb_idt_app_frame);
	if (copy_from_user((void __force *)payload, buf, count)) {
		ret = -EFAULT;
		goto out;
	}

	ret = ntb_idt_app_tx_commit_locked(app, count);
	if (!ret)
		ret = count;

out:
	mutex_unlock(&app->lock);
	return ret;
}

static long ntb_idt_app_ioctl(struct file *file, unsigned int cmd,
				      unsigned long arg)
{
	struct ntb_idt_app_dev *app = file->private_data;
	void __user *argp = (void __user *)arg;
	unsigned long flags;
	int ret;

	switch (cmd) {
	case NTB_IDT_APP_IOC_GET_INFO: {
		struct ntb_idt_app_ioc_info info;

		memset(&info, 0, sizeof(info));
		mutex_lock(&app->lock);
		if (app->removed) {
			mutex_unlock(&app->lock);
			return -ENODEV;
		}

		info.rx_map_size = app->rx_size;
		info.tx_map_size = app->peer_ready ? app->peer_size : 0;
		info.rx_payload_size = ntb_idt_app_rx_payload_size(app);
		info.tx_payload_size = app->peer_ready ?
			ntb_idt_app_tx_payload_size(app) : 0;
		info.frame_header_size = sizeof(struct ntb_idt_app_frame);
		mutex_unlock(&app->lock);

		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case NTB_IDT_APP_IOC_TX_COMMIT: {
		struct ntb_idt_app_ioc_tx tx;

		if (copy_from_user(&tx, argp, sizeof(tx)))
			return -EFAULT;
		if (tx.len > SIZE_MAX)
			return -EMSGSIZE;

		if (file->f_flags & O_NONBLOCK) {
			if (!ntb_idt_app_can_write(app))
				return -EAGAIN;
		} else {
			ret = wait_event_interruptible(app->write_wq,
						   ntb_idt_app_can_write(app));
			if (ret)
				return ret;
		}

		mutex_lock(&app->lock);
		ret = ntb_idt_app_tx_commit_locked(app, tx.len);
		mutex_unlock(&app->lock);
		return ret;
	}
	case NTB_IDT_APP_IOC_RX_WAIT: {
		struct ntb_idt_app_ioc_rx rx;

		if (file->f_flags & O_NONBLOCK) {
			if (!ntb_idt_app_can_read(app))
				return -EAGAIN;
		} else {
			ret = wait_event_interruptible(app->read_wq,
						   ntb_idt_app_can_read(app));
			if (ret)
				return ret;
		}

		spin_lock_irqsave(&app->state_lock, flags);
		if (app->removed) {
			spin_unlock_irqrestore(&app->state_lock, flags);
			return -ENODEV;
		}
		if (!app->rx_avail) {
			spin_unlock_irqrestore(&app->state_lock, flags);
			return -EAGAIN;
		}
		rx.len = app->rx_len;
		rx.seq = app->rx_seq;
		rx.reserved = 0;
		spin_unlock_irqrestore(&app->state_lock, flags);

		dma_rmb();
		if (copy_to_user(argp, &rx, sizeof(rx)))
			return -EFAULT;

		spin_lock_irqsave(&app->state_lock, flags);
		app->rx_avail = false;
		app->rx_len = 0;
		spin_unlock_irqrestore(&app->state_lock, flags);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int ntb_idt_app_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ntb_idt_app_dev *app = file->private_data;
	unsigned long region = vma->vm_pgoff;
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret;

	mutex_lock(&app->lock);

	if (app->removed) {
		ret = -ENODEV;
		goto out;
	}

	switch (region) {
	case NTB_IDT_APP_MMAP_RX:
		if (!app->rx_buf || size > PAGE_ALIGN(app->rx_size)) {
			ret = -EINVAL;
			goto out;
		}
		ret = dma_mmap_coherent(app->dma_dev, vma, app->rx_buf,
					 app->rx_dma, app->rx_size);
		break;
	case NTB_IDT_APP_MMAP_TX:
		if (!app->peer_ready || !app->peer_io ||
		    !PAGE_ALIGNED(app->peer_phys) ||
		    size > PAGE_ALIGN(app->peer_size)) {
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

static __poll_t ntb_idt_app_poll(struct file *file, poll_table *wait)
{
	struct ntb_idt_app_dev *app = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &app->read_wq, wait);
	poll_wait(file, &app->write_wq, wait);

	if (ntb_idt_app_can_read(app))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ntb_idt_app_can_write(app))
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations ntb_idt_app_fops = {
	.owner = THIS_MODULE,
	.open = ntb_idt_app_open,
	.read = ntb_idt_app_read,
	.write = ntb_idt_app_write,
	.unlocked_ioctl = ntb_idt_app_ioctl,
	.mmap = ntb_idt_app_mmap,
	.poll = ntb_idt_app_poll,
	.llseek = no_llseek,
};

static int ntb_idt_app_enable(struct ntb_idt_app_dev *app)
{
	u64 inbits;
	u64 outbits;
	int ret;

	ret = ntb_set_ctx(app->ntb, app, &ntb_idt_app_ctx_ops);
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

static void ntb_idt_app_disable(struct ntb_idt_app_dev *app)
{
	ntb_link_disable(app->ntb);
	ntb_clear_ctx(app->ntb);
}

static int ntb_idt_app_probe(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct ntb_idt_app_dev *app;
	int ret;

	if (global_app)
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
	spin_lock_init(&app->state_lock);
	init_waitqueue_head(&app->read_wq);
	init_waitqueue_head(&app->write_wq);
	INIT_WORK(&app->service_work, ntb_idt_app_service_work);

	app->miscdev.minor = MISC_DYNAMIC_MINOR;
	app->miscdev.name = DEV_NAME;
	app->miscdev.fops = &ntb_idt_app_fops;

	ret = misc_register(&app->miscdev);
	if (ret)
		goto err_free;

	global_app = app;

	ret = ntb_idt_app_enable(app);
	if (ret)
		goto err_misc;

	dev_info(&ntb->dev, "registered /dev/%s peer=%d mw=%d buffer=%u\n",
		 DEV_NAME, app->peer_idx, app->mw_idx, buffer_size);

	return 0;

err_misc:
	global_app = NULL;
	misc_deregister(&app->miscdev);
err_free:
	kfree(app);
	return ret;
}

static void ntb_idt_app_remove(struct ntb_client *client, struct ntb_dev *ntb)
{
	struct ntb_idt_app_dev *app = global_app;

	if (!app || app->ntb != ntb)
		return;

	ntb_idt_app_set_removed(app);
	ntb_idt_app_disable(app);
	cancel_work_sync(&app->service_work);
	misc_deregister(&app->miscdev);

	mutex_lock(&app->lock);
	ntb_idt_app_free_peer_mw(app);
	ntb_idt_app_free_inbound_mw(app);
	mutex_unlock(&app->lock);

	global_app = NULL;
	kfree(app);
}

static struct ntb_client ntb_idt_app_client = {
	.ops = {
		.probe = ntb_idt_app_probe,
		.remove = ntb_idt_app_remove,
	},
};

static int __init ntb_idt_app_init(void)
{
	return ntb_register_client(&ntb_idt_app_client);
}

static void __exit ntb_idt_app_exit(void)
{
	ntb_unregister_client(&ntb_idt_app_client);
}

module_init(ntb_idt_app_init);
module_exit(ntb_idt_app_exit);

MODULE_DESCRIPTION("IDT NTB userspace application channel");
MODULE_LICENSE("GPL");