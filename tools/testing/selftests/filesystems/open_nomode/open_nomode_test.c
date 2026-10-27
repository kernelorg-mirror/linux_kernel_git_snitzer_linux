// SPDX-License-Identifier: GPL-2.0
/*
 * O_NOMODE versus OPENAT2_REGULAR.
 *
 * O_NOMODE (bit 30 in open()/openat() flags) asks an NFS client to create
 * a file without sending a mode, so the server picks it.  The kernel-internal
 * __O_REGULAR carrier for openat2's OPENAT2_REGULAR must not share that bit:
 *
 *  (i)  open(O_CREAT | O_NOMODE) creates a file whose mode does not depend
 *       on the mode argument, keeps O_NOMODE in F_GETFL, and does not get
 *       regular-file-only semantics (directories and FIFOs still open).
 *  (ii) openat2(OPENAT2_REGULAR | O_CREAT) behaves as upstream: it creates
 *       a regular file with the requested mode, refuses non-regular files
 *       with EFTYPE, refuses O_DIRECTORY with EINVAL, and leaks neither the
 *       carrier bit nor O_NOMODE into F_GETFL.
 *
 * O_NOMODE only changes behaviour on NFSv4 (atomic open), so run it on an
 * NFSv4.x mount.
 *
 * Usage: open_nomode_test <directory on an NFSv4 mount>
 *        (or NOMODE_TEST_DIR=<dir>).  Without a directory the test skips.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <linux/types.h>

#include "../../kselftest.h"

#ifndef O_NOMODE
#define O_NOMODE 010000000000
#endif
#ifndef OPENAT2_REGULAR
#define OPENAT2_REGULAR ((__u64)1 << 32)
#endif
#ifndef EFTYPE
#define EFTYPE 134
#endif
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
/* Kernel-internal carrier bit for OPENAT2_REGULAR; must never be visible. */
#define CARRIER_BIT (1 << 29)

struct how {
	__u64 flags;
	__u64 mode;
	__u64 resolve;
};

static char dir[4096];
static char p_nm0[4200], p_nm7[4200], p_old[4200], p_reg[4200], p_sub[4200], p_fifo[4200];

static int sys_openat2(const char *path, __u64 flags, __u64 mode)
{
	struct how how = { .flags = flags, .mode = mode };
	int fd = syscall(__NR_openat2, AT_FDCWD, path, &how, sizeof(how));

	return fd < 0 ? -errno : fd;
}

static int ofd(int fd)
{
	return fd < 0 ? -errno : fd;
}

static void cleanup(void)
{
	unlink(p_nm0);
	unlink(p_nm7);
	unlink(p_old);
	unlink(p_reg);
	unlink(p_fifo);
	rmdir(p_sub);
}

