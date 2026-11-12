// membw.c THREADS MiB_PER_THREAD SECONDS [write|copy|read]: memory bandwidth
// with THREADS threads, each on its own buffer, repeating the operation for
// SECONDS; prints the total GB/s moved (copy counts read + write bytes).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
static int secs; static size_t sz; static const char *mode;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *run(void *a) {
	long i = (long)a; cpu_set_t c; CPU_ZERO(&c); CPU_SET(8 + i % 48, &c); pthread_setaffinity_np(pthread_self(), sizeof c, &c);
	char *x = aligned_alloc(65536, sz), *y = aligned_alloc(65536, sz); memset(x, 1, sz); memset(y, 2, sz);
	double t0 = now(), t; size_t bytes = 0; volatile uint64_t sink = 0;
	while ((t = now()) - t0 < secs) {
		if (mode[0] == 'w') { memset(x, (int)bytes, sz); bytes += sz; }
		else if (mode[0] == 'c') { memcpy(y, x, sz); bytes += 2 * sz; }
		else { uint64_t s = 0; for (size_t k = 0; k < sz; k += 8) s += *(uint64_t *)(x + k); sink += s; bytes += sz; }
	}
	double *r = malloc(sizeof *r); *r = bytes / (t - t0); return r;
}
int main(int argc, char **argv) {
	int n = atoi(argv[1]); sz = (size_t)atoi(argv[2]) << 20; secs = atoi(argv[3]); mode = argc > 4 ? argv[4] : "write";
	pthread_t th[256]; double tot = 0;
	for (long i = 0; i < n; i++) pthread_create(&th[i], NULL, run, (void *)i);
	for (int i = 0; i < n; i++) { double *r; pthread_join(th[i], (void **)&r); tot += *r; }
	printf("%s: %d threads x %zu MiB: %.1f GB/s\n", mode, n, sz >> 20, tot / 1e9);
	return 0;
}
