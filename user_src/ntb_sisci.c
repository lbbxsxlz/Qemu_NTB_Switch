#define _GNU_SOURCE

#include "ntb_sisci.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define NTB_IDT_SISCI_DEFAULT_DEVICE "/dev/ntb_idt_sisci"

#define NTB_IDT_SISCI_MMAP_LOCAL 0U
#define NTB_IDT_SISCI_MMAP_REMOTE 1U

#define NTB_IDT_SISCI_IOC_MAGIC 'S'
#define NTB_IDT_SISCI_INFO_F_LINK_UP 1U
#define NTB_IDT_SISCI_INFO_F_LOCAL_AVAILABLE 2U
#define NTB_IDT_SISCI_INFO_F_REMOTE_CONNECTED 4U

struct ntb_idt_sisci_ioc_info {
       uint32_t local_node_id;
       uint32_t peer_node_id;
       uint32_t local_segment_id;
       uint32_t remote_segment_id;
       uint64_t local_segment_size;
       uint64_t remote_segment_size;
       uint32_t max_segments;
       uint32_t flags;
};

struct ntb_idt_sisci_ioc_segment {
       uint32_t segment_id;
       uint32_t adapter_no;
       uint64_t size;
};

struct ntb_idt_sisci_ioc_remote_segment {
       uint32_t remote_node_id;
       uint32_t segment_id;
       uint32_t adapter_no;
       uint32_t reserved;
       uint64_t timeout_ms;
       uint64_t size;
};

struct ntb_idt_sisci_ioc_barrier {
       uint32_t flags;
       uint32_t reserved;
};

#define NTB_IDT_SISCI_IOC_GET_INFO _IOR(NTB_IDT_SISCI_IOC_MAGIC, 0x01, struct ntb_idt_sisci_ioc_info)
#define NTB_IDT_SISCI_IOC_CREATE_SEGMENT _IOWR(NTB_IDT_SISCI_IOC_MAGIC, 0x02, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_PREPARE_SEGMENT _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x03, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_SET_AVAILABLE _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x04, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_SET_UNAVAILABLE _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x05, struct ntb_idt_sisci_ioc_segment)
#define NTB_IDT_SISCI_IOC_CONNECT_SEGMENT _IOWR(NTB_IDT_SISCI_IOC_MAGIC, 0x06, struct ntb_idt_sisci_ioc_remote_segment)
#define NTB_IDT_SISCI_IOC_STORE_BARRIER _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x07, struct ntb_idt_sisci_ioc_barrier)
#define NTB_IDT_SISCI_IOC_REMOVE_SEGMENT _IOW(NTB_IDT_SISCI_IOC_MAGIC, 0x08, struct ntb_idt_sisci_ioc_segment)

struct ntb_sisci_desc {
       int fd;
};

struct ntb_sisci_local_segment {
       sci_desc_t sd;
       unsigned int segment_id;
       size_t size;
};

struct ntb_sisci_remote_segment {
       sci_desc_t sd;
       unsigned int remote_node_id;
       unsigned int segment_id;
       size_t size;
};

struct ntb_sisci_map {
       sci_desc_t sd;
       void *base;
       void *addr;
       size_t map_size;
       size_t size;
       unsigned int region;
};

struct ntb_sisci_sequence {
       sci_map_t map;
};

static char ntb_sisci_device_path[PATH_MAX] = NTB_IDT_SISCI_DEFAULT_DEVICE;

static void sci_set_error(sci_error_t *error, int value)
{
       if (error)
               *error = value;
}

static void sci_set_errno(sci_error_t *error)
{
       sci_set_error(error, errno ? errno : EIO);
}

static long sci_page_size(void)
{
       long page_size = sysconf(_SC_PAGESIZE);

       if (page_size <= 0)
               return 4096;

       return page_size;
}

int SCISetDevicePath(const char *path)
{
       if (!path || !path[0]) {
               errno = EINVAL;
               return -1;
       }

       if (snprintf(ntb_sisci_device_path, sizeof(ntb_sisci_device_path),
                    "%s", path) >= (int)sizeof(ntb_sisci_device_path)) {
               errno = ENAMETOOLONG;
               return -1;
       }

       return 0;
}

void SCIInitialize(unsigned int flags, sci_error_t *error)
{
       (void)flags;
       sci_set_error(error, 0);
}

void SCITerminate(void)
{
}