static int check_fd(int fd, const char *what, int want_nomode)
{
	struct stat st;
	int fl;

	if (fd < 0) {
		ksft_print_msg("%s: open failed: %s\n", what, strerror(-fd));
		return -1;
	}
	fl = fcntl(fd, F_GETFL);
	if (fstat(fd, &st) || fl < 0) {
		ksft_print_msg("%s: fstat/F_GETFL: %s\n", what, strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	if (!S_ISREG(st.st_mode)) {
		ksft_print_msg("%s: not a regular file (0%o)\n", what, st.st_mode);
		return -1;
	}
	if (!!(fl & O_NOMODE) != want_nomode) {
		ksft_print_msg("%s: F_GETFL 0x%x, O_NOMODE %s expected\n", what,
			       fl, want_nomode ? "" : "not");
		return -1;
	}
	if (fl & CARRIER_BIT) {
		ksft_print_msg("%s: F_GETFL 0x%x leaks the __O_REGULAR carrier\n",
			       what, fl);
		return -1;
	}
	return st.st_mode & 07777;
}

/* (i) O_CREAT|O_NOMODE: mode argument ignored, flag kept. */
static void test_nomode_create(void)
{
	int m0, m7;

	m0 = check_fd(ofd(open(p_nm0, O_CREAT | O_EXCL | O_RDWR | O_NOMODE, 0)),
		      "nomode mode 0", 1);
	m7 = check_fd(ofd(open(p_nm7, O_CREAT | O_EXCL | O_RDWR | O_NOMODE,
			       0777)), "nomode mode 0777", 1);
	if (m0 < 0 || m7 < 0) {
		ksft_test_result_fail("O_CREAT|O_NOMODE create\n");
		return;
	}
	ksft_print_msg("O_NOMODE created modes: 0%o (arg 0), 0%o (arg 0777)\n",
		       m0, m7);
	ksft_test_result(m0 == m7,
			 "O_CREAT|O_NOMODE ignores the mode argument\n");
}

static void test_nomode_create_fd(void)
{
	int fd, m;

	/* An existing file, created the ordinary way. */
	fd = open(p_old, O_CREAT | O_EXCL | O_RDWR, 0644);
	if (fd >= 0)
		close(fd);
	fd = ofd(open(p_old, O_CREAT | O_RDWR | O_NOMODE, 0600));
	m = check_fd(fd, "nomode open of existing file", 1);
	ksft_test_result(m == 0644,
			 "O_CREAT|O_NOMODE on an existing file opens it unchanged, O_NOMODE in F_GETFL (mode 0%o)\n",
			 m < 0 ? 0 : m);
}

/* (i) O_NOMODE must not carry regular-file-only semantics. */
static void test_nomode_not_regular_only(void)
{
	int fd;

	fd = open(p_sub, O_RDONLY | O_DIRECTORY | O_NOMODE);
	ksft_test_result(fd >= 0, "open(dir, O_NOMODE) succeeds (got %s)\n",
			 fd >= 0 ? "fd" : strerror(errno));
	if (fd >= 0)
		close(fd);

	if (access(p_fifo, F_OK)) {
		ksft_test_result_skip("FIFO not available on this mount\n");
		return;
	}
	fd = open(p_fifo, O_RDONLY | O_NONBLOCK | O_NOMODE);
	ksft_test_result(fd >= 0, "open(fifo, O_NOMODE) succeeds (got %s)\n",
			 fd >= 0 ? "fd" : strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* (ii) OPENAT2_REGULAR behaves as upstream. */
static void test_regular(void)
{
	int fd, m;

	fd = sys_openat2(p_reg, OPENAT2_REGULAR | O_CREAT | O_EXCL | O_RDWR,
			 0640);
	if (fd == -EINVAL || fd == -ENOSYS) {
		for (m = 0; m < 5; m++)
			ksft_test_result_skip("openat2 OPENAT2_REGULAR unsupported\n");
		return;
	}
	m = check_fd(fd, "OPENAT2_REGULAR create", 0);
	ksft_test_result(m == 0640,
			 "openat2(OPENAT2_REGULAR|O_CREAT, 0640) creates 0640 (got 0%o)\n",
			 m < 0 ? 0 : m);

	fd = sys_openat2(p_reg, OPENAT2_REGULAR | O_CREAT | O_RDWR, 0600);
	m = check_fd(fd, "OPENAT2_REGULAR existing", 0);
	ksft_test_result(m == 0640,
			 "openat2(OPENAT2_REGULAR|O_CREAT) opens an existing regular file unchanged\n");

	fd = sys_openat2(p_sub, OPENAT2_REGULAR | O_RDONLY, 0);
	ksft_test_result(fd == -EFTYPE,
			 "openat2(dir, OPENAT2_REGULAR) fails EFTYPE (got %s)\n",
			 fd >= 0 ? "fd" : strerror(-fd));
	if (fd >= 0)
		close(fd);

	fd = sys_openat2(p_sub, OPENAT2_REGULAR | O_DIRECTORY | O_RDONLY, 0);
	ksft_test_result(fd == -EINVAL,
			 "openat2(OPENAT2_REGULAR|O_DIRECTORY) fails EINVAL (got %s)\n",
			 fd >= 0 ? "fd" : strerror(-fd));
	if (fd >= 0)
		close(fd);

	if (access(p_fifo, F_OK)) {
		ksft_test_result_skip("FIFO not available on this mount\n");
		return;
	}
	fd = sys_openat2(p_fifo, OPENAT2_REGULAR | O_CREAT | O_RDONLY |
			 O_NONBLOCK, 0600);
	ksft_test_result(fd == -EFTYPE,
			 "openat2(fifo, OPENAT2_REGULAR|O_CREAT) fails EFTYPE (got %s)\n",
			 fd >= 0 ? "fd" : strerror(-fd));
	if (fd >= 0)
		close(fd);
}

int main(int argc, char **argv)
{
	const char *base = argc > 1 ? argv[1] : getenv("NOMODE_TEST_DIR");

	ksft_print_header();
	if (!base || !*base)
		ksft_exit_skip("usage: %s <dir on NFS mount> (or NOMODE_TEST_DIR)\n",
			       argv[0]);
	snprintf(dir, sizeof(dir), "%s/open_nomode.%d", base, getpid());
	if (mkdir(dir, 0755))
		ksft_exit_fail_msg("mkdir %s: %s\n", dir, strerror(errno));
	snprintf(p_nm0, sizeof(p_nm0), "%s/nomode0", dir);
	snprintf(p_nm7, sizeof(p_nm7), "%s/nomode777", dir);
	snprintf(p_old, sizeof(p_old), "%s/existing", dir);
	snprintf(p_reg, sizeof(p_reg), "%s/regular", dir);
	snprintf(p_sub, sizeof(p_sub), "%s/subdir", dir);
	snprintf(p_fifo, sizeof(p_fifo), "%s/fifo", dir);
	umask(022);
	if (mkdir(p_sub, 0755))
		ksft_exit_fail_msg("mkdir %s: %s\n", p_sub, strerror(errno));
	if (mkfifo(p_fifo, 0644))
		ksft_print_msg("mkfifo: %s (FIFO cases skip)\n", strerror(errno));

	ksft_set_plan(9);
	test_nomode_create();
	test_nomode_create_fd();
	test_nomode_not_regular_only();
	test_regular();

	cleanup();
	rmdir(dir);
	ksft_finished();
}
