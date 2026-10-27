/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 * test_perf.c - Statically-linked A/B performance comparison.
 *
 * Both the baseline and current versions of libdto are compiled into this
 * binary as object files with renamed symbols:
 *   - Baseline: bl_memcpy, bl_memset, bl_memcmp, bl_memmove
 *   - Current:  cur_memcpy, cur_memset, cur_memcmp, cur_memmove
 *
 * For each benchmark size, tests all DTO configs (stdc, dsa, dsa_auto)
 * with both 4KB and 2MB page sizes, running interleaved A/B comparisons
 * in forked children.
 *
 * Environment variables:
 *   PERF_MIN_EFFECT   - Minimum trimmed mean change %% to FAIL (default: 2)
 *   PERF_AB_ROUNDS    - Number of interleaved A/B rounds (default: 3)
 *   PERF_SHORT        - 1 = fast sanity subset: memcpy 4k/64k/1m + memset
 *                       64k, 100 iterations, 1 round, 5%% threshold
 *   PERF_ITERS        - Iterations per A/B round (default: 10000)
 *   RESULTS_DIR       - Directory to write sample + CSV files
 *   PERF_CPU          - CPU core to pin to (default: 1)
 *   PERF_COLD_CACHE   - 1=flush per iteration (default), 0=flush once
 *   PERF_CORE_FREQ_MHZ   - Pin core frequency (MHz); 0/unset leaves it alone
 *   PERF_UNCORE_FREQ_MHZ - Pin uncore frequency (MHz); 0/unset leaves it alone
 *   PERF_OP           - Run only this op type: memcpy, memset, or memcmp
 *   PERF_SIZE         - Run only this size (e.g. 4k, 64k, 128k, 1m)
 ******************************************************************************/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dto_test_utils.h"

#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <x86intrin.h>

/* ---- Extern declarations for statically-linked DTO versions ---- */

extern void *bl_memcpy(void *dest, const void *src, size_t n);
extern void *bl_memset(void *s, int c, size_t n);
extern int   bl_memcmp(const void *s1, const void *s2, size_t n);

extern void *cur_memcpy(void *dest, const void *src, size_t n);
extern void *cur_memset(void *s, int c, size_t n);
extern int   cur_memcmp(const void *s1, const void *s2, size_t n);

/* ---- Configuration ---- */

#define WARMUP_ITERS  100
#define DEFAULT_CPU   1

static int cold_cache;
static double tsc_ghz;
static int short_mode;
static int filter_op = -1;
static size_t filter_size;

/* ---- rdtsc helpers ---- */

static inline uint64_t rdtsc_start(void)
{
	__asm__ volatile("lfence");
	return __rdtsc();
}

static inline uint64_t rdtsc_end(void)
{
	unsigned int aux;

	return __rdtscp(&aux);
}

static double calibrate_tsc(void)
{
	struct timespec t0, t1;
	uint64_t tsc0, tsc1, elapsed_ns;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	tsc0 = rdtsc_end();
	do {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1000000000ULL +
			     (t1.tv_nsec - t0.tv_nsec);
	} while (elapsed_ns < 50000000ULL);
	tsc1 = rdtsc_end();
	return (double)(tsc1 - tsc0) / (double)elapsed_ns;
}

/* ---- CPU pinning ---- */

static int pin_to_cpu(int cpu)
{
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask) < 0) {
		perror("sched_setaffinity");
		return -1;
	}
	return 0;
}

/* ---- Frequency pinning ---- */

static struct {
	int active;
	int core_pinned;
	int cpu;
	unsigned long orig_core_min_khz, orig_core_max_khz;
	char uncore_dir[256];
	unsigned long orig_uncore_min_khz, orig_uncore_max_khz;
} freq_state;

static int read_sysfs_ulong(const char *path, unsigned long *val)
{
	FILE *f = fopen(path, "r");

	if (!f) return -1;
	if (fscanf(f, "%lu", val) != 1) { fclose(f); return -1; }
	fclose(f);
	return 0;
}

static int write_sysfs_ulong(const char *path, unsigned long val)
{
	FILE *f = fopen(path, "w");

	if (!f) return -1;
	fprintf(f, "%lu", val);
	fclose(f);
	return 0;
}

static int set_core_freq(int cpu, unsigned long khz)
{
	char path[256];
	unsigned long abs_min;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_min_freq", cpu);
	if (read_sysfs_ulong(path, &abs_min) < 0) return -1;
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
	if (write_sysfs_ulong(path, abs_min) < 0) return -1;
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
	if (write_sysfs_ulong(path, khz) < 0) return -1;
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
	return write_sysfs_ulong(path, khz);
}

static void restore_freq(void)
{
	char path[512];

	if (!freq_state.active) return;
	freq_state.active = 0;
	if (freq_state.core_pinned) {
		set_core_freq(freq_state.cpu, freq_state.orig_core_max_khz);
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
			 freq_state.cpu);
		write_sysfs_ulong(path, freq_state.orig_core_min_khz);
	}

	if (freq_state.uncore_dir[0]) {
		snprintf(path, sizeof(path), "%s/min_freq_khz",
			 freq_state.uncore_dir);
		write_sysfs_ulong(path, freq_state.orig_uncore_min_khz);
		snprintf(path, sizeof(path), "%s/max_freq_khz",
			 freq_state.uncore_dir);
		write_sysfs_ulong(path, freq_state.orig_uncore_max_khz);
	}
}

