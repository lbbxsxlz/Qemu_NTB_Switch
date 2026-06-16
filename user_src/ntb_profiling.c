#define _GNU_SOURCE

#include "ntb_profiling.h"
#include "ntb_sisci.h"
#include "ntb_utils.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DEFAULT_BACKEND "generic"
#define DEFAULT_SEGMENT_ID 1U
#define DEFAULT_WINDOW_SIZE (4ULL * 1024ULL * 1024ULL)
#define DEFAULT_TOTAL_SIZE (64ULL * 1024ULL * 1024ULL)
#define DEFAULT_CHUNK_SIZE (1ULL * 1024ULL * 1024ULL)
#define DEFAULT_TIMEOUT_SEC 30

#define NTB_SISCI_DEVICE_PATH "/dev/ntb_idt_sisci"
#define NTB_SISCI_MODULE_NAME "ntb-idt-sisci-app"

struct ntb_sisci_profile_endpoint {
	sci_desc_t sd;
	sci_local_segment_t local_segment;
	sci_remote_segment_t remote_segment;
	sci_map_t map;
	sci_sequence_t sequence;
	bool local;
	bool available;
};

static void die_sci(const char *what, sci_error_t error)
{
	fprintf(stderr, "error: %s: %s\n", what, strerror(error ? error : EIO));
}

static int open_sisci_desc(const struct ntb_profile_options *opt, sci_desc_t *sd)
{
	sci_error_t error;

	if (SCISetDevicePath(opt->device_path) != 0) {
		fprintf(stderr, "error: set SISCI device path '%s': %s\n",
			opt->device_path, strerror(errno));
		return -1;
	}
	SCIInitialize(SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCIInitialize", error);
		return -1;
	}
	SCIOpen(sd, SCI_FLAG_EMPTY, &error);
	if (error) {
		ntb_try_modprobe(NTB_SISCI_MODULE_NAME, opt->no_modprobe);
		SCIOpen(sd, SCI_FLAG_EMPTY, &error);
	}
	if (error) {
		die_sci("SCIOpen", error);
		fprintf(stderr,
			"hint: load ntb-idt-sisci-app on both VMs after unloading ntb_tool, ntb_perf, ntb_pingpong, and ntb_idt_app\n");
		SCITerminate();
		return -1;
	}
	return 0;
}

static void sisci_endpoint_close(struct ntb_profile_endpoint *endpoint)
{
	struct ntb_sisci_profile_endpoint *ctx = endpoint->ctx;
	sci_error_t error;

	if (!ctx)
		return;
	if (ctx->local && ctx->available)
		SCISetSegmentUnavailable(ctx->local_segment, 0, SCI_FLAG_EMPTY, &error);
	if (ctx->sequence)
		SCIRemoveSequence(ctx->sequence, SCI_FLAG_EMPTY, &error);
	if (ctx->map)
		SCIUnmapSegment(ctx->map, SCI_FLAG_EMPTY, &error);
	if (ctx->remote_segment)
		SCIDisconnectSegment(ctx->remote_segment, SCI_FLAG_EMPTY, &error);
	if (ctx->local_segment)
		SCIRemoveSegment(ctx->local_segment, SCI_FLAG_EMPTY, &error);
	if (ctx->sd)
		SCIClose(ctx->sd, SCI_FLAG_EMPTY, &error);
	SCITerminate();
	free(ctx);
	memset(endpoint, 0, sizeof(*endpoint));
}

static int sisci_open_local(const struct ntb_profile_options *opt,
			    struct ntb_profile_endpoint *endpoint)
{
	struct ntb_sisci_profile_endpoint *ctx;
	sci_error_t error;

	memset(endpoint, 0, sizeof(*endpoint));
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	ctx->local = true;
	endpoint->ctx = ctx;
	if (open_sisci_desc(opt, &ctx->sd) != 0)
		goto fail;
	SCICreateSegment(ctx->sd, &ctx->local_segment, opt->segment_id,
			 (size_t)opt->window_size, NULL, NULL, SCI_FLAG_EMPTY,
			 &error);
	if (error) {
		die_sci("SCICreateSegment", error);
		goto fail;
	}
	SCIPrepareSegment(ctx->local_segment, 0, SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCIPrepareSegment", error);
		goto fail;
	}
	SCIMapLocalSegment(ctx->local_segment, &ctx->map, 0, 0, NULL,
			   SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCIMapLocalSegment", error);
		goto fail;
	}
	endpoint->addr = SCIMapAddr(ctx->map);
	endpoint->size = SCIMapSize(ctx->map);
	return 0;
fail:
	sisci_endpoint_close(endpoint);
	return -1;
}

