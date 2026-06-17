// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s ADDRESS [WIDTH [VALUE]]\n", prog);
	fprintf(stderr, "only 32-bit accesses are supported\n");
}

int main(int argc, char **argv)
{
	unsigned long long addr, base;
	unsigned long width = 32;
	long page_size;
	size_t offset;
	int write_access, fd;
	void *map;
	volatile uint32_t *reg;

	if (argc < 2 || argc > 4) {
		usage(argv[0]);
		return 2;
	}

	addr = strtoull(argv[1], NULL, 0);
	if (argc >= 3)
		width = strtoul(argv[2], NULL, 0);
	if (width != 32) {
		fprintf(stderr, "unsupported width: %lu\n", width);
		return 2;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		perror("sysconf");
		return 1;
	}

	base = addr & ~((unsigned long long)page_size - 1);
	offset = (size_t)(addr - base);
	write_access = (argc == 4);

	fd = open("/dev/mem", write_access ? O_RDWR | O_SYNC : O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	map = mmap(NULL, (size_t)page_size, write_access ? (PROT_READ | PROT_WRITE) : PROT_READ,
		   MAP_SHARED, fd, (off_t)base);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap 0x%llx: %s\n", base, strerror(errno));
		close(fd);
		return 1;
	}

	reg = (volatile uint32_t *)((char *)map + offset);
	if (write_access) {
		*reg = (uint32_t)strtoul(argv[3], NULL, 0);
		__sync_synchronize();
	}

	printf("0x%08x\n", *reg);

	munmap(map, (size_t)page_size);
	close(fd);
	return 0;
}
