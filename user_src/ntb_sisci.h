#ifndef NTB_SISCI_H
#define NTB_SISCI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCI_FLAG_EMPTY 0U
#define SCI_INFINITE_TIMEOUT UINT64_MAX

typedef int sci_error_t;
typedef void (*sci_callback_t)(void *arg, unsigned int event, void *data);

typedef struct ntb_sisci_desc *sci_desc_t;
typedef struct ntb_sisci_local_segment *sci_local_segment_t;
typedef struct ntb_sisci_remote_segment *sci_remote_segment_t;
typedef struct ntb_sisci_map *sci_map_t;
typedef struct ntb_sisci_sequence *sci_sequence_t;

int SCISetDevicePath(const char *path);

void SCIInitialize(unsigned int flags, sci_error_t *error);
void SCITerminate(void);
void SCIOpen(sci_desc_t *sd, unsigned int flags, sci_error_t *error);
void SCIClose(sci_desc_t sd, unsigned int flags, sci_error_t *error);

void SCICreateSegment(sci_desc_t sd, sci_local_segment_t *segment,
                     unsigned int segment_id, size_t size,
                     sci_callback_t callback, void *callback_arg,
                     unsigned int flags, sci_error_t *error);
void SCIPrepareSegment(sci_local_segment_t segment, unsigned int adapter_no,
                       unsigned int flags, sci_error_t *error);
void SCISetSegmentAvailable(sci_local_segment_t segment, unsigned int adapter_no,
                            unsigned int flags, sci_error_t *error);
void SCISetSegmentUnavailable(sci_local_segment_t segment,
                              unsigned int adapter_no, unsigned int flags,
                              sci_error_t *error);
void SCIRemoveSegment(sci_local_segment_t segment, unsigned int flags,
                      sci_error_t *error);

void SCIConnectSegment(sci_desc_t sd, sci_remote_segment_t *segment,
                       unsigned int remote_node_id, unsigned int segment_id,
                       unsigned int adapter_no, sci_callback_t callback,
                       void *callback_arg, uint64_t timeout_ms,
                       unsigned int flags, sci_error_t *error);
void SCIDisconnectSegment(sci_remote_segment_t segment, unsigned int flags,
                          sci_error_t *error);

void SCIMapLocalSegment(sci_local_segment_t segment, sci_map_t *map,
                        size_t offset, size_t size, void *suggested_addr,
                        unsigned int flags, sci_error_t *error);
void SCIMapRemoteSegment(sci_remote_segment_t segment, sci_map_t *map,
                         size_t offset, size_t size, void *suggested_addr,
                         unsigned int flags, sci_error_t *error);
void *SCIMapAddr(sci_map_t map);
size_t SCIMapSize(sci_map_t map);
void SCIUnmapSegment(sci_map_t map, unsigned int flags, sci_error_t *error);

void SCICreateMapSequence(sci_map_t map, sci_sequence_t *sequence,
                          unsigned int flags, sci_error_t *error);
void SCIStartSequence(sci_sequence_t sequence, unsigned int flags,
                      sci_error_t *error);
void SCIStoreBarrier(sci_sequence_t sequence, unsigned int flags,
                     sci_error_t *error);
void SCIFlush(sci_sequence_t sequence, unsigned int flags, sci_error_t *error);
void SCICheckSequence(sci_sequence_t sequence, unsigned int flags,
                      sci_error_t *error);
void SCIRemoveSequence(sci_sequence_t sequence, unsigned int flags,
                       sci_error_t *error);

#ifdef __cplusplus
}
#endif

#endif