static int sisci_publish_local(struct ntb_profile_endpoint *endpoint)
{
	struct ntb_sisci_profile_endpoint *ctx = endpoint->ctx;
	sci_error_t error;

	if (!ctx || !ctx->local_segment)
		return -1;
	SCISetSegmentAvailable(ctx->local_segment, 0, SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCISetSegmentAvailable", error);
		return -1;
	}
	ctx->available = true;
	return 0;
}

static int sisci_open_remote(const struct ntb_profile_options *opt,
			     struct ntb_profile_endpoint *endpoint)
{
	struct ntb_sisci_profile_endpoint *ctx;
	sci_error_t error;

	memset(endpoint, 0, sizeof(*endpoint));
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;
	endpoint->ctx = ctx;
	if (open_sisci_desc(opt, &ctx->sd) != 0)
		goto fail;
	SCIConnectSegment(ctx->sd, &ctx->remote_segment, opt->peer_node,
			  opt->segment_id, 0, NULL, NULL,
			  (uint64_t)opt->timeout_sec * 1000ULL, SCI_FLAG_EMPTY,
			  &error);
	if (error) {
		die_sci("SCIConnectSegment", error);
		goto fail;
	}
	SCIMapRemoteSegment(ctx->remote_segment, &ctx->map, 0, 0, NULL,
			    SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCIMapRemoteSegment", error);
		goto fail;
	}
	SCICreateMapSequence(ctx->map, &ctx->sequence, SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCICreateMapSequence", error);
		goto fail;
	}
	endpoint->addr = SCIMapAddr(ctx->map);
	endpoint->size = SCIMapSize(ctx->map);
	return 0;
fail:
	sisci_endpoint_close(endpoint);
	return -1;
}

static int sisci_store_barrier(struct ntb_profile_endpoint *endpoint)
{
	struct ntb_sisci_profile_endpoint *ctx = endpoint->ctx;
	sci_error_t error;

	if (!ctx || !ctx->sequence)
		return -1;
	SCIStoreBarrier(ctx->sequence, SCI_FLAG_EMPTY, &error);
	if (error) {
		die_sci("SCIStoreBarrier", error);
		return -1;
	}
	return 0;
}

static const struct ntb_profile_backend ntb_sisci_backend = {
	.name = "sisci",
	.default_device_path = NTB_SISCI_DEVICE_PATH,
	.open_local = sisci_open_local,
	.publish_local = sisci_publish_local,
	.open_remote = sisci_open_remote,
	.store_barrier = sisci_store_barrier,
	.close = sisci_endpoint_close,
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s rx [options]\n"
		"       %s tx [options]\n"
		"\n"
		"options:\n"
		"  --backend MODE    backend to use: generic or sisci (default generic)\n"
		"  --dev PATH        backend character device path\n"
		"  --segment ID      SISCI segment id to publish/connect (default 1)\n"
		"  --peer-node ID    SISCI remote node id passed to SCIConnectSegment (default 0)\n"
		"  --window SIZE     SISCI local segment size for rx (default 4M)\n"
		"  --total SIZE      tx total payload bytes (default 64M)\n"
		"  --chunk SIZE      payload bytes per ring slot (default 1M)\n"
		"  --timeout SEC     connect/progress timeout (default 30)\n"
		"  --no-modprobe     do not try to load the selected backend module\n",
		prog, prog);
}