static void restore_freq_and_die(int sig)
{
	restore_freq();
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Pin core and/or uncore frequency. A value of 0 for either core_mhz or
 * uncore_mhz leaves that domain untouched, so the two can be controlled
 * independently.
 */
static int pin_freq(int cpu, unsigned long core_mhz, unsigned long uncore_mhz)
{
	char path[512];
	/* atexit does not run when the process dies by signal (^C, ctest
	 * timeout kill); restore the frequency there too before dying. */
	static const int fatal_sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };

	freq_state.cpu = cpu;
	freq_state.active = 1;
	atexit(restore_freq);
	for (size_t i = 0; i < sizeof(fatal_sigs) / sizeof(fatal_sigs[0]); i++)
		signal(fatal_sigs[i], restore_freq_and_die);

	/* Pin core */
	if (core_mhz) {
		unsigned long core_khz = core_mhz * 1000;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
		if (read_sysfs_ulong(path, &freq_state.orig_core_min_khz) < 0) {
			fprintf(stderr, "ERROR: cannot read core freq for CPU %d\n", cpu);
			return -1;
		}
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
		read_sysfs_ulong(path, &freq_state.orig_core_max_khz);
		if (set_core_freq(cpu, core_khz) < 0) {
			fprintf(stderr, "ERROR: could not pin core freq to %lu MHz\n",
				core_mhz);
			return -1;
		}
		freq_state.core_pinned = 1;
	}

	/* Pin uncore */
	if (!uncore_mhz)
		return 0;

	unsigned long uncore_khz = uncore_mhz * 1000;
	unsigned long pkg = 0, die = 0;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
	read_sysfs_ulong(path, &pkg);
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/die_id", cpu);
	if (read_sysfs_ulong(path, &die) < 0) die = 0;

	snprintf(freq_state.uncore_dir, sizeof(freq_state.uncore_dir),
		 "/sys/devices/system/cpu/intel_uncore_frequency/"
		 "package_%02lu_die_%02lu", pkg, die);

	snprintf(path, sizeof(path), "%s/min_freq_khz", freq_state.uncore_dir);
	if (read_sysfs_ulong(path, &freq_state.orig_uncore_min_khz) == 0) {
		snprintf(path, sizeof(path), "%s/max_freq_khz",
			 freq_state.uncore_dir);
		read_sysfs_ulong(path, &freq_state.orig_uncore_max_khz);

		snprintf(path, sizeof(path), "%s/min_freq_khz",
			 freq_state.uncore_dir);
		write_sysfs_ulong(path, freq_state.orig_uncore_min_khz);
		snprintf(path, sizeof(path), "%s/max_freq_khz",
			 freq_state.uncore_dir);
		if (write_sysfs_ulong(path, uncore_khz) < 0) {
			fprintf(stderr, "WARNING: could not pin uncore freq\n");
			freq_state.uncore_dir[0] = '\0';
		} else {
			snprintf(path, sizeof(path), "%s/min_freq_khz",
				 freq_state.uncore_dir);
			write_sysfs_ulong(path, uncore_khz);
		}
	} else {
		fprintf(stderr, "WARNING: uncore sysfs not found\n");
		freq_state.uncore_dir[0] = '\0';
	}

	return 0;
}

static void print_freq_info(int cpu)
{
	char path[512];
	unsigned long cur_freq, min_freq, max_freq;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
	if (read_sysfs_ulong(path, &cur_freq) == 0) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
		read_sysfs_ulong(path, &min_freq);
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
		read_sysfs_ulong(path, &max_freq);
		printf("Core freq:     %lu MHz (min=%lu max=%lu)%s\n",
		       cur_freq / 1000, min_freq / 1000, max_freq / 1000,
		       (min_freq == max_freq) ? " [pinned]" : "");
	}
	if (freq_state.uncore_dir[0]) {
		snprintf(path, sizeof(path), "%s/min_freq_khz",
			 freq_state.uncore_dir);
		if (read_sysfs_ulong(path, &min_freq) == 0) {
			snprintf(path, sizeof(path), "%s/max_freq_khz",
				 freq_state.uncore_dir);
			read_sysfs_ulong(path, &max_freq);
			printf("Uncore freq:   min=%lu max=%lu MHz%s\n",
			       min_freq / 1000, max_freq / 1000,
			       (min_freq == max_freq) ? " [pinned]" : "");
		}
	}
}

/* ---- Cache flush ---- */

static inline void clflushopt_addr(volatile void *p)
{
	asm volatile("clflushopt (%0)" :: "r"(p) : "memory");
}

static void flush_buffer(void *buf, size_t size)
{
	char *p = buf;

	for (size_t i = 0; i < size; i += 64)
		clflushopt_addr(p + i);
	_mm_sfence();
}

/* ---- Benchmark definitions ---- */

enum perf_op { OP_MEMSET, OP_MEMCPY, OP_MEMCMP };

struct perf_test {
	const char *name;
	enum perf_op op;
	size_t size;
	int ref_only;
	int pf_pct;	/* page-fault probability: 0=none, 1=1%, etc. */
};

