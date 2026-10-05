/*
 * odirect_pread FILE OFFSET LEN [PATTERN_MOD]
 *
 * One O_DIRECT pread() of LEN bytes at OFFSET from a 4 KiB-aligned buffer.
 * Prints "ret=<n>" (or "ret=-1 errno=<e> <strerror>") on stdout.  With
 * PATTERN_MOD, every returned byte is checked against the pattern
 * byte(i) = i % PATTERN_MOD for file position i, so a read that lands data
 * at the wrong file offset is reported as "mismatch at +<k>" even when the
 * byte count looks right.  Exit status: 0 on success, 1 on a read error,
 * 2 on a data mismatch.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s FILE OFFSET LEN [PATTERN_MOD]\n", argv[0]);
		return 64;
	}
	const char *path = argv[1];
	off_t off = atoll(argv[2]);
	size_t len = atol(argv[3]);
	long mod = argc > 4 ? atol(argv[4]) : 0;
	size_t buflen = (len + 4095) & ~(size_t)4095;
	unsigned char *buf;

	if (posix_memalign((void **)&buf, 4096, buflen)) {
		perror("posix_memalign");
		return 64;
	}
	memset(buf, 0xee, buflen);

	int fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		printf("ret=-1 errno=%d %s\n", errno, strerror(errno));
		return 1;
	}
	ssize_t ret = pread(fd, buf, len, off);
	if (ret < 0) {
		printf("ret=-1 errno=%d %s\n", errno, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	printf("ret=%zd\n", ret);
	if (mod) {
		for (ssize_t i = 0; i < ret; i++) {
			unsigned char want = (unsigned char)((off + i) % mod);
			if (buf[i] != want) {
				printf("mismatch at +%zd: got 0x%02x want 0x%02x\n",
				       i, buf[i], want);
				return 2;
			}
		}
		printf("data ok\n");
	}
	return 0;
}