static int parse_options(int argc, char **argv, struct ntb_profile_options *opt)
{
	bool role_set = false;
	int i;

	memset(opt, 0, sizeof(*opt));
	snprintf(opt->backend_name, sizeof(opt->backend_name), "%s", DEFAULT_BACKEND);
	opt->segment_id = DEFAULT_SEGMENT_ID;
	opt->window_size = DEFAULT_WINDOW_SIZE;
	opt->total_size = DEFAULT_TOTAL_SIZE;
	opt->chunk_size = DEFAULT_CHUNK_SIZE;
	opt->timeout_sec = DEFAULT_TIMEOUT_SEC;
	if (argc < 2) {
		usage(argv[0]);
		return -1;
	}
	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if ((strcmp(arg, "rx") == 0 || strcmp(arg, "receiver") == 0) && !role_set) {
			opt->role = NTB_PROFILE_ROLE_RX;
			role_set = true;
		} else if ((strcmp(arg, "tx") == 0 || strcmp(arg, "sender") == 0) && !role_set) {
			opt->role = NTB_PROFILE_ROLE_TX;
			role_set = true;
		} else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			usage(argv[0]);
			exit(0);
		} else if ((strcmp(arg, "--backend") == 0 || strcmp(arg, "--mode") == 0) &&
			   i + 1 < argc) {
			if (snprintf(opt->backend_name, sizeof(opt->backend_name), "%s",
				     argv[++i]) >= (int)sizeof(opt->backend_name)) {
				fprintf(stderr, "error: --backend value is too long\n");
				return -1;
			}
		} else if (strcmp(arg, "--no-modprobe") == 0) {
			opt->no_modprobe = true;
		} else if ((strcmp(arg, "--dev") == 0 || strcmp(arg, "--device") == 0) &&
			   i + 1 < argc) {
			if (snprintf(opt->device_path, sizeof(opt->device_path), "%s",
				     argv[++i]) >= (int)sizeof(opt->device_path)) {
				fprintf(stderr, "error: --dev path is too long\n");
				return -1;
			}
			opt->device_path_set = true;
		} else if (strcmp(arg, "--segment") == 0 && i + 1 < argc) {
			int parsed;

			if (ntb_parse_int_arg(argv[++i], 0, INT_MAX, "segment", &parsed) != 0)
				return -1;
			opt->segment_id = (uint32_t)parsed;
		} else if (strcmp(arg, "--peer-node") == 0 && i + 1 < argc) {
			int parsed;

			if (ntb_parse_int_arg(argv[++i], 0, INT_MAX, "peer-node", &parsed) != 0)
				return -1;
			opt->peer_node = (uint32_t)parsed;
		} else if (strcmp(arg, "--timeout") == 0 && i + 1 < argc) {
			if (ntb_parse_int_arg(argv[++i], 1, 86400, "timeout",
					  &opt->timeout_sec) != 0)
				return -1;
		} else if (strcmp(arg, "--window") == 0 && i + 1 < argc) {
			if (ntb_parse_size_arg(argv[++i], &opt->window_size) != 0) {
				fprintf(stderr, "error: invalid --window value\n");
				return -1;
			}
		} else if (strcmp(arg, "--total") == 0 && i + 1 < argc) {
			if (ntb_parse_size_arg(argv[++i], &opt->total_size) != 0) {
				fprintf(stderr, "error: invalid --total value\n");
				return -1;
			}
		} else if (strcmp(arg, "--chunk") == 0 && i + 1 < argc) {
			if (ntb_parse_size_arg(argv[++i], &opt->chunk_size) != 0) {
				fprintf(stderr, "error: invalid --chunk value\n");
				return -1;
			}
		} else {
			fprintf(stderr, "error: unknown or incomplete option: %s\n", arg);
			usage(argv[0]);
			return -1;
		}
	}
	if (!role_set) {
		usage(argv[0]);
		return -1;
	}
	if (!opt->window_size || !opt->total_size || !opt->chunk_size) {
		fprintf(stderr, "error: sizes must be non-zero\n");
		return -1;
	}
	if (opt->window_size > SIZE_MAX || opt->chunk_size > SIZE_MAX ||
	    opt->chunk_size > UINT32_MAX) {
		fprintf(stderr, "error: --window must fit size_t and --chunk must be <= 4G-1\n");
		return -1;
	}
	return 0;
}

static const struct ntb_profile_backend *select_backend(struct ntb_profile_options *opt)
{
	const struct ntb_profile_backend *backend;

	if (strcasecmp(opt->backend_name, "generic") == 0)
		backend = ntb_profile_get_generic_backend();
	else if (strcasecmp(opt->backend_name, "sisci") == 0)
		backend = &ntb_sisci_backend;
	else {
		fprintf(stderr, "error: unknown backend '%s'\n", opt->backend_name);
		return NULL;
	}
	if (!opt->device_path_set)
		snprintf(opt->device_path, sizeof(opt->device_path), "%s",
			 backend->default_device_path);
	return backend;
}