static struct perf_test benchmarks[] = {
	{"memcpy_4k",      OP_MEMCPY, 4096,    1, 0},
	{"memcpy_8k",      OP_MEMCPY, 8192,    1, 0},
	{"memcpy_16k",     OP_MEMCPY, 16384,   0, 0},
	{"memcpy_32k",     OP_MEMCPY, 32768,   0, 0},
	{"memcpy_64k",     OP_MEMCPY, 65536,   0, 0},
	{"memcpy_128k",    OP_MEMCPY, 131072,  0, 0},
	{"memcpy_256k",    OP_MEMCPY, 262144,  0, 0},
	{"memcpy_512k",    OP_MEMCPY, 524288,  0, 0},
	{"memcpy_1m",      OP_MEMCPY, 1048576, 0, 0},
	{"memcpy_1m_pf50",  OP_MEMCPY, 1048576, 0, 50},
	{"memset_64k",     OP_MEMSET, 65536,   0, 0},
	{"memset_128k",    OP_MEMSET, 131072,  0, 0},
	{"memset_256k",    OP_MEMSET, 262144,  0, 0},
	{"memset_1m",      OP_MEMSET, 1048576, 0, 0},
	{"memcmp_64k",     OP_MEMCMP, 65536,   0, 0},
	{"memcmp_128k",    OP_MEMCMP, 131072,  0, 0},
	{"memcmp_1m",      OP_MEMCMP, 1048576, 0, 0},
	{NULL, 0, 0, 0, 0}
};

#define DEFAULT_ITERS  10000

/* ---- DTO config sets ---- */

struct dto_config {
	const char *label;
	int use_libc;	/* 1 = bypass DTO, use raw libc via dlsym */
	struct { const char *key; const char *val; } envs[8];
};

static struct dto_config dto_configs[] = {
	{"cpu",      1, {{NULL, NULL}}},
	{"stdc",     0, {{"DTO_USESTDC_CALLS", "1"}, {NULL, NULL}}},
	{"dsa",      0, {{"DTO_CPU_SIZE_FRACTION", "0"},
			  {"DTO_AUTO_ADJUST_KNOBS", "0"},
			  {"DTO_MIN_BYTES", "4096"}, {NULL, NULL}}},
	{"dsa_auto", 0, {{"DTO_CPU_SIZE_FRACTION", "0"},
			  {"DTO_AUTO_ADJUST_KNOBS", "1"},
			  {"DTO_MIN_BYTES", "4096"}, {NULL, NULL}}},
};
#define NUM_DTO_CONFIGS (sizeof(dto_configs) / sizeof(dto_configs[0]))
#define CPU_CONFIG_IDX 0

/* Page size configs */
struct page_config {
	const char *label;
	int hugepage;
};

static struct page_config page_configs[] = {
	{"4k",  0},
	{"2m",  1},
};
#define NUM_PAGE_CONFIGS (sizeof(page_configs) / sizeof(page_configs[0]))

/*
 * Benchmark selection: PERF_OP/PERF_SIZE filters, plus the PERF_SHORT
 * sanity subset — memcpy 4k/64k/1m and memset 64k, no fault-injection
 * variants. Used by the run loop and both output passes so skipped
 * benchmarks disappear from the tables instead of printing as errors.
 */
static int test_selected(const struct perf_test *test)
{
	if (filter_op >= 0 && (int)test->op != filter_op)
		return 0;
	if (filter_size && test->size != filter_size)
		return 0;
	if (short_mode &&
	    !((test->op == OP_MEMCPY && test->pf_pct == 0 &&
	       (test->size == 4096 || test->size == 65536 ||
		test->size == 1048576)) ||
	      (test->op == OP_MEMSET && test->size == 65536)))
		return 0;
	return 1;
}

/* ---- Shared memory for child results ---- */

struct shared_result {
	int ok;
	int nsamples;
	int requested_iters;  /* parent sets this before fork */
	int pad;              /* keep the uint64_t sample arrays at (slot + 1)
			       * 8-byte aligned */
};

#define MAX_CHILD_SAMPLES DEFAULT_ITERS
#define SHARED_SLOT_SIZE (sizeof(struct shared_result) + \
			  2 * MAX_CHILD_SAMPLES * sizeof(uint64_t))

static uint64_t *get_bl_samples(struct shared_result *slot)
{
	return (uint64_t *)(slot + 1);
}

static uint64_t *get_cur_samples(struct shared_result *slot)
{
	return (uint64_t *)(slot + 1) + MAX_CHILD_SAMPLES;
}

/* ---- Buffer allocation ---- */

static size_t buf_alloc_size(size_t size, int hugepage)
{
	if (hugepage)
		return (size + (2 << 20) - 1) & ~((2 << 20) - 1UL);
	return size;
}

static void *alloc_shared_buffer(size_t size, int hugepage)
{
	int flags = MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE;
	size_t alloc = buf_alloc_size(size, hugepage);

	if (hugepage)
		flags |= MAP_HUGETLB | (21 << MAP_HUGE_SHIFT);

	void *p = mmap(NULL, alloc, PROT_READ | PROT_WRITE, flags, -1, 0);

	return (p == MAP_FAILED) ? NULL : p;
}

static void free_shared_buffer(void *p, size_t size, int hugepage)
{
	munmap(p, buf_alloc_size(size, hugepage));
}

/* ---- Sort ---- */

static int cmp_u64(const void *a, const void *b)
{
	uint64_t va = *(const uint64_t *)a;
	uint64_t vb = *(const uint64_t *)b;

	return (va > vb) - (va < vb);
}

static volatile int benchmark_sink;

