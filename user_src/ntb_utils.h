#ifndef NTB_UTILS_H
#define NTB_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTB_PROFILE_RING_MAGIC 0x4e544250U
#define NTB_PROFILE_RING_VERSION 1U
#define NTB_PROFILE_RING_FLAG_DONE 1U

struct ntb_profile_ring_header {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t slot_header_size;
	uint32_t slot_count;
	uint32_t slot_size;
	uint32_t flags;
	uint32_t reserved;
	uint64_t head;
	uint64_t tail;
	uint64_t total_bytes;
	uint64_t elapsed_ns;
};

struct ntb_profile_ring_slot {
	uint64_t seq;
	uint64_t total;
	uint32_t len;
	uint32_t flags;
	uint8_t reserved[40];
};

void ntb_print_errno(const char *what, const char *path);
int ntb_run_program(char *const argv[]);
void ntb_try_modprobe(const char *module_name, bool no_modprobe);
long ntb_page_size(void);
int ntb_wait_fd(int fd, short events, int timeout_sec, const char *what);
double ntb_monotonic_sec(void);
uint64_t ntb_sec_to_ns(double seconds);
int ntb_parse_size_arg(const char *text, uint64_t *value);
int ntb_parse_int_arg(const char *text, int min_value, int max_value,
			      const char *name, int *value);
uint32_t ntb_ring_load_u32(uint32_t *ptr);
void ntb_ring_store_u32(uint32_t *ptr, uint32_t value);
uint64_t ntb_ring_load_u64(uint64_t *ptr);
void ntb_ring_store_u64(uint64_t *ptr, uint64_t value);
void ntb_ring_wait_pause(unsigned int *spins);
int ntb_ring_check_timeout(double last_progress, int timeout_sec,
				   const char *what);
size_t ntb_ring_max_slot_size(size_t capacity);
uint32_t ntb_ring_calc_slot_count(size_t capacity, size_t slot_size);
struct ntb_profile_ring_slot *ntb_ring_slot(struct ntb_profile_ring_header *hdr,
						    uint64_t index);
unsigned char *ntb_ring_slot_payload(struct ntb_profile_ring_header *hdr,
					     struct ntb_profile_ring_slot *slot);

#endif