#ifndef NTB_PROFILING_H
#define NTB_PROFILING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum ntb_profile_role {
	NTB_PROFILE_ROLE_RX,
	NTB_PROFILE_ROLE_TX,
};

struct ntb_profile_options {
	enum ntb_profile_role role;
	char backend_name[32];
	char device_path[PATH_MAX];
	bool device_path_set;
	uint32_t segment_id;
	uint32_t peer_node;
	uint64_t window_size;
	uint64_t total_size;
	uint64_t chunk_size;
	int timeout_sec;
	bool no_modprobe;
};

struct ntb_profile_endpoint {
	void *ctx;
	unsigned char *addr;
	size_t size;
};

struct ntb_profile_backend {
	const char *name;
	const char *default_device_path;
	int (*open_local)(const struct ntb_profile_options *opt,
			  struct ntb_profile_endpoint *endpoint);
	int (*publish_local)(struct ntb_profile_endpoint *endpoint);
	int (*open_remote)(const struct ntb_profile_options *opt,
			   struct ntb_profile_endpoint *endpoint);
	int (*store_barrier)(struct ntb_profile_endpoint *endpoint);
	void (*close)(struct ntb_profile_endpoint *endpoint);
};

const struct ntb_profile_backend *ntb_profile_get_generic_backend(void);

#endif