/* Iterations per A/B round; PERF_ITERS (capped at DEFAULT_ITERS, which
 * sizes the shared result slot) or PERF_SHORT override the default. */
static int perf_iters = DEFAULT_ITERS;

/* ---- KS test ---- */

struct ks_result {
	double d;
	double p;
};

static struct ks_result ks_test(const uint64_t *a, int n1,
				const uint64_t *b, int n2)
{
	struct ks_result r = {0.0, 1.0};
	int i = 0, j = 0;
	double d_max = 0.0;

	while (i < n1 || j < n2) {
		uint64_t val;
		double diff;

		if (i < n1 && (j >= n2 || a[i] <= b[j]))
			val = a[i];
		else
			val = b[j];

		while (i < n1 && a[i] == val) i++;
		while (j < n2 && b[j] == val) j++;

		diff = fabs((double)i / n1 - (double)j / n2);
		if (diff > d_max)
			d_max = diff;
	}

	r.d = d_max;

	double ne = (double)n1 * n2 / (n1 + n2);
	double lambda = (sqrt(ne) + 0.12 + 0.11 / sqrt(ne)) * d_max;
	double sum = 0.0;

	for (int k = 1; k <= 100; k++) {
		double term = exp(-2.0 * k * k * lambda * lambda);

		if (k % 2 == 1)
			sum += term;
		else
			sum -= term;
		if (term < 1e-12)
			break;
	}
	r.p = 2.0 * sum;
	if (r.p < 0) r.p = 0;
	if (r.p > 1) r.p = 1;

	return r;
}

/* ---- CSV export ---- */

static void append_csv(const char *dir, const char *testname,
		       const char *config, const char *pagesz,
		       const char *version,
		       const uint64_t *ticks, int count, double ghz)
{
	char path[4096];
	FILE *f;
	int need_header;

	snprintf(path, sizeof(path), "%s/distributions.csv", dir);
	f = fopen(path, "r");
	need_header = (f == NULL);
	if (f) fclose(f);

	f = fopen(path, "a");
	if (!f) return;

	if (need_header)
		fprintf(f, "test,config,pagesize,version,latency_ns\n");

	for (int i = 0; i < count; i++)
		fprintf(f, "%s,%s,%s,%s,%.1f\n",
			testname, config, pagesz, version,
			(double)ticks[i] / ghz);
	fclose(f);
}

/* ---- Result for one (config, pagesize) cell in the table ---- */

struct cell_result {
	int valid;
	int skipped;	/* environment can't run this cell (no hugepages) */
	int iters;	/* iterations per round (from pilot) */
	int n_total;	/* total samples (rounds × iters) */
	int bl_outliers;/* baseline outliers (outside trimmed range) */
	int cur_outliers;
	double bl_ns;	/* baseline trimmed mean (10th-90th pctl) */
	double cur_ns;	/* current trimmed mean */
	double change;	/* percent change */
	double ks_d;
	double ks_p;
	int regression;
	int improved;
};

/* ---- Function pointer types for cpu/libc mode ---- */

typedef void *(*fn_memcpy_t)(void *, const void *, size_t);
typedef void *(*fn_memset_t)(void *, int, size_t);
typedef int   (*fn_memcmp_t)(const void *, const void *, size_t);

/* ---- Child: interleaved A/B benchmark ---- */

/*
 * Drop one page so the next access to it faults. Warns once (per child) if
 * MADV_DONTNEED is rejected — e.g. hugetlb MADV_DONTNEED is unsupported before
 * Linux ~5.18 — so a silently fault-free _pf run is visible rather than
 * masquerading as page-fault-path numbers.
 */
static void pf_drop_page(void *addr, size_t len)
{
	static int warned;

	if (madvise(addr, len, MADV_DONTNEED) != 0 && !warned) {
		warned = 1;
		fprintf(stderr,
			"WARNING: MADV_DONTNEED failed (%s); page-fault injection "
			"is not being applied on this kernel/configuration\n",
			strerror(errno));
	}
}

