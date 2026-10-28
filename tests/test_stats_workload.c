/*******************************************************************************
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 * test_stats_workload.c - known memory operations for tests/test_stats.py,
 * which runs this with DTO_COLLECT_STATS set and checks what DTO reports.
 *
 *   basic <threads> <iters> [sleep_ms]  each thread, per iteration: memcpy,
 *                                       memset, memmove, memcmp of
 *                                       SIZE_{CPY,SET,MOV,CMP} bytes
 *   churn <threads> <iters>             the same, from short-lived threads
 *                                       started one after another; prints
 *                                       the mapping count before and after
 *   fork <iters>                        memcpy SIZE_CPY in the parent and
 *                                       SIZE_CHILD in a forked child
 *
 * Built with -fno-builtin so every call reaches DTO.
 ******************************************************************************/

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* distinctive sizes, each in its own histogram bucket */
#define SIZE_CPY (131072 + 123)
#define SIZE_SET (65536 + 4096)
#define SIZE_MOV (98304 + 7)
#define SIZE_CMP (262144 + 64)
#define SIZE_CHILD (524288 + 1)
#define BUF (1 << 20)

static int iters, sleep_ms;

static void *work(void *arg)
{
	char *a = mmap(NULL, BUF, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	char *b = mmap(NULL, BUF, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	struct timespec pause = { 0, sleep_ms * 1000000L };

	for (int i = 0; i < iters; i++) {
		memcpy(b, a, SIZE_CPY);
		memset(a, i, SIZE_SET);
		memmove(b + 64, b, SIZE_MOV);
		if (memcmp(a, a + 0, SIZE_CMP))
			abort();
		if (sleep_ms)
			nanosleep(&pause, NULL);
	}
	munmap(a, BUF);
	munmap(b, BUF);
	return NULL;
}

static int count_maps(void)
{
	char line[512];
	int n = 0;
	FILE *f = fopen("/proc/self/maps", "r");

	while (f && fgets(line, sizeof(line), f))
		n++;
	if (f)
		fclose(f);
	return n;
}

static void copies(size_t size, int n)
{
	char *a = mmap(NULL, BUF, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	char *b = mmap(NULL, BUF, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

	for (int i = 0; i < n; i++)
		memcpy(b, a, size);
	munmap(a, BUF);
	munmap(b, BUF);
}

int main(int argc, char **argv)
{
	pthread_t t[256];

	if (argc < 3)
		return 2;
	if (!strcmp(argv[1], "basic")) {
		int nt = atoi(argv[2]);

		iters = atoi(argv[3]);
		sleep_ms = argc > 4 ? atoi(argv[4]) : 0;
		for (int i = 0; i < nt; i++)
			pthread_create(&t[i], NULL, work, NULL);
		for (int i = 0; i < nt; i++)
			pthread_join(t[i], NULL);
	} else if (!strcmp(argv[1], "churn")) {
		int nt = atoi(argv[2]), before, after;

		iters = atoi(argv[3]);
		/* the first thread's block and allocations, so they don't count */
		pthread_create(&t[0], NULL, work, NULL);
		pthread_join(t[0], NULL);
		before = count_maps();
		for (int i = 1; i < nt; i++) {
			pthread_create(&t[0], NULL, work, NULL);
			pthread_join(t[0], NULL);
		}
		after = count_maps();
		printf("maps_before %d maps_after %d\n", before, after);
	} else if (!strcmp(argv[1], "fork")) {
		int n = atoi(argv[2]), st;
		pid_t pid;

		copies(SIZE_CPY, n);
		pid = fork();
		if (pid == 0) {
			copies(SIZE_CHILD, n);
			exit(0);
		}
		waitpid(pid, &st, 0);
		copies(SIZE_CPY, n);
		printf("child %d\n", pid);
	}
	return 0;
}