void SCIOpen(sci_desc_t *sd, unsigned int flags, sci_error_t *error)
{
       sci_desc_t desc;
       int fd;

       (void)flags;
       if (!sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       fd = open(ntb_sisci_device_path, O_RDWR | O_CLOEXEC);
       if (fd < 0) {
               *sd = NULL;
               sci_set_errno(error);
               return;
       }

       desc = calloc(1, sizeof(*desc));
       if (!desc) {
               close(fd);
               *sd = NULL;
               sci_set_errno(error);
               return;
       }

       desc->fd = fd;
       *sd = desc;
       sci_set_error(error, 0);
}

void SCIClose(sci_desc_t sd, unsigned int flags, sci_error_t *error)
{
       int saved_errno = 0;

       (void)flags;
       if (!sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       if (close(sd->fd) != 0)
               saved_errno = errno;
       free(sd);
       sci_set_error(error, saved_errno);
}

void SCICreateSegment(sci_desc_t sd, sci_local_segment_t *segment,
                     unsigned int segment_id, size_t size,
                     sci_callback_t callback, void *callback_arg,
                     unsigned int flags, sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_segment request;
       sci_local_segment_t local;

       (void)callback;
       (void)callback_arg;
       (void)flags;
       if (!sd || !segment || !size) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.segment_id = segment_id;
       request.size = size;
       if (ioctl(sd->fd, NTB_IDT_SISCI_IOC_CREATE_SEGMENT, &request) != 0) {
               *segment = NULL;
               sci_set_errno(error);
               return;
       }

       local = calloc(1, sizeof(*local));
       if (!local) {
               *segment = NULL;
               sci_set_errno(error);
               return;
       }

       local->sd = sd;
       local->segment_id = segment_id;
       local->size = (size_t)request.size;
       *segment = local;
       sci_set_error(error, 0);
}

void SCIPrepareSegment(sci_local_segment_t segment, unsigned int adapter_no,
                       unsigned int flags, sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_segment request;

       (void)flags;
       if (!segment || !segment->sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.segment_id = segment->segment_id;
       request.adapter_no = adapter_no;
       request.size = segment->size;
       if (ioctl(segment->sd->fd, NTB_IDT_SISCI_IOC_PREPARE_SEGMENT,
                 &request) != 0) {
               sci_set_errno(error);
               return;
       }

       sci_set_error(error, 0);
}

void SCISetSegmentAvailable(sci_local_segment_t segment, unsigned int adapter_no,
                            unsigned int flags, sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_segment request;

       (void)flags;
       if (!segment || !segment->sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.segment_id = segment->segment_id;
       request.adapter_no = adapter_no;
       request.size = segment->size;
       if (ioctl(segment->sd->fd, NTB_IDT_SISCI_IOC_SET_AVAILABLE,
                 &request) != 0) {
               sci_set_errno(error);
               return;
       }

       sci_set_error(error, 0);
}

void SCISetSegmentUnavailable(sci_local_segment_t segment,
                              unsigned int adapter_no, unsigned int flags,
                              sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_segment request;

       (void)flags;
       if (!segment || !segment->sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.segment_id = segment->segment_id;
       request.adapter_no = adapter_no;
       request.size = segment->size;
       if (ioctl(segment->sd->fd, NTB_IDT_SISCI_IOC_SET_UNAVAILABLE,
                 &request) != 0) {
               sci_set_errno(error);
               return;
       }

       sci_set_error(error, 0);
}

void SCIRemoveSegment(sci_local_segment_t segment, unsigned int flags,
                      sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_segment request;

       (void)flags;
       if (!segment) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.segment_id = segment->segment_id;
       request.size = segment->size;
       if (segment->sd && ioctl(segment->sd->fd, NTB_IDT_SISCI_IOC_REMOVE_SEGMENT,
                                &request) != 0 && errno != ENOENT) {
               sci_set_errno(error);
               free(segment);
               return;
       }

       free(segment);
       sci_set_error(error, 0);
}

void SCIConnectSegment(sci_desc_t sd, sci_remote_segment_t *segment,
                       unsigned int remote_node_id, unsigned int segment_id,
                       unsigned int adapter_no, sci_callback_t callback,
                       void *callback_arg, uint64_t timeout_ms,
                       unsigned int flags, sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_remote_segment request;
       sci_remote_segment_t remote;

       (void)callback;
       (void)callback_arg;
       (void)flags;
       if (!sd || !segment) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.remote_node_id = remote_node_id;
       request.segment_id = segment_id;
       request.adapter_no = adapter_no;
       request.timeout_ms = timeout_ms;
       if (ioctl(sd->fd, NTB_IDT_SISCI_IOC_CONNECT_SEGMENT, &request) != 0) {
               *segment = NULL;
               sci_set_errno(error);
               return;
       }

       remote = calloc(1, sizeof(*remote));
       if (!remote) {
               *segment = NULL;
               sci_set_errno(error);
               return;
       }

       remote->sd = sd;
       remote->remote_node_id = remote_node_id;
       remote->segment_id = segment_id;
       remote->size = (size_t)request.size;
       *segment = remote;
       sci_set_error(error, 0);
}

void SCIDisconnectSegment(sci_remote_segment_t segment, unsigned int flags,
                          sci_error_t *error)
{
       (void)flags;
       if (!segment) {
               sci_set_error(error, EINVAL);
               return;
       }

       free(segment);
       sci_set_error(error, 0);
}

static void sci_map_segment(sci_desc_t sd, unsigned int region, sci_map_t *map,
                            size_t offset, size_t size, size_t segment_size,
                            void *suggested_addr, int prot,
                            sci_error_t *error)
{
       sci_map_t mapped;
       size_t map_size;
       void *base;
       off_t mmap_offset;

       if (!sd || !map || !size || offset > segment_size ||
           size > segment_size - offset) {
               sci_set_error(error, EINVAL);
               return;
       }

       map_size = offset + size;
       mmap_offset = (off_t)region * sci_page_size();
       base = mmap(suggested_addr, map_size, prot, MAP_SHARED, sd->fd,
                   mmap_offset);
       if (base == MAP_FAILED) {
               *map = NULL;
               sci_set_errno(error);
               return;
       }

       mapped = calloc(1, sizeof(*mapped));
       if (!mapped) {
               munmap(base, map_size);
               *map = NULL;
               sci_set_errno(error);
               return;
       }

       mapped->sd = sd;
       mapped->base = base;
       mapped->addr = (unsigned char *)base + offset;
       mapped->map_size = map_size;
       mapped->size = size;
       mapped->region = region;
       *map = mapped;
       sci_set_error(error, 0);
}

void SCIMapLocalSegment(sci_local_segment_t segment, sci_map_t *map,
                        size_t offset, size_t size, void *suggested_addr,
                        unsigned int flags, sci_error_t *error)
{
       (void)flags;
       if (!segment) {
               sci_set_error(error, EINVAL);
               return;
       }

              if (!size)
                     size = segment->size - offset;
       sci_map_segment(segment->sd, NTB_IDT_SISCI_MMAP_LOCAL, map, offset,
                       size, segment->size, suggested_addr,
                       PROT_READ | PROT_WRITE, error);
}

void SCIMapRemoteSegment(sci_remote_segment_t segment, sci_map_t *map,
                         size_t offset, size_t size, void *suggested_addr,
                         unsigned int flags, sci_error_t *error)
{
       (void)flags;
       if (!segment) {
               sci_set_error(error, EINVAL);
               return;
       }

              if (!size)
                     size = segment->size - offset;
       sci_map_segment(segment->sd, NTB_IDT_SISCI_MMAP_REMOTE, map, offset,
                       size, segment->size, suggested_addr,
                       PROT_READ | PROT_WRITE, error);
}

void *SCIMapAddr(sci_map_t map)
{
       return map ? map->addr : NULL;
}

size_t SCIMapSize(sci_map_t map)
{
       return map ? map->size : 0;
}

void SCIUnmapSegment(sci_map_t map, unsigned int flags, sci_error_t *error)
{
       int saved_errno = 0;

       (void)flags;
       if (!map) {
               sci_set_error(error, EINVAL);
               return;
       }

       if (munmap(map->base, map->map_size) != 0)
               saved_errno = errno;
       free(map);
       sci_set_error(error, saved_errno);
}

void SCICreateMapSequence(sci_map_t map, sci_sequence_t *sequence,
                          unsigned int flags, sci_error_t *error)
{
       sci_sequence_t seq;

       (void)flags;
       if (!map || !sequence) {
               sci_set_error(error, EINVAL);
               return;
       }

       seq = calloc(1, sizeof(*seq));
       if (!seq) {
               *sequence = NULL;
               sci_set_errno(error);
               return;
       }

       seq->map = map;
       *sequence = seq;
       sci_set_error(error, 0);
}

void SCIStartSequence(sci_sequence_t sequence, unsigned int flags,
                      sci_error_t *error)
{
       (void)flags;
       sci_set_error(error, sequence ? 0 : EINVAL);
}

void SCIStoreBarrier(sci_sequence_t sequence, unsigned int flags,
                     sci_error_t *error)
{
       struct ntb_idt_sisci_ioc_barrier request;

       if (!sequence || !sequence->map || !sequence->map->sd) {
               sci_set_error(error, EINVAL);
               return;
       }

       memset(&request, 0, sizeof(request));
       request.flags = flags;
       if (ioctl(sequence->map->sd->fd, NTB_IDT_SISCI_IOC_STORE_BARRIER,
                 &request) != 0) {
               sci_set_errno(error);
               return;
       }

       sci_set_error(error, 0);
}

void SCIFlush(sci_sequence_t sequence, unsigned int flags, sci_error_t *error)
{
       SCIStoreBarrier(sequence, flags, error);
}

void SCICheckSequence(sci_sequence_t sequence, unsigned int flags,
                      sci_error_t *error)
{
       (void)flags;
       sci_set_error(error, sequence ? 0 : EINVAL);
}

void SCIRemoveSequence(sci_sequence_t sequence, unsigned int flags,
                       sci_error_t *error)
{
       (void)flags;
       if (!sequence) {
               sci_set_error(error, EINVAL);
               return;
       }

       free(sequence);
       sci_set_error(error, 0);
}