static void child_run_ab(struct perf_test *test,
			 struct shared_result *slot,
			 uint8_t *src, uint8_t *dst,
			 int use_libc, int hugepage)
{
	uint64_t *bl_s = get_bl_samples(slot);
	uint64_t *cur_s = get_cur_samples(slot);
	uint64_t start, end;
	int iters = slot->requested_iters;

	/*
	 * Function pointers: for cpu mode, resolve libc directly.
	 * For DTO modes, use the statically-linked bl_/cur_ symbols.
	 */
	fn_memcpy_t a_memcpy, b_memcpy;
	fn_memset_t a_memset, b_memset;
	fn_memcmp_t a_memcmp, b_memcmp;

	if (use_libc) {
		void *libc = dlopen("libc.so.6", RTLD_NOW | RTLD_NOLOAD);

		if (!libc) libc = dlopen("libc.so.6", RTLD_NOW);
		if (!libc) _exit(1);
		a_memcpy = b_memcpy = (fn_memcpy_t)dlsym(libc, "memcpy");
		a_memset = b_memset = (fn_memset_t)dlsym(libc, "memset");
		a_memcmp = b_memcmp = (fn_memcmp_t)dlsym(libc, "memcmp");
		if (!a_memcpy || !a_memset || !a_memcmp) _exit(1);
	} else {
		a_memcpy = (fn_memcpy_t)bl_memcpy;
		b_memcpy = (fn_memcpy_t)cur_memcpy;
		a_memset = (fn_memset_t)bl_memset;
		b_memset = (fn_memset_t)cur_memset;
		a_memcmp = bl_memcmp;
		b_memcmp = cur_memcmp;
	}

	if (iters < 1) iters = 1;
	if (iters > MAX_CHILD_SAMPLES)
		iters = MAX_CHILD_SAMPLES;

	for (size_t i = 0; i < test->size; i++) {
		src[i] = (uint8_t)(i & 0xFF);
		dst[i] = (uint8_t)(i & 0xFF);
        }

	/* Quick warmup phase */
	for (int w = 0; w < WARMUP_ITERS; w++) {
		switch (test->op) {
		case OP_MEMSET: a_memset(dst, 0xAA, test->size);
				b_memset(dst, 0xAA, test->size); break;
		case OP_MEMCPY: a_memcpy(dst, src, test->size);
				b_memcpy(dst, src, test->size); break;
		case OP_MEMCMP: benchmark_sink = a_memcmp(dst, src, test->size);
				benchmark_sink = b_memcmp(dst, src, test->size); break;
		}
	}

	/*
	 * Page fault injection setup. Use the mapping's actual page size:
	 * MADV_DONTNEED on a MAP_HUGETLB region must be huge-page aligned and
	 * sized, so a 4KB range would fail with EINVAL and inject nothing.
	 * num_pages is derived from the rounded-up allocation size so a buffer
	 * smaller than one huge page still yields one droppable page.
	 */
	size_t page_size = hugepage ? (2UL << 20) : (size_t)sysconf(_SC_PAGESIZE);
	size_t num_pages = buf_alloc_size(test->size, hugepage) / page_size;
	uintptr_t dst_base = (uintptr_t)dst & ~(page_size - 1);

	/* Randomized interleaved measurement */
	uint32_t rng = (uint32_t)rdtsc_end() | 1;  /* must be nonzero for xorshift */

	for (int i = 0; i < iters; i++) {
		uint64_t t_bl, t_cur;

		rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
		int bl_first = (rng & 1);

		/* First */
		if (cold_cache) {
			flush_buffer(src, test->size);
			flush_buffer(dst, test->size);
		}
		if (test->pf_pct && num_pages > 0) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			if ((rng % 100) < (uint32_t)test->pf_pct) {
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				size_t pg = rng % num_pages;
				pf_drop_page((void *)(dst_base + pg * page_size),
					     page_size);
			}
		}
		start = rdtsc_start();
		switch (test->op) {
		case OP_MEMSET: (bl_first ? a_memset : b_memset)(dst, 0xAA, test->size); break;
		case OP_MEMCPY: (bl_first ? a_memcpy : b_memcpy)(dst, src, test->size); break;
		case OP_MEMCMP: benchmark_sink = (bl_first ? a_memcmp : b_memcmp)(dst, src, test->size); break;
		}
		COMPILER_BARRIER();
		end = rdtsc_end();
		if (bl_first) t_bl = end - start; else t_cur = end - start;

		if (test->op == OP_MEMCMP)
			for (size_t j = 0; j < test->size; j++)
				dst[j] = src[j];

		/* Second */
		if (cold_cache) {
			flush_buffer(src, test->size);
			flush_buffer(dst, test->size);
		}
		if (test->pf_pct && num_pages > 0) {
			rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
			if ((rng % 100) < (uint32_t)test->pf_pct) {
				rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
				size_t pg = rng % num_pages;
				pf_drop_page((void *)(dst_base + pg * page_size),
					     page_size);
			}
		}
		start = rdtsc_start();
		switch (test->op) {
		case OP_MEMSET: (!bl_first ? a_memset : b_memset)(dst, 0xAA, test->size); break;
		case OP_MEMCPY: (!bl_first ? a_memcpy : b_memcpy)(dst, src, test->size); break;
		case OP_MEMCMP: benchmark_sink = (!bl_first ? a_memcmp : b_memcmp)(dst, src, test->size); break;
		}
		COMPILER_BARRIER();
		end = rdtsc_end();
		if (!bl_first) t_bl = end - start; else t_cur = end - start;

		bl_s[i] = t_bl;
		cur_s[i] = t_cur;

		if (test->op == OP_MEMCMP)
			for (size_t j = 0; j < test->size; j++)
				dst[j] = src[j];
	}

	qsort(bl_s, iters, sizeof(uint64_t), cmp_u64);
	qsort(cur_s, iters, sizeof(uint64_t), cmp_u64);
	slot->nsamples = iters;
	slot->ok = 1;
	_exit(0);
}

static int fork_ab(struct perf_test *test, struct shared_result *slot,
		   uint8_t *src, uint8_t *dst, struct dto_config *cfg,
		   int iters, int hugepage)
{
	pid_t pid;
	int status;

	slot->ok = 0;
	slot->requested_iters = iters;

	for (int e = 0; cfg->envs[e].key; e++)
		setenv(cfg->envs[e].key, cfg->envs[e].val, 1);

	pid = fork();
	if (pid < 0) { perror("fork"); return -1; }

	if (pid == 0)
		child_run_ab(test, slot, src, dst, cfg->use_libc, hugepage);

	waitpid(pid, &status, 0);

	for (int e = 0; cfg->envs[e].key; e++)
		unsetenv(cfg->envs[e].key);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return slot->ok ? 0 : -1;
}

