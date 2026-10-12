// copybench: N threads each copy a large source to a large destination,
// both far larger than the caches, by one of four methods.  Reports GB/s.
//   copybench <method> <threads> <first cpu> <seconds> [MiB per buffer]
//   methods: libc  stp  zva  stnp
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static size_t SZ, PIECE, STRIDE, SOFF; static int method, secs; static volatile int go, stop;
struct th { pthread_t t; int cpu; uint64_t bytes; char *src, *dst; };

static void copy_stp(char *d, const char *s, size_t n)
{	// 64 bytes a turn with paired loads and stores, as the kernel's memcpy does
	for (; n >= 64; n -= 64, s += 64, d += 64)
		asm volatile("ldp q0, q1, [%0]\n ldp q2, q3, [%0, #32]\n stp q0, q1, [%1]\n stp q2, q3, [%1, #32]"
			     :: "r"(s), "r"(d) : "v0", "v1", "v2", "v3", "memory");
}
static void copy_zva(char *d, const char *s, size_t n)
{	// zero each destination line with DC ZVA before storing to it, so it is not fetched
	for (; n >= 64; n -= 64, s += 64, d += 64)
		asm volatile("ldp q0, q1, [%0]\n ldp q2, q3, [%0, #32]\n dc zva, %1\n stp q0, q1, [%1]\n stp q2, q3, [%1, #32]"
			     :: "r"(s), "r"(d) : "v0", "v1", "v2", "v3", "memory");
}
static void copy_stnp(char *d, const char *s, size_t n)
{	// non-temporal stores
	for (; n >= 64; n -= 64, s += 64, d += 64)
		asm volatile("ldp q0, q1, [%0]\n ldp q2, q3, [%0, #32]\n stnp q0, q1, [%1]\n stnp q2, q3, [%1, #32]"
			     :: "r"(s), "r"(d) : "v0", "v1", "v2", "v3", "memory");
}
static void *run(void *a)
{
	struct th *t = a; cpu_set_t c; CPU_ZERO(&c); CPU_SET(t->cpu, &c);
	pthread_setaffinity_np(pthread_self(), sizeof c, &c);
	t->src = mmap(0, SZ, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	t->dst = mmap(0, SZ, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	madvise(t->src, SZ, MADV_HUGEPAGE); madvise(t->dst, SZ, MADV_HUGEPAGE);
	memset(t->src, 0x5a, SZ); memset(t->dst, 1, SZ);
	__sync_fetch_and_add(&go, 1);
	while (go >= 0) ;
	while (!stop) {
		for (size_t o = 0, so = 0; o + (1 << 20) <= SZ && !stop; o += 1 << 20) {	// 1 MiB at a time, as a WRITE's payload
			if (PIECE) {	// as received: pieces of PIECE bytes, each from SOFF into its own STRIDE-byte buffer
				for (size_t d = 0; d < 1 << 20; d += PIECE, so += STRIDE) {
					size_t n = (1 << 20) - d < PIECE ? (1 << 20) - d : PIECE;
					if (so + STRIDE > SZ) so = 0;
					if (method == 0) memcpy(t->dst + o + d, t->src + so + SOFF, n);
					else if (method == 4) __builtin_memmove(t->dst + o + d, t->src + so + SOFF, n);
				}
				t->bytes += 1 << 20; continue;
			}
			switch (method) {
			case 0: memcpy(t->dst + o, t->src + o, 1 << 20); break;
			case 1: copy_stp(t->dst + o, t->src + o, 1 << 20); break;
			case 2: copy_zva(t->dst + o, t->src + o, 1 << 20); break;
			case 3: copy_stnp(t->dst + o, t->src + o, 1 << 20); break;
			}
			t->bytes += 1 << 20;
		}
	}
	return 0;
}
int main(int c, char **v)
{
	const char *m[] = {"libc", "stp", "zva", "stnp"};
	if (c < 5) return 1;
	for (method = 0; method < 4 && strcmp(m[method], v[1]); method++) ;
	PIECE = getenv("PIECE") ? atoi(getenv("PIECE")) : 0; STRIDE = getenv("STRIDE") ? atoi(getenv("STRIDE")) : 8192; SOFF = getenv("SOFF") ? atoi(getenv("SOFF")) : 68;
	int n = atoi(v[2]), cpu0 = atoi(v[3]); secs = atoi(v[4]); SZ = (size_t)(c > 5 ? atoi(v[5]) : 64) << 20;
	uint64_t z; asm("mrs %0, dczid_el0" : "=r"(z));
	if (method == 2 && ((z & 0x1f) != 4)) { fprintf(stderr, "DC ZVA block is not 64 bytes (dczid %lx)\n", z); return 1; }
	struct th *t = calloc(n, sizeof *t);
	for (int i = 0; i < n; i++) { t[i].cpu = cpu0 + i; pthread_create(&t[i].t, 0, run, &t[i]); }
	while (go < n) usleep(1000);
	struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	go = -1; sleep(secs); stop = 1;
	clock_gettime(CLOCK_MONOTONIC, &b);
	uint64_t tot = 0; for (int i = 0; i < n; i++) { pthread_join(t[i].t, 0); tot += t[i].bytes; }
	double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	if (!PIECE && memcmp(t[0].dst, t[0].src, 1 << 20)) { fprintf(stderr, "copy is wrong\n"); return 1; }
	printf("%s threads %d: %.1f GB/s\n", m[method], n, tot / s / 1e9);
	return 0;
}
