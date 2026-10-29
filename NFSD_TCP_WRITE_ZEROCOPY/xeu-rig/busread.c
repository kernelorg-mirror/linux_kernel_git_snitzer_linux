/*
 * busread: what one count of bus_access_rd is on this core.  Reads N MiB
 * once, a line at a time, after pushing it out of the caches with 1 GiB of
 * other memory, and counts bus_access_rd (0x60), l2d_cache_refill (0x17)
 * and ll_cache_miss_rd (0x37) around the read loop alone.
 */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int open_raw(uint64_t code, int group)
{
	struct perf_event_attr a;

	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.type = PERF_TYPE_RAW;
	a.config = code;
	a.disabled = group == -1;
	a.exclude_hv = 1;
	return syscall(SYS_perf_event_open, &a, 0, -1, group, 0);
}

int main(int argc, char **argv)
{
	size_t n = (size_t)atoi(argv[1]) << 20, i;
	static const uint64_t code[3] = { 0x60, 0x17, 0x37 };
	static const char * const name[3] = { "bus_access_rd", "l2d_cache_refill", "ll_cache_miss_rd" };
	volatile uint64_t s = 0;
	uint64_t v[3];
	int fd[3], k;
	uint8_t *a = aligned_alloc(65536, n), *f = aligned_alloc(65536, 1UL << 30);

	memset(a, 1, n);
	memset(f, 2, 1UL << 30);
	for (i = 0; i < (1UL << 30); i += 64)
		s += f[i];
	fd[0] = open_raw(code[0], -1);
	for (k = 1; k < 3; k++)
		fd[k] = open_raw(code[k], fd[0]);
	if (fd[0] < 0 || fd[1] < 0 || fd[2] < 0) { perror("perf_event_open"); return 1; }
	ioctl(fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
	for (i = 0; i < n; i += 64)
		s += a[i];
	ioctl(fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
	printf("read %zu MiB, %zu lines:", n >> 20, n / 64);
	for (k = 0; k < 3; k++) {
		read(fd[k], &v[k], sizeof(v[k]));
		printf(" %s %llu (%.3f a line)", name[k], (unsigned long long)v[k], (double)v[k] / (n / 64));
	}
	printf("\n");
	return 0;
}