static struct cell_result run_cell(struct perf_test *test,
				   struct dto_config *cfg,
				   struct page_config *pg,
				   void *shm, int ab_rounds,
				   double min_effect,
				   const char *results_dir)
{
	struct cell_result res = {0};
	int iters;
	int max_total;
	uint64_t *all_bl, *all_cur;
	int n_bl = 0, n_cur = 0;

	iters = perf_iters;
	max_total = ab_rounds * iters;

	uint8_t *src = alloc_shared_buffer(test->size, pg->hugepage);
	uint8_t *dst = alloc_shared_buffer(test->size, pg->hugepage);

	if (!src || !dst) {
		if (src) free_shared_buffer(src, test->size, pg->hugepage);
		if (dst) free_shared_buffer(dst, test->size, pg->hugepage);
		/* A failed 2MB allocation means no hugepages are reserved on
		 * this host — an environment limitation, not a regression, so
		 * skip the cell instead of failing the run. */
		res.skipped = pg->hugepage;
		return res;
	}

	all_bl = malloc(max_total * sizeof(uint64_t));
	all_cur = malloc(max_total * sizeof(uint64_t));
	if (!all_bl || !all_cur) {
		free(all_bl); free(all_cur);
		free_shared_buffer(src, test->size, pg->hugepage);
		free_shared_buffer(dst, test->size, pg->hugepage);
		return res;
	}

	for (int r = 0; r < ab_rounds; r++) {
		struct shared_result *slot = shm;

		memset(slot, 0, SHARED_SLOT_SIZE);
		if (fork_ab(test, slot, src, dst, cfg, iters, pg->hugepage) < 0)
			goto out;

		memcpy(all_bl + n_bl, get_bl_samples(slot),
		       slot->nsamples * sizeof(uint64_t));
		n_bl += slot->nsamples;
		memcpy(all_cur + n_cur, get_cur_samples(slot),
		       slot->nsamples * sizeof(uint64_t));
		n_cur += slot->nsamples;
	}

	qsort(all_bl, n_bl, sizeof(uint64_t), cmp_u64);
	qsort(all_cur, n_cur, sizeof(uint64_t), cmp_u64);

	/* Trimmed mean 10th-90th */
	{
		int blo = n_bl / 10, bhi = n_bl * 9 / 10;
		int clo = n_cur / 10, chi = n_cur * 9 / 10;

		/* With fewer than 10 samples the 10th-90th percentile window
		 * is empty, which would yield 0/0 NaN means; fall back to the
		 * untrimmed range. */
		if (bhi <= blo) { blo = 0; bhi = n_bl; }
		if (chi <= clo) { clo = 0; chi = n_cur; }

		int bn = bhi - blo, cn = chi - clo;
		double bs = 0, cs = 0;

		for (int s = blo; s < bhi; s++)
			bs += (double)all_bl[s] / tsc_ghz;
		for (int s = clo; s < chi; s++)
			cs += (double)all_cur[s] / tsc_ghz;

		res.bl_ns = bs / bn;
		res.cur_ns = cs / cn;
		res.change = ((res.cur_ns - res.bl_ns) / res.bl_ns) * 100.0;

		struct ks_result ks = ks_test(all_bl + blo, bn,
					      all_cur + clo, cn);
		res.ks_d = ks.d;
		res.ks_p = ks.p;
	}

	res.regression = (res.change > min_effect);
	res.improved = (res.change < -min_effect);
	res.iters = iters;
	res.n_total = n_bl;

	/* Count outliers using shared IQR threshold */
	{
		double q1_bl = (double)all_bl[n_bl / 4] / tsc_ghz;
		double q3_bl = (double)all_bl[3 * n_bl / 4] / tsc_ghz;
		double q1_cur = (double)all_cur[n_cur / 4] / tsc_ghz;
		double q3_cur = (double)all_cur[3 * n_cur / 4] / tsc_ghz;
		double q1 = q1_bl < q1_cur ? q1_bl : q1_cur;
		double q3 = q3_bl > q3_cur ? q3_bl : q3_cur;
		double thresh_ns = q3 + 1.5 * (q3 - q1);
		uint64_t thresh_t = (uint64_t)(thresh_ns * tsc_ghz);

		res.bl_outliers = 0;
		res.cur_outliers = 0;
		for (int s = n_bl - 1; s >= 0 && all_bl[s] > thresh_t; s--)
			res.bl_outliers++;
		for (int s = n_cur - 1; s >= 0 && all_cur[s] > thresh_t; s--)
			res.cur_outliers++;
	}
	res.valid = 1;

	/* Save CSV */
	if (results_dir) {
		append_csv(results_dir, test->name, cfg->label,
			   pg->label, "baseline", all_bl, n_bl, tsc_ghz);
		append_csv(results_dir, test->name, cfg->label,
			   pg->label, "current", all_cur, n_cur, tsc_ghz);
	}

out:
	free(all_bl);
	free(all_cur);
	free_shared_buffer(src, test->size, pg->hugepage);
	free_shared_buffer(dst, test->size, pg->hugepage);
	return res;
}

/*
 * Condensed summary: current-library speedup vs raw CPU, one table per page
 * size.
 */
