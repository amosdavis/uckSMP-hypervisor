/*
 * uck_test.c - Test program for UCK shared memory
 *
 * Demonstrates two nodes sharing memory:
 *   Node 1 (owner): writes data to shared region
 *   Node 2 (reader): reads data from shared region via page faults
 *
 * Usage:
 *   # On node1 (owner):
 *   ./uck_test write 1
 *
 *   # On node2 (reader):
 *   ./uck_test read 2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>

#include "uck.h"

#define REGION_ID    1
#define REGION_SIZE  (64 * 1024 * 1024)  /* 64 MB */
#define TEST_PATTERN 0xDEADBEEF

int main(int argc, char *argv[])
{
	int fd;
	void *mem;
	int mode;       /* 0=read, 1=write */
	int node_id;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <read|write> <node_id>\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "write") == 0)
		mode = 1;
	else if (strcmp(argv[1], "read") == 0)
		mode = 0;
	else {
		fprintf(stderr, "Unknown mode: %s\n", argv[1]);
		return 1;
	}
	node_id = atoi(argv[2]);

	fd = open("/dev/uck", O_RDWR);
	if (fd < 0) {
		perror("open /dev/uck");
		return 1;
	}

	/*
	 * mmap the shared region.
	 * offset = region_id * PAGE_SIZE (region_id goes into vm_pgoff)
	 */
	mem = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, (off_t)REGION_ID * getpagesize());
	if (mem == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	printf("uck_test: mapped region %d at %p (%d MB)\n",
	       REGION_ID, mem, REGION_SIZE / (1024 * 1024));

	if (mode) {
		/* Writer: fill pages with a pattern */
		printf("uck_test: writing pattern 0x%X to %d pages...\n",
		       TEST_PATTERN,
		       REGION_SIZE / getpagesize());

		struct timespec start, end;
		clock_gettime(CLOCK_MONOTONIC, &start);

		unsigned int *p = (unsigned int *)mem;
		size_t count = REGION_SIZE / sizeof(unsigned int);
		for (size_t i = 0; i < count; i++)
			p[i] = TEST_PATTERN + (unsigned int)i;

		clock_gettime(CLOCK_MONOTONIC, &end);
		double elapsed = (end.tv_sec - start.tv_sec) +
				 (end.tv_nsec - start.tv_nsec) / 1e9;

		printf("uck_test: wrote %zu MB in %.3f seconds (%.1f MB/s)\n",
		       REGION_SIZE / (1024 * 1024), elapsed,
		       (REGION_SIZE / (1024.0 * 1024.0)) / elapsed);

		/* Keep running so the pages stay available */
		printf("uck_test: data written. Press Ctrl+C to stop.\n");
		pause();
	} else {
		/* Reader: read pages (triggers remote page faults) */
		printf("uck_test: reading from shared region...\n");
		printf("uck_test: (each page access may trigger a network fetch)\n");

		/* Wait a moment for writer to populate */
		sleep(2);

		struct timespec start, end;
		clock_gettime(CLOCK_MONOTONIC, &start);

		unsigned int *p = (unsigned int *)mem;
		size_t count = REGION_SIZE / sizeof(unsigned int);
		int errors = 0;
		for (size_t i = 0; i < count; i++) {
			unsigned int expected = TEST_PATTERN + (unsigned int)i;
			if (p[i] != expected) {
				if (errors < 10)
					printf("uck_test: MISMATCH at [%zu]: "
					       "got 0x%X expected 0x%X\n",
					       i, p[i], expected);
				errors++;
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &end);
		double elapsed = (end.tv_sec - start.tv_sec) +
				 (end.tv_nsec - start.tv_nsec) / 1e9;

		printf("uck_test: read %zu MB in %.3f seconds (%.1f MB/s)\n",
		       REGION_SIZE / (1024 * 1024), elapsed,
		       (REGION_SIZE / (1024.0 * 1024.0)) / elapsed);

		if (errors == 0)
			printf("uck_test: ALL DATA CORRECT! "
			       "Remote memory access works!\n");
		else
			printf("uck_test: %d errors found\n", errors);
	}

	munmap(mem, REGION_SIZE);
	close(fd);
	return 0;
}
