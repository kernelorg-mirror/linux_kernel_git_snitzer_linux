// SPDX-License-Identifier: GPL-2.0
/*
 * sglwrite: 1 MiB O_DIRECT writes laid out as a loaned NFS WRITE's
 * payload reaches the drive (FLAMEGRAPH round 94), or as one flat buffer.
 *
 *   sglwrite <dir> <threads> <ramp s> <run s> sgl|flat
 *
 * Each thread writes its own 1 GiB file in <dir> from offset 0, one 1 MiB
 * pwritev() at a time, wrapping at 1 GiB, as an nfsd thread issues a
 * WRITE.  "sgl": the 1 MiB is 136 iovecs, one a received frame: 7,740
 * payload bytes (MTU 7792 less 52 of IP and TCP) at byte 68 of an 8 KiB
 * receive buffer (rx_ip_align 2, Ethernet 14, IP 20, TCP 32), two
 * buffers to a 16 KiB page, the last piece 3,676 bytes.  "flat": one
 * 1 MiB iovec, page-aligned, as fio writes.  After the ramp, for the run's
 * length: GB/s, each write's time (mean, p50, p99, p99.9).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MIB	(1 << 20)
#define FILESZ	(1LL << 30)
#define FRAME	7740
#define HDR	68
#define BUF	8192
#define NIOV	136
#define MAXLAT	(1 << 20)

static const char *dir;
static int sgl;
static volatile int phase;	/* 0 ramp, 1 measure, 2 stop */

struct th {
	pthread_t t;
	int id;
	long long bytes;
	int nlat;
	unsigned *lat;		/* microseconds, measured writes */
};

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *run(void *a)
{
	struct th *t = a;
	struct iovec iov[NIOV];
	char path[512];
	char *mem;
	int fd, n = 0, i;
	long long off = 0, left = MIB;

	snprintf(path, sizeof(path), "%s/sglwrite.%d", dir, t->id);
	fd = open(path, O_WRONLY | O_CREAT | O_DIRECT, 0600);
	if (fd < 0 || posix_memalign((void **)&mem, 16384, (NIOV / 2 + 1) * 16384)) {
		perror(path);
		exit(1);
	}
	memset(mem, 0x5a, (NIOV / 2 + 1) * 16384);
	if (sgl) {
		for (i = 0; i < NIOV; i++) {
			int len = left < FRAME ? left : FRAME;

			iov[i].iov_base = mem + (size_t)i * BUF + HDR;
			iov[i].iov_len = len;
			left -= len;
		}
		n = NIOV;
	} else {
		iov[0].iov_base = mem;
		iov[0].iov_len = MIB;
		n = 1;
		left = 0;
	}
	if (left) {
		fprintf(stderr, "layout short by %lld\n", left);
		exit(1);
	}
	while (phase < 2) {
		double t0 = now();
		ssize_t r = pwritev(fd, iov, n, off);

		if (r != MIB) {
			perror("pwritev");
			exit(1);
		}
		if (phase == 1) {
			t->bytes += r;
			if (t->nlat < MAXLAT)
				t->lat[t->nlat++] = (now() - t0) * 1e6;
		}
		off = (off + MIB) % FILESZ;
	}
	close(fd);
	unlink(path);
	return NULL;
}

static int cmp(const void *a, const void *b)
{
	return *(unsigned *)a > *(unsigned *)b ? 1 : *(unsigned *)a < *(unsigned *)b ? -1 : 0;
}

int main(int argc, char **argv)
{
	int nt, ramp, secs, i, nl = 0;
	struct th *t;
	unsigned *all;
	long long bytes = 0;
	double sum = 0, t0, el;

	if (argc != 6) {
		fprintf(stderr, "sglwrite <dir> <threads> <ramp s> <run s> sgl|flat\n");
		return 1;
	}
	dir = argv[1]; nt = atoi(argv[2]); ramp = atoi(argv[3]); secs = atoi(argv[4]);
	sgl = !strcmp(argv[5], "sgl");
	t = calloc(nt, sizeof(*t));
	for (i = 0; i < nt; i++) {
		t[i].id = i;
		t[i].lat = malloc(MAXLAT * sizeof(unsigned));
		pthread_create(&t[i].t, NULL, run, &t[i]);
	}
	sleep(ramp);
	t0 = now(); phase = 1;
	sleep(secs);
	phase = 2; el = now() - t0;
	for (i = 0; i < nt; i++) {
		pthread_join(t[i].t, NULL);
		bytes += t[i].bytes; nl += t[i].nlat;
	}
	all = malloc((size_t)nl * sizeof(unsigned));
	for (nl = 0, i = 0; i < nt; i++) {
		memcpy(all + nl, t[i].lat, t[i].nlat * sizeof(unsigned));
		nl += t[i].nlat;
	}
	qsort(all, nl, sizeof(unsigned), cmp);
	for (i = 0; i < nl; i++)
		sum += all[i];
	printf("%s threads %2d: %5.2f GB/s, a 1 MiB write %5.0f us mean, p50 %u, p99 %u, p99.9 %u us, %d writes\n",
	       argv[5], nt, bytes / el / 1e9, sum / nl, all[nl / 2],
	       all[(int)(nl * 0.99)], all[(int)(nl * 0.999)], nl);
	return 0;
}