static void print_speedup_summary(struct cell_result (*all_results)[NUM_DTO_CONFIGS][NUM_PAGE_CONFIGS],
				  int num_benchmarks)
{
	static const char rule[] = "=========================================="
				   "============================";

	printf("\n%s\n", rule);
	printf("Speedup vs CPU (current library)\n");
	printf("%s\n", rule);

	for (int p = 0; p < (int)NUM_PAGE_CONFIGS; p++) {
		printf("\n%s pages:\n",
		       page_configs[p].hugepage ? "2MB" : "4KB");
		printf("%-14s", "Test");
		for (int c = 1; c < (int)NUM_DTO_CONFIGS; c++)
			printf("  %10s", dto_configs[c].label);
		printf("\n");
		printf("%-14s", "--------------");
		for (int c = 1; c < (int)NUM_DTO_CONFIGS; c++)
			printf("  %10s", "----------");
		printf("\n");

		for (int b = 0; b < num_benchmarks; b++) {
			if (!test_selected(&benchmarks[b]))
				continue;
			if (benchmarks[b].ref_only)
				continue;

			struct cell_result *cpu_r =
				&all_results[b][CPU_CONFIG_IDX][p];

			if (!cpu_r->valid)
				continue;

			printf("%-14s", benchmarks[b].name);

			for (int c = 1; c < (int)NUM_DTO_CONFIGS; c++) {
				struct cell_result *r = &all_results[b][c][p];

				if (!r->valid)
					printf("  %10s", "N/A");
				else
					printf("  %9.2fx",
					       cpu_r->cur_ns / r->cur_ns);
			}
			printf("\n");
		}
	}
}

