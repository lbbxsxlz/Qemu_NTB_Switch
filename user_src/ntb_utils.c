#define _GNU_SOURCE

#include "ntb_utils.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(struct ntb_profile_ring_header) == 64, "unexpected ring header size");
_Static_assert(sizeof(struct ntb_profile_ring_slot) == 64, "unexpected ring slot header size");

void ntb_print_errno(const char *what, const char *path)
{
	if (path)
		fprintf(stderr, "error: %s '%s': %s\n", what, path, strerror(errno));
	else
		fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
}

int ntb_run_program(char *const argv[])
{
	int status;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = ECHILD;
		return -1;
	}
	return 0;
}

void ntb_try_modprobe(const char *module_name, bool no_modprobe)
{
	char *modprobe_argv[] = { "modprobe", (char *)module_name, NULL };

	if (!no_modprobe)
		(void)ntb_run_program(modprobe_argv);
}

long ntb_page_size(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	if (page_size <= 0)
		return 4096;
	return page_size;
}

int ntb_wait_fd(int fd, short events, int timeout_sec, const char *what)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	do {
		ret = poll(&pfd, 1, timeout_sec * 1000);
	} while (ret < 0 && errno == EINTR);
	if (ret == 0) {
		fprintf(stderr, "error: timed out waiting to %s\n", what);
		return -1;
	}
	if (ret < 0) {
		ntb_print_errno(what, NULL);
		return -1;
	}
	if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		fprintf(stderr, "error: device event while waiting to %s: revents=0x%x\n",
			what, pfd.revents);
		return -1;
	}
	if (!(pfd.revents & events)) {
		fprintf(stderr, "error: unexpected poll event while waiting to %s: revents=0x%x\n",
			what, pfd.revents);
		return -1;
	}
	return 0;
}

double ntb_monotonic_sec(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

uint64_t ntb_sec_to_ns(double seconds)
{
	if (seconds <= 0.0)
		return 0;
	if (seconds >= (double)UINT64_MAX / 1000000000.0)
		return UINT64_MAX;
	return (uint64_t)(seconds * 1000000000.0 + 0.5);
}

int ntb_parse_size_arg(const char *text, uint64_t *value)
{
	char *end;
	uint64_t multiplier = 1;
	unsigned long long base;

	errno = 0;
	base = strtoull(text, &end, 0);
	if (errno != 0 || end == text)
		return -1;
	while (isspace((unsigned char)*end))
		end++;
	if (*end != '\0') {
		switch (tolower((unsigned char)*end)) {
		case 'k':
			multiplier = 1024ULL;
			end++;
			break;
		case 'm':
			multiplier = 1024ULL * 1024ULL;
			end++;
			break;
		case 'g':
			multiplier = 1024ULL * 1024ULL * 1024ULL;
			end++;
			break;
		default:
			return -1;
		}
		if (tolower((unsigned char)end[0]) == 'i')
			end++;
		if (tolower((unsigned char)end[0]) == 'b')
			end++;
		while (isspace((unsigned char)*end))
			end++;
		if (*end != '\0')
			return -1;
	}
	if (base > UINT64_MAX / multiplier)
		return -1;
	*value = (uint64_t)base * multiplier;
	return 0;
}

int ntb_parse_int_arg(const char *text, int min_value, int max_value,
			      const char *name, int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' || parsed < min_value ||
	    parsed > max_value) {
		fprintf(stderr, "error: invalid %s: %s\n", name, text);
		return -1;
	}
	*value = (int)parsed;
	return 0;
}

uint32_t ntb_ring_load_u32(uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void ntb_ring_store_u32(uint32_t *ptr, uint32_t value)
{
	__atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

uint64_t ntb_ring_load_u64(uint64_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void ntb_ring_store_u64(uint64_t *ptr, uint64_t value)
{
	__atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

void ntb_ring_wait_pause(unsigned int *spins)
{
	if (((*spins)++ & 0xfffU) == 0)
		sched_yield();
}

int ntb_ring_check_timeout(double last_progress, int timeout_sec,
				   const char *what)
{
	if (ntb_monotonic_sec() - last_progress < (double)timeout_sec)
		return 0;
	fprintf(stderr, "error: timed out waiting to %s\n", what);
	return -1;
}

size_t ntb_ring_max_slot_size(size_t capacity)
{
	if (capacity <= sizeof(struct ntb_profile_ring_header) +
	    sizeof(struct ntb_profile_ring_slot))
		return 0;
	return capacity - sizeof(struct ntb_profile_ring_header) -
	       sizeof(struct ntb_profile_ring_slot);
}

uint32_t ntb_ring_calc_slot_count(size_t capacity, size_t slot_size)
{
	size_t stride;
	size_t slots;

	if (!slot_size || capacity <= sizeof(struct ntb_profile_ring_header))
		return 0;
	stride = sizeof(struct ntb_profile_ring_slot) + slot_size;
	slots = (capacity - sizeof(struct ntb_profile_ring_header)) / stride;
	if (slots > UINT32_MAX)
		slots = UINT32_MAX;
	return (uint32_t)slots;
}

struct ntb_profile_ring_slot *ntb_ring_slot(struct ntb_profile_ring_header *hdr,
						    uint64_t index)
{
	unsigned char *base = (unsigned char *)hdr;
	size_t stride = (size_t)hdr->slot_header_size + hdr->slot_size;

	return (struct ntb_profile_ring_slot *)(base + hdr->header_size +
						 (index % hdr->slot_count) * stride);
}

unsigned char *ntb_ring_slot_payload(struct ntb_profile_ring_header *hdr,
					     struct ntb_profile_ring_slot *slot)
{
	return (unsigned char *)slot + hdr->slot_header_size;
}