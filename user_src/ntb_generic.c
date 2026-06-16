#define _GNU_SOURCE

#include "ntb_profiling.h"
#include "ntb_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NTB_GENERIC_DEVICE_PATH "/dev/ntb_idt_app"
#define NTB_GENERIC_MODULE_NAME "ntb_idt_app"

#define NTB_IDT_APP_MMAP_RX 0U
#define NTB_IDT_APP_MMAP_TX 1U

#define NTB_IDT_APP_IOC_MAGIC 'N'

struct ntb_idt_app_ioc_info {
	uint64_t rx_map_size;
	uint64_t tx_map_size;
	uint64_t rx_payload_size;
	uint64_t tx_payload_size;
	uint32_t frame_header_size;
	uint32_t reserved;
};

#define NTB_IDT_APP_IOC_GET_INFO _IOR(NTB_IDT_APP_IOC_MAGIC, 0x01, struct ntb_idt_app_ioc_info)

struct ntb_generic_endpoint {
	int fd;
	void *base;
	size_t map_size;
};

static int open_generic_device(const struct ntb_profile_options *opt)
{
	int fd;

	fd = open(opt->device_path, O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return fd;
	ntb_try_modprobe(NTB_GENERIC_MODULE_NAME, opt->no_modprobe);
	fd = open(opt->device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ntb_print_errno("open ntb_idt_app device", opt->device_path);
		fprintf(stderr,
			"hint: run 'modprobe -r ntb_tool ntb_perf ntb_pingpong 2>/dev/null || true'\n"
			"      then 'modprobe ntb_idt_app buffer_size=4194304' on both VMs\n");
	}
	return fd;
}

static int get_driver_info(int fd, struct ntb_idt_app_ioc_info *info)
{
	int ret;

	memset(info, 0, sizeof(*info));
	do {
		ret = ioctl(fd, NTB_IDT_APP_IOC_GET_INFO, info);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -1;
	if (!info->frame_header_size)
		return -1;
	return 0;
}

static int map_generic_region(int fd, unsigned int region, uint64_t map_size,
			      uint64_t payload_size, uint32_t frame_header_size,
			      int prot, struct ntb_profile_endpoint *endpoint)
{
	struct ntb_generic_endpoint *ctx;
	off_t offset;
	void *base;

	if (!map_size || !payload_size || map_size > SIZE_MAX ||
	    payload_size > SIZE_MAX || frame_header_size >= map_size) {
		errno = EMSGSIZE;
		return -1;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	offset = (off_t)region * ntb_page_size();
	base = mmap(NULL, (size_t)map_size, prot, MAP_SHARED, fd, offset);
	if (base == MAP_FAILED) {
		free(ctx);
		return -1;
	}
	ctx->fd = fd;
	ctx->base = base;
	ctx->map_size = (size_t)map_size;
	endpoint->ctx = ctx;
	endpoint->addr = (unsigned char *)base + frame_header_size;
	endpoint->size = (size_t)payload_size;
	return 0;
}

static int ntb_generic_open_local(const struct ntb_profile_options *opt,
				  struct ntb_profile_endpoint *endpoint)
{
	struct ntb_idt_app_ioc_info info;
	int fd;

	memset(endpoint, 0, sizeof(*endpoint));
	fd = open_generic_device(opt);
	if (fd < 0)
		return -1;
	if (get_driver_info(fd, &info) != 0) {
		ntb_print_errno("get ntb_idt_app info", NULL);
		close(fd);
		return -1;
	}
	if (map_generic_region(fd, NTB_IDT_APP_MMAP_RX, info.rx_map_size,
			       info.rx_payload_size, info.frame_header_size,
			       PROT_READ | PROT_WRITE, endpoint) != 0) {
		ntb_print_errno("mmap ntb_idt_app rx window", NULL);
		close(fd);
		return -1;
	}
	return 0;
}

static int ntb_generic_publish_local(struct ntb_profile_endpoint *endpoint)
{
	(void)endpoint;
	return 0;
}

static int ntb_generic_open_remote(const struct ntb_profile_options *opt,
				   struct ntb_profile_endpoint *endpoint)
{
	struct ntb_idt_app_ioc_info info;
	int fd;

	memset(endpoint, 0, sizeof(*endpoint));
	fd = open_generic_device(opt);
	if (fd < 0)
		return -1;
	if (ntb_wait_fd(fd, POLLOUT, opt->timeout_sec, "connect to ntb_idt_app peer") != 0) {
		close(fd);
		return -1;
	}
	if (get_driver_info(fd, &info) != 0) {
		ntb_print_errno("get ntb_idt_app info", NULL);
		close(fd);
		return -1;
	}
	if (map_generic_region(fd, NTB_IDT_APP_MMAP_TX, info.tx_map_size,
			       info.tx_payload_size, info.frame_header_size,
			       PROT_READ | PROT_WRITE, endpoint) != 0) {
		ntb_print_errno("mmap ntb_idt_app tx window", NULL);
		close(fd);
		return -1;
	}
	return 0;
}

static int ntb_generic_store_barrier(struct ntb_profile_endpoint *endpoint)
{
	(void)endpoint;
	__sync_synchronize();
	return 0;
}

static void ntb_generic_close(struct ntb_profile_endpoint *endpoint)
{
	struct ntb_generic_endpoint *ctx = endpoint->ctx;

	if (!ctx)
		return;
	if (ctx->base)
		munmap(ctx->base, ctx->map_size);
	if (ctx->fd >= 0)
		close(ctx->fd);
	free(ctx);
	memset(endpoint, 0, sizeof(*endpoint));
}

static const struct ntb_profile_backend ntb_generic_backend = {
	.name = "generic",
	.default_device_path = NTB_GENERIC_DEVICE_PATH,
	.open_local = ntb_generic_open_local,
	.publish_local = ntb_generic_publish_local,
	.open_remote = ntb_generic_open_remote,
	.store_barrier = ntb_generic_store_barrier,
	.close = ntb_generic_close,
};

const struct ntb_profile_backend *ntb_profile_get_generic_backend(void)
{
	return &ntb_generic_backend;
}