int main(void)
{
	const char *results_dir = getenv("RESULTS_DIR");
	const char *cpu_env = getenv("PERF_CPU");
	const char *effect_env = getenv("PERF_MIN_EFFECT");
	const char *rounds_env = getenv("PERF_AB_ROUNDS");
	int cpu = cpu_env ? atoi(cpu_env) : DEFAULT_CPU;
	/* PERF_SHORT=1: a fast sanity pass — memcpy 4k/64k/1m and memset 64k
	 * (no fault-injection variants), few iterations, one A/B round —
	 * sized to finish in well under a minute. Explicit PERF_ITERS/
	 * PERF_AB_ROUNDS/PERF_MIN_EFFECT still take precedence. Statistical
	 * verdicts from so few samples are indicative only. */
	const char *short_env = getenv("PERF_SHORT");
	short_mode = short_env ? atoi(short_env) : 0;
	double min_effect = effect_env ? atof(effect_env) : (short_mode ? 5.0 : 2.0);
	int ab_rounds = rounds_env ? atoi(rounds_env) : (short_mode ? 1 : 3);
	int errors = 0, failures = 0, skipped = 0;

	{
		const char *iters_env = getenv("PERF_ITERS");

		if (iters_env)
			perf_iters = atoi(iters_env);
		else if (short_mode)
			perf_iters = 100;
		if (perf_iters < 1)
			perf_iters = 1;
		if (perf_iters > DEFAULT_ITERS)
			perf_iters = DEFAULT_ITERS;
	}

	cold_cache = getenv("PERF_COLD_CACHE") ?
		     atoi(getenv("PERF_COLD_CACHE")) : 1;
	{
		const char *op_env = getenv("PERF_OP");
		if (op_env) {
			if (strcasecmp(op_env, "memcpy") == 0) filter_op = OP_MEMCPY;
			else if (strcasecmp(op_env, "memset") == 0) filter_op = OP_MEMSET;
			else if (strcasecmp(op_env, "memcmp") == 0) filter_op = OP_MEMCMP;
			else fprintf(stderr, "WARNING: unknown PERF_OP=%s, running all\n", op_env);
		}
		const char *size_env = getenv("PERF_SIZE");
		if (size_env) {
			char *end;
			filter_size = strtoul(size_env, &end, 10);
			if (*end == 'k' || *end == 'K')
				filter_size *= 1024;
			else if (*end == 'm' || *end == 'M')
				filter_size *= 1024 * 1024;
		}
	}
	if (ab_rounds < 1) ab_rounds = 1;

	/* Setup */
	{
		struct sched_param sp = { .sched_priority = 1 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
			fprintf(stderr, "WARNING: could not set SCHED_FIFO\n");
	}
	if (pin_to_cpu(cpu) < 0)
		fprintf(stderr, "WARNING: could not pin to CPU %d\n", cpu);
	{
		const char *core_env = getenv("PERF_CORE_FREQ_MHZ");
		const char *uncore_env = getenv("PERF_UNCORE_FREQ_MHZ");
		unsigned long core_mhz = core_env ? strtoul(core_env, NULL, 10) : 0;
		unsigned long uncore_mhz = uncore_env ? strtoul(uncore_env, NULL, 10) : 0;

		if (core_mhz || uncore_mhz) {
			if (pin_freq(cpu, core_mhz, uncore_mhz) < 0) {
				fprintf(stderr,
					"ERROR: PERF_CORE_FREQ_MHZ=%s "
					"PERF_UNCORE_FREQ_MHZ=%s but could not "
					"pin. Run as root.\n",
					core_env ? core_env : "(unset)",
					uncore_env ? uncore_env : "(unset)");
			}
		}
	}

	tsc_ghz = calibrate_tsc();

	printf("DTO A/B Performance Test (%s cache)\n",
	       cold_cache ? "cold" : "warm");
	printf("================================================\n");
	printf("Pinned CPU:    %d\n", cpu);
	printf("TSC freq:      %.3f GHz\n", tsc_ghz);
	print_freq_info(cpu);
	printf("A/B rounds:    %d (interleaved, randomized order)%s\n",
	       ab_rounds, short_mode ? " [SHORT]" : "");
	printf("Iters/round:   %d\n", perf_iters);
	printf("Min effect:    %.1f%%\n", min_effect);
	fflush(stdout);

	/* Shared memory for child results */
	void *shm = mmap(NULL, SHARED_SLOT_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm == MAP_FAILED) { perror("mmap shm"); return 1; }

	if (results_dir) {
		char csv_path[4096];

		mkdir(results_dir, 0755);
		snprintf(csv_path, sizeof(csv_path),
			 "%s/distributions.csv", results_dir);
		remove(csv_path);
	}

	/* Count benchmarks */
	int num_benchmarks = 0;

	while (benchmarks[num_benchmarks].name)
		num_benchmarks++;

	/* Store all results: [benchmark][config][pagesize] */
	struct cell_result (*all_results)[NUM_DTO_CONFIGS][NUM_PAGE_CONFIGS];

	all_results = calloc(num_benchmarks,
			     sizeof(*all_results));
	if (!all_results) {
		perror("calloc results");
		return 1;
	}

	/*
	 * Run all benchmarks and collect results.
	 * Then print two detail tables (one per page size) and a summary.
	 */
	for (int b = 0; b < num_benchmarks; b++) {
		struct perf_test *test = &benchmarks[b];

		if (!test_selected(test))
			continue;

		for (int c = 0; c < (int)NUM_DTO_CONFIGS; c++) {
			for (int p = 0; p < (int)NUM_PAGE_CONFIGS; p++) {
				all_results[b][c][p] = run_cell(
					test, &dto_configs[c], &page_configs[p],
					shm, ab_rounds, min_effect,
					results_dir);

				if (all_results[b][c][p].skipped)
					skipped++;
				else if (!all_results[b][c][p].valid)
					errors++;
				/* The cpu config runs libc against itself, so
				 * its "change" is pure noise — never a
				 * regression verdict. */
				else if (all_results[b][c][p].regression &&
					 !test->ref_only &&
					 c != CPU_CONFIG_IDX)
					failures++;

				printf("    [done] %-14s %-10s %-4s\n",
				       test->name, dto_configs[c].label,
				       page_configs[p].label);
				fflush(stdout);
			}
		}
	}

	munmap(shm, SHARED_SLOT_SIZE);

	/*
	 * ---- Detail tables: one per page size ----
	 *
	 * Each table has a row per transaction size, with sub-rows
	 * for each DTO config (cpu, stdc, dsa, dsa_auto).
	 */
	for (int p = 0; p < (int)NUM_PAGE_CONFIGS; p++) {
		printf("\n  ==============================="
		       "=======================================\n");
		printf("  A/B Results — %s pages\n",
		       page_configs[p].hugepage ? "2MB" : "4KB");
		printf("  ==============================="
		       "=======================================\n");
		printf("  %-13s %-9s %-9s   %-9s %-8s %-5s   %-7s   %s\n",
		       "Test", "Config",
		       "Base mean", "Cur mean",
		       "Change", "KS D", "Result", "Outliers");
		printf("  %-13s %-9s %-9s   %-9s %-8s %-5s   %-7s   %s\n",
		       "-------------", "---------",
		       "---------", "---------",
		       "--------", "-----", "-------",
		       "--------");
		for (int b = 0; b < num_benchmarks; b++) {
			if (!test_selected(&benchmarks[b]))
				continue;
			for (int c = 0; c < (int)NUM_DTO_CONFIGS; c++) {
				struct cell_result *r =
					&all_results[b][c][p];

				if (c == 0)
					printf("  %-13s", benchmarks[b].name);
				else
					printf("  %-13s", "");

				printf(" %-9s", dto_configs[c].label);

				if (r->skipped) {
					printf(" %6s ns   %6s ns %8s %-5s   SKIP\n",
					       "", "", "", "");
				} else if (!r->valid) {
					printf(" %6s ns   %6s ns %8s %-5s   ERROR\n",
					       "", "", "", "");
				} else {
					const char *result;

					if (benchmarks[b].ref_only ||
					    c == CPU_CONFIG_IDX)
						result = "REF";
					else if (r->regression)
						result = "FAIL ***";
					else if (r->improved)
						result = "IMPROVED";
					else
						result = "PASS";

					printf(" %6.0f ns   %6.0f ns "
					       "%+7.1f%% %-5.3f   %-7s   "
					       "bl=%d cur=%d\n",
					       r->bl_ns, r->cur_ns,
					       r->change, r->ks_d,
					       result,
					       r->bl_outliers,
					       r->cur_outliers);
				}
			}
		}
	}

	/* ---- Summary: current library speedup vs CPU ---- */
	print_speedup_summary(all_results, num_benchmarks);

	/* Final summary */
	if (failures > 0)
		printf("  %d regression(s) detected (> +%.1f%%)\n",
		       failures, min_effect);
	else
		printf("  No regressions (threshold: +%.1f%%)\n", min_effect);
	if (errors > 0)
		printf("  %d error(s)\n", errors);
	if (skipped > 0)
		printf("  %d cell(s) skipped: 2MB pages unavailable "
		       "(reserve some via /proc/sys/vm/nr_hugepages)\n",
		       skipped);

	free(all_results);
	return (failures > 0 || errors > 0) ? 1 : 0;
}
