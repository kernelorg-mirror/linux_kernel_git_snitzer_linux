/* ior-hard-like interleaved writer: N ranks (threads) write RECSZ-byte
 * records O_DIRECT into one file; rank r's segment s lands at
 * offset (s*N + r)*RECSZ, so consecutive records in the file come from
 * different ranks in whatever order the threads happen to run. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int N; static long RECSZ = 47008; static double DUR; static const char *path; static long MAXREC = 0; /* per rank, 0 = unlimited */
static volatile int stop;
static struct timespec t0;
static long done_recs[4096]; static double done_at[4096]; /* per rank: records written, time of last write */
static double since_start(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; }
static void *rank(void *arg) {
	long r = (long)arg; char *buf; double topen = since_start(); int fd = open(path, O_WRONLY | O_DIRECT);
	if (fd < 0) { perror("open"); return NULL; }
	double topened = since_start();
	if (posix_memalign((void **)&buf, 4096, RECSZ)) return NULL;
	memset(buf, 'A' + (r % 26), RECSZ);
	for (long s = 0; !stop && (!MAXREC || s < MAXREC); s++) {
		off_t off = (off_t)(s * N + r) * RECSZ;
		if (pwrite(fd, buf, RECSZ, off) != RECSZ) { perror("pwrite"); break; }
		if (s == 0) fprintf(stderr, "rank %2ld: open at %.3fs took %.3fs, first pwrite done at %.3fs\n", r, topen, topened - topen, since_start());
		done_recs[r] = s + 1; done_at[r] = since_start();
	}
	close(fd); return NULL;
}
int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s FILE NRANKS SECONDS [RECSZ [MAXBYTES]]\n", argv[0]); return 1; }
	path = argv[1]; N = atoi(argv[2]); DUR = atof(argv[3]); if (argc > 4) RECSZ = atol(argv[4]);
	if (argc > 5) MAXREC = atoll(argv[5]) / RECSZ / N; /* total bytes cap, spread over ranks */
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { perror("create"); return 1; } close(fd);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	pthread_t *t = calloc(N, sizeof(*t));
	for (long r = 0; r < N; r++) pthread_create(&t[r], NULL, rank, (void *)r);
	struct timespec ts = { (time_t)DUR, (long)((DUR - (time_t)DUR) * 1e9) }; nanosleep(&ts, NULL); stop = 1;
	for (int r = 0; r < N; r++) pthread_join(t[r], NULL);
	long total = 0; double last = 0; for (int r = 0; r < N; r++) { total += done_recs[r]; if (done_at[r] > last) last = done_at[r]; }
	/* summary on stdout: time of the last completed write (the ranks stop at the byte cap or at SECONDS) */
	printf("iorhard: %d ranks wrote %ld records x %ld bytes = %.1f MiB in %.2fs (%.1f MiB/s, %.0f writes/s)\n",
	       N, total, RECSZ, total * (double)RECSZ / 1048576, last, total * (double)RECSZ / 1048576 / last, total / last);
	return 0;
}