static int run_rx(const struct ntb_profile_options *opt,
		  const struct ntb_profile_backend *backend)
{
	struct ntb_profile_endpoint endpoint;
	struct ntb_profile_ring_header *hdr;
	uint64_t received = 0;
	double start = 0.0;
	double last_progress;
	unsigned int spins = 0;
	size_t slot_size;
	size_t max_slot_size;
	uint32_t slot_count;
	int ret = -1;

	if (backend->open_local(opt, &endpoint) != 0)
		return -1;
	if (!endpoint.addr) {
		fprintf(stderr, "error: backend %s returned no local mapping\n", backend->name);
		goto out_close;
	}
	max_slot_size = ntb_ring_max_slot_size(endpoint.size);
	slot_size = (size_t)opt->chunk_size;
	if (!slot_size || !max_slot_size) {
		fprintf(stderr, "error: mapped local memory is too small for a profiling ring\n");
		goto out_close;
	}
	if (slot_size > max_slot_size)
		slot_size = max_slot_size;
	slot_count = ntb_ring_calc_slot_count(endpoint.size, slot_size);
	if (!slot_count) {
		fprintf(stderr, "error: mapped local memory is too small for ring slots\n");
		goto out_close;
	}
	hdr = (struct ntb_profile_ring_header *)endpoint.addr;
	ntb_ring_store_u32(&hdr->magic, 0);
	__sync_synchronize();
	memset(hdr, 0, sizeof(*hdr));
	hdr->version = NTB_PROFILE_RING_VERSION;
	hdr->header_size = sizeof(struct ntb_profile_ring_header);
	hdr->slot_header_size = sizeof(struct ntb_profile_ring_slot);
	hdr->slot_count = slot_count;
	hdr->slot_size = (uint32_t)slot_size;
	__sync_synchronize();
	ntb_ring_store_u32(&hdr->magic, NTB_PROFILE_RING_MAGIC);
	if (backend->publish_local(&endpoint) != 0)
		goto out_close;
	printf("rx: backend=%s device=%s mapped=%zu bytes slots=%u slot=%u bytes\n",
	       backend->name, opt->device_path, endpoint.size, hdr->slot_count,
	       hdr->slot_size);
	printf("rx: waiting for remote stores\n");
	last_progress = ntb_monotonic_sec();
	for (;;) {
		uint64_t tail = ntb_ring_load_u64(&hdr->tail);
		uint64_t head = ntb_ring_load_u64(&hdr->head);

		if (tail < head) {
			struct ntb_profile_ring_slot *slot = ntb_ring_slot(hdr, tail);
			uint32_t len = ntb_ring_load_u32(&slot->len);
			uint64_t seq = ntb_ring_load_u64(&slot->seq);

			if (seq != tail + 1 || len > hdr->slot_size) {
				fprintf(stderr,
					"error: invalid ring slot tail=%llu seq=%llu len=%u slot=%u\n",
					(unsigned long long)tail, (unsigned long long)seq,
					len, hdr->slot_size);
				goto out_close;
			}
			if (received == 0)
				start = ntb_monotonic_sec();
			received += len;
			ntb_ring_store_u64(&hdr->tail, tail + 1);
			last_progress = ntb_monotonic_sec();
			spins = 0;
			continue;
		}
		if ((ntb_ring_load_u32(&hdr->flags) & NTB_PROFILE_RING_FLAG_DONE) &&
		    tail == ntb_ring_load_u64(&hdr->head)) {
			double local_elapsed = start > 0.0 ? ntb_monotonic_sec() - start : 0.0;
			double tx_elapsed = (double)ntb_ring_load_u64(&hdr->elapsed_ns) /
				1000000000.0;
			double local_mib = local_elapsed > 0.0 ?
				((double)received / 1048576.0) / local_elapsed : 0.0;
			double tx_mib = tx_elapsed > 0.0 ?
				((double)ntb_ring_load_u64(&hdr->total_bytes) / 1048576.0) /
				tx_elapsed : 0.0;

			printf("rx: received %llu bytes in %.6f sec, %.2f MiB/s local\n",
			       (unsigned long long)received, local_elapsed, local_mib);
			printf("rx: sender reported %llu bytes in %.6f sec, %.2f MiB/s\n",
			       (unsigned long long)ntb_ring_load_u64(&hdr->total_bytes),
			       tx_elapsed, tx_mib);
			ret = 0;
			goto out_close;
		}
		if (ntb_ring_check_timeout(last_progress, opt->timeout_sec,
				       "receive profiling ring data") != 0)
			goto out_close;
		ntb_ring_wait_pause(&spins);
	}

out_close:
	backend->close(&endpoint);
	return ret;
}

