#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) { perror("open"); return 1; }
	size_t len = st.st_size, pages = (len + 4095) / 4096;
	void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	unsigned char *vec = malloc(pages);
	if (mincore(m, len, vec)) { perror("mincore"); return 1; }
	size_t res = 0;
	for (size_t i = 0; i < pages; i++) if (vec[i] & 1) res++;
	printf("%s: %zu of %zu pages resident (%.1f MiB of %.1f MiB, %.1f%%)\n",
	       argv[1], res, pages, res * 4096.0 / 1048576, len / 1048576.0,
	       100.0 * res / pages);
	return 0;
}