static int run_tx(const struct ntb_profile_options *opt,
		  const struct ntb_profile_backend *backend)
{
	struct ntb_profile_endpoint endpoint;
	struct ntb_profile_ring_header *hdr;
	unsigned char *pattern;
	uint64_t sent = 0;
	double start;
	double elapsed;
	double wait_start;
	double mib_per_sec;
	unsigned int spins = 0;
	uint32_t slot_count;
	uint32_t slot_size;
	int ret = -1;

	if (backend->open_remote(opt, &endpoint) != 0)
		return -1;
	if (!endpoint.addr) {
		fprintf(stderr, "error: backend %s returned no remote mapping\n", backend->name);
		goto out_close;
	}
	hdr = (struct ntb_profile_ring_header *)endpoint.addr;
	wait_start = ntb_monotonic_sec();
	for (;;) {
		if (ntb_ring_load_u32(&hdr->magic) == NTB_PROFILE_RING_MAGIC &&
		    ntb_ring_load_u32(&hdr->version) == NTB_PROFILE_RING_VERSION &&
		    ntb_ring_load_u32(&hdr->flags) == 0 &&
		    ntb_ring_load_u64(&hdr->head) == 0 && ntb_ring_load_u64(&hdr->tail) == 0)
			break;
		if (ntb_ring_check_timeout(wait_start, opt->timeout_sec,
				       "find receiver profiling ring") != 0)
			goto out_close;
		ntb_ring_wait_pause(&spins);
	}
	slot_count = ntb_ring_load_u32(&hdr->slot_count);
	slot_size = ntb_ring_load_u32(&hdr->slot_size);
	if (!slot_count || !slot_size || opt->chunk_size > slot_size) {
		fprintf(stderr,
			"error: receiver ring slot is too small; start rx with --chunk >= tx --chunk (slot=%u)\n",
			slot_size);
		goto out_close;
	}
	pattern = malloc((size_t)opt->chunk_size);
	if (!pattern) {
		fprintf(stderr, "error: allocate tx pattern: %s\n", strerror(errno));
		goto out_close;
	}
	memset(pattern, 0xa5, (size_t)opt->chunk_size);
	printf("tx: backend=%s device=%s mapped=%zu bytes slots=%u slot=%u bytes\n",
	       backend->name, opt->device_path, endpoint.size, slot_count, slot_size);
	start = ntb_monotonic_sec();
	wait_start = start;
	while (sent < opt->total_size) {
		uint64_t head = ntb_ring_load_u64(&hdr->head);
		uint64_t tail = ntb_ring_load_u64(&hdr->tail);
		size_t payload;

		if (head - tail >= slot_count) {
			if (ntb_ring_check_timeout(wait_start, opt->timeout_sec,
					       "write free profiling ring slot") != 0)
				goto out_free_pattern;
			ntb_ring_wait_pause(&spins);
			continue;
		}
		payload = (size_t)opt->chunk_size;
		if (opt->total_size - sent < opt->chunk_size)
			payload = (size_t)(opt->total_size - sent);
		{
			struct ntb_profile_ring_slot *slot = ntb_ring_slot(hdr, head);

			memcpy(ntb_ring_slot_payload(hdr, slot), pattern, payload);
			slot->total = sent + payload;
			slot->flags = 0;
			slot->len = (uint32_t)payload;
			if (backend->store_barrier(&endpoint) != 0)
				goto out_free_pattern;
			ntb_ring_store_u64(&slot->seq, head + 1);
			ntb_ring_store_u64(&hdr->head, head + 1);
		}
		sent += payload;
		wait_start = ntb_monotonic_sec();
		spins = 0;
	}
	wait_start = ntb_monotonic_sec();
	for (;;) {
		uint64_t head = ntb_ring_load_u64(&hdr->head);
		uint64_t tail = ntb_ring_load_u64(&hdr->tail);

		if (tail == head)
			break;
		if (ntb_ring_check_timeout(wait_start, opt->timeout_sec,
				       "drain receiver profiling ring") != 0)
			goto out_free_pattern;
		ntb_ring_wait_pause(&spins);
	}
	elapsed = ntb_monotonic_sec() - start;
	mib_per_sec = elapsed > 0.0 ? ((double)sent / 1048576.0) / elapsed : 0.0;
	ntb_ring_store_u64(&hdr->total_bytes, sent);
	ntb_ring_store_u64(&hdr->elapsed_ns, ntb_sec_to_ns(elapsed));
	if (backend->store_barrier(&endpoint) != 0)
		goto out_free_pattern;
	ntb_ring_store_u32(&hdr->flags, NTB_PROFILE_RING_FLAG_DONE);
	printf("tx: wrote %llu bytes in %.6f sec, %.2f MiB/s\n",
	       (unsigned long long)sent, elapsed, mib_per_sec);
	ret = 0;

out_free_pattern:
	free(pattern);
out_close:
	backend->close(&endpoint);
	return ret;
}

int main(int argc, char **argv)
{
	struct ntb_profile_options opt;
	const struct ntb_profile_backend *backend;

	if (parse_options(argc, argv, &opt) != 0)
		return EXIT_FAILURE;
	backend = select_backend(&opt);
	if (!backend)
		return EXIT_FAILURE;
	if (opt.role == NTB_PROFILE_ROLE_RX)
		return run_rx(&opt, backend) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	return run_tx(&opt, backend) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
