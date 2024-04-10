/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <cpuid.h>
#include <linux/idxd.h>
#include <x86intrin.h>
#include <sched.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <dlfcn.h>
#include <accel-config/libaccel_config.h>
#include <numaif.h>
#include <numa.h>

#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)

// DSA capabilities
#define GENCAP_CC_MEMORY  0x4

#define ENQCMD_MAX_RETRIES 3

#define UMWAIT_DELAY 100000
/* C0.1 state */
#define UMWAIT_STATE 1

#define USE_ORIG_FUNC(n) (use_std_lib_calls == 1 || n < dsa_min_size)
#define TS_NS(s, e) (((e.tv_sec*1000000000) + e.tv_nsec) - ((s.tv_sec*1000000000) + s.tv_nsec))

/* Maximum WQs that DTO will use. It is rather an arbitrary limit
 * to keep things simple and avoid having to dynamically allocate memory.
 * Allocating memory dynamically may create cyclic dependency and may cause
 * a hang (e.g., memset --> malloc --> alloc library calls memset --> memset)
 */
#define MAX_WQS 32
#define MAX_NUMA_NODES 32
#define DTO_DEFAULT_MIN_SIZE 8192
#define DTO_INITIALIZED 0
#define DTO_INITIALIZING 1

// thread specific variables
static __thread struct dsa_hw_desc thr_desc;
static __thread struct dsa_completion_record thr_comp __attribute__((aligned(32)));
static __thread uint64_t thr_bytes_completed;

// orignal std memory functions
static void * (*orig_memset)(void *s, int c, size_t n);
static void * (*orig_memcpy)(void *dest, const void *src, size_t n);
static void * (*orig_memmove)(void *dest, const void *src, size_t n);
static int (*orig_memcmp)(const void *s1, const void *s2, size_t n);

struct dto_wq {
	struct accfg_wq *acc_wq;
	char wq_path[PATH_MAX];
	uint64_t dsa_gencap;
	int wq_size;
	uint32_t max_transfer_size;
	int wq_fd;
	void *wq_portal;
};

struct dto_device {
	struct dto_wq* wqs[MAX_WQS];
	uint8_t num_wqs;
	atomic_uchar next_wq;
};

enum wait_options {
	WAIT_BUSYPOLL = 0,
	WAIT_UMWAIT,
	WAIT_YIELD
};

enum numa_aware {
	NA_NONE = 0,
	NA_BUFFER_CENTRIC,
	NA_CPU_CENTRIC,
	NA_LAST_ENTRY
};

static const char * const numa_aware_names[] = {
	[NA_NONE] = "none",
	[NA_BUFFER_CENTRIC] = "buffer-centric",
	[NA_CPU_CENTRIC] = "cpu-centric"
};

// global workqueue variables
static struct dto_wq wqs[MAX_WQS];
static struct dto_device* devices[MAX_NUMA_NODES];
static uint8_t num_wqs;
static atomic_uchar next_wq;
static atomic_uchar dto_initialized;
static atomic_uchar dto_initializing;
static uint8_t use_std_lib_calls;
static enum numa_aware is_numa_aware;
static size_t dsa_min_size = DTO_DEFAULT_MIN_SIZE;
static int wait_method = WAIT_YIELD;
static double cpu_size_fraction;

static uint8_t fork_handler_registered;

enum memop {
	MEMSET = 0x0,
	MEMCOPY,
	MEMMOVE,
	MEMCMP,
	MAX_MEMOP,
};

static const char * const memop_names[] = {
	[MEMSET] = "set",
	[MEMCOPY] = "cpy",
	[MEMMOVE] = "mov",
	[MEMCMP] = "cmp"
};

// memory stats
#define HIST_BUCKET_SIZE 4096
#define HIST_NO_BUCKETS 512
enum stat_group {
	STDC_CALL = 0x0,
	DSA_CALL_SUCCESS,
	DSA_CALL_FAILED,
	DSA_FAIL_CODES,
	MAX_STAT_GROUP
};

static const char * const stat_group_names[] = {
	[STDC_CALL] = "stdc calls",
	[DSA_CALL_SUCCESS] = "dsa (success)",
	[DSA_CALL_FAILED] = "dsa (failed)",
	[DSA_FAIL_CODES] = "failure reason"
};

enum return_code {
	SUCCESS = 0x0,
	RETRIES,
	PAGE_FAULT,
	FAIL_OTHERS,
	MAX_FAILURES,
};

static const char * const failure_names[] = {
	[SUCCESS] = "Success",
	[RETRIES] = "Retries",
	[PAGE_FAULT] = "PFs",
	[FAIL_OTHERS] = "Others",
};

static const char * const wait_names[] = {
	[WAIT_BUSYPOLL] = "busypoll",
	[WAIT_UMWAIT] = "umwait",
	[WAIT_YIELD] = "yield",
};

static int collect_stats;
static char dto_log_path[PATH_MAX];
static int log_fd = -1;

#ifdef DTO_STATS_SUPPORT
static struct timespec dto_start_time;

#define DTO_COLLECT_STATS_START(cs, st)				\
	do {							\
		if (unlikely(cs)) {				\
			clock_gettime(CLOCK_BOOTTIME, &st);	\
		}						\
	} while (0)						\


#define DTO_COLLECT_STATS_DSA_END(cs, st, et, op, n, tbc, r)				\
	do {										\
		if (unlikely(cs)) {							\
			uint64_t t;							\
			clock_gettime(CLOCK_BOOTTIME, &et);				\
			t = (((et.tv_sec*1000000000) + et.tv_nsec) -			\
					((st.tv_sec*1000000000) + st.tv_nsec));		\
			if (unlikely(r != SUCCESS))					\
				update_stats(op, n, tbc, t, DSA_CALL_FAILED, r);	\
			else								\
				update_stats(op, n, tbc, t, DSA_CALL_SUCCESS, 0);	\
		}									\
	} while (0)									\

#define DTO_COLLECT_STATS_CPU_END(cs, st, et, op, n, orig_n)			\
	do {									\
		if (unlikely(cs)) {						\
			uint64_t t;						\
			clock_gettime(CLOCK_BOOTTIME, &et);			\
			t = (((et.tv_sec*1000000000) + et.tv_nsec) -		\
				((st.tv_sec*1000000000) + st.tv_nsec));		\
			update_stats(op, orig_n, n, t, STDC_CALL, 0);		\
		}								\
	} while (0)								\

static atomic_int op_counter[HIST_NO_BUCKETS][MAX_STAT_GROUP][MAX_MEMOP];
static atomic_ullong bytes_counter[HIST_NO_BUCKETS][MAX_STAT_GROUP];
static atomic_ullong lat_counter[HIST_NO_BUCKETS][MAX_STAT_GROUP][MAX_MEMOP];
static atomic_int fail_counter[HIST_NO_BUCKETS][MAX_FAILURES];
#endif

/* call initialize/cleanup functions when library is loaded/unloaded */
static int init_dto(void) __attribute__((constructor));
static void cleanup_dto(void) __attribute__((destructor));

static int umwait_support;

static enum {
	LOG_LEVEL_FATAL,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_TRACE
} log_levels;

#define LOG_FATAL(...) dto_log(LOG_LEVEL_FATAL, __VA_ARGS__)
#define LOG_ERROR(...) dto_log(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_TRACE(...) dto_log(LOG_LEVEL_TRACE, __VA_ARGS__)

static unsigned int log_level = LOG_LEVEL_FATAL;

/* Auto tune heuristics magic numbers */
#define DESCS_PER_RUN 0xF0
#define NUM_DESCS 16
#define MIN_AVG_YIELD_WAITS 1.0
#define MAX_AVG_YIELD_WAITS 2.0
#define MIN_AVG_POLL_WAITS 5.0
#define MAX_AVG_POLL_WAITS 20.0
#define MAX_CPU_SIZE_FRACTION 0.9
#define CSF_STEP_INCREMENT 0.01
#define CSF_STEP_DECREMENT 0.01
#define MAX_DSA_MIN_SIZE 65536
#define MIN_DSA_MIN_SIZE 6144
#define DMS_STEP_INCREMENT 1024
#define DMS_STEP_DECREMENT 1024

/* Auto tuning variables */
static atomic_ullong num_descs;
static atomic_ullong adjust_num_descs;
static atomic_ullong adjust_num_waits;
/* default waits are for yield because yield is default waiting method */
static double min_avg_waits = MIN_AVG_YIELD_WAITS;
static double max_avg_waits = MAX_AVG_YIELD_WAITS;
static uint8_t auto_adjust_knobs = 1;

extern char *__progname;

static void dto_log(int req_log_level, const char *fmt, ...)
{
	char buf[512];
	va_list args;

	if (req_log_level > log_level)
		return;

	va_start(args, fmt);
	if (log_fd == -1)
		vprintf(fmt, args);
	else {
		vsnprintf(buf, sizeof(buf), fmt, args);
		write(log_fd, buf, strlen(buf));
	}
	va_end(args);
}

/* Reinitialize DTO in the child process. */
static void child (void)
{
#ifdef DTO_STATS_SUPPORT
	int i, j, k;

	/* Reset the counters */
	for (i = 0; i < HIST_NO_BUCKETS; i++) {
		for (j = 0; j < MAX_STAT_GROUP; j++) {
			for (k = 0; k < MAX_MEMOP; k++) {
				op_counter[i][j][k] = 0;
				lat_counter[i][j][k] = 0;
			}
			bytes_counter[i][j] = 0;
		}
		for (j = 0; j < MAX_FAILURES; j++)
			fail_counter[i][j] = 0;
	}
#endif
	dto_initializing = 0;
	dto_initialized = 0;
	log_fd = -1;

	init_dto();
}

static __always_inline unsigned char enqcmd(struct dsa_hw_desc *desc, volatile void *reg)
{
	unsigned char retry;

	asm volatile(".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
			"setz %0\t\n"
			: "=r"(retry) : "a" (reg), "d" (desc));
	return retry;
}

static __always_inline void movdir64b(struct dsa_hw_desc *desc, volatile void *reg)
{
	asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n"
		: : "a" (reg), "d" (desc));
}

static __always_inline void umonitor(const volatile void *addr)
{
	asm volatile(".byte 0xf3, 0x48, 0x0f, 0xae, 0xf0" : : "a"(addr));
}

static __always_inline int umwait(unsigned long timeout, unsigned int state)
{
	uint8_t r;
	uint32_t timeout_low = (uint32_t)timeout;
	uint32_t timeout_high = (uint32_t)(timeout >> 32);

	asm volatile(".byte 0xf2, 0x48, 0x0f, 0xae, 0xf1\t\n"
		"setc %0\t\n"
		: "=r"(r)
		: "c"(state), "a"(timeout_low), "d"(timeout_high));
	return r;
}

static __always_inline void dsa_wait_yield(const volatile uint8_t *comp)
{
	while (*comp == 0)
		sched_yield();
}

static __always_inline void dsa_wait_busy_poll(const volatile uint8_t *comp)
{
	while (*comp == 0)
		_mm_pause();
}

static __always_inline void __dsa_wait_umwait(const volatile uint8_t *comp)
{
	umonitor(comp);

	// Hardware never writes 0 to this field. Software should initialize this field to 0
	// so it can detect when the completion record has been written
	if (*comp == 0) {
		uint64_t delay = __rdtsc() + UMWAIT_DELAY;

		umwait(delay, UMWAIT_STATE);
	}
}

static __always_inline void dsa_wait_umwait(const volatile uint8_t *comp)
{

	while (*comp == 0)
		__dsa_wait_umwait(comp);
}

static __always_inline void __dsa_wait(const volatile uint8_t *comp)
{
	if (wait_method == WAIT_YIELD)
		sched_yield();
	else if (wait_method == WAIT_UMWAIT)
		__dsa_wait_umwait(comp);
	else
		_mm_pause();
}

static __always_inline void dsa_wait_no_adjust(const volatile uint8_t *comp)
{
	if (wait_method == WAIT_YIELD)
		dsa_wait_yield(comp);
	else if (wait_method == WAIT_UMWAIT)
		dsa_wait_umwait(comp);
	else
		dsa_wait_busy_poll(comp);
}

/* A simple auto-tuning heuristic.
 * Goal of the Heuristic:
 *   - CPU and DSA should complete their fraction of the job roughly simultaneously.
 *     This minimizes the thread's wait time for DSA while maximizing DSA utilization
 *  Approximating the goal:
 *   - Threads waiting for DSA completion (e.g., by yielding), keep avg. no. of
 *     waits (i.e., yields) between min_avg_waits and max_avg_waits
 *     (see local_num_waits variable below)
 * Heuristic
 *   1) Sample number of waits (local_num_waits) for NUM_DESCS descriptors
 *   every DESCS_PER_RUN descriptors
 *   2) If the avg. number of waits per descriptor > max_avg_waits, decrease load on DSA
 *      - If cpu_size_fraction not too high, increase it by CSF_STEP_INCREMENT
 *      - else if dsa_min_size not too high, increase it by DMS_STEP_INCREMENT
 *   3) If the avg. number of waits per descriptor < min_avg_waits, increase load on DSA
 *      - If cpu_size_fraction not too low, decrease it by CSF_STEP_DECREMENT
 *      - else if dsa_min_size not too low, decrease it by DMS_STEP_DECREMENT
 */
static __always_inline void dsa_wait_and_adjust(const volatile uint8_t *comp)
{
	uint64_t local_num_waits = 0;

	if ((++num_descs & DESCS_PER_RUN) != DESCS_PER_RUN) {
		while (*comp == 0)
			__dsa_wait(comp);

		return;
	}

	/* Run the heuristics as well as wait for DSA */
	while (*comp == 0) {
		__dsa_wait(comp);
		local_num_waits++;
	}
	adjust_num_descs++;
	adjust_num_waits += local_num_waits;

	if (adjust_num_descs >= NUM_DESCS) {
		unsigned long long temp = adjust_num_descs;

		if (temp && atomic_compare_exchange_strong(&adjust_num_descs, &temp, 0)) {
			double avg_num_waits = (double)adjust_num_waits / temp;

			adjust_num_waits = 0;
			if (avg_num_waits > max_avg_waits) {
				if (cpu_size_fraction < MAX_CPU_SIZE_FRACTION)
					cpu_size_fraction += CSF_STEP_INCREMENT;
				else if (dsa_min_size < MAX_DSA_MIN_SIZE)
					dsa_min_size += DMS_STEP_INCREMENT;
			} else if (avg_num_waits < min_avg_waits) {
				if (cpu_size_fraction >= CSF_STEP_DECREMENT)
					cpu_size_fraction -= CSF_STEP_DECREMENT;
				else if (dsa_min_size > MIN_DSA_MIN_SIZE)
					dsa_min_size -= DMS_STEP_DECREMENT;
			}
		}
	}
}

static __always_inline int dsa_wait(struct dto_wq *wq,
	struct dsa_hw_desc *hw, volatile uint8_t *comp)
{
	if (auto_adjust_knobs)
		dsa_wait_and_adjust(comp);
	else
		dsa_wait_no_adjust(comp);

	if (likely(*comp == DSA_COMP_SUCCESS)) {
		thr_bytes_completed += hw->xfer_size;
		return SUCCESS;
	} else if ((*comp & DSA_COMP_STATUS_MASK) == DSA_COMP_PAGE_FAULT_NOBOF) {
		thr_bytes_completed += thr_comp.bytes_completed;
		return PAGE_FAULT;
	}
	LOG_ERROR("failed status %x xfersz %x\n", *comp, hw->xfer_size);
	return FAIL_OTHERS;
}

static __always_inline int dsa_submit(struct dto_wq *wq,
	struct dsa_hw_desc *hw)
{
	int retry;
	//LOG_TRACE("desc flags: 0x%x, opcode: 0x%x\n", hw->flags, hw->opcode);
	__builtin_ia32_sfence();
	for (int r = 0; r < ENQCMD_MAX_RETRIES; ++r) {
		retry = enqcmd(hw, wq->wq_portal);
		if (!retry)
			return SUCCESS;
	}
	return RETRIES;
}

static __always_inline int dsa_execute(struct dto_wq *wq,
	struct dsa_hw_desc *hw, volatile uint8_t *comp)
{
	int retry;
	*comp = 0;
	//LOG_TRACE("desc flags: 0x%x, opcode: 0x%x\n", hw->flags, hw->opcode);
	__builtin_ia32_sfence();
	for (int r = 0; r < ENQCMD_MAX_RETRIES; ++r) {
		retry = enqcmd(hw, wq->wq_portal);
		if (!retry) {
			if (auto_adjust_knobs)
				dsa_wait_and_adjust(comp);
			else
				dsa_wait_no_adjust(comp);

			if (*comp == DSA_COMP_SUCCESS) {
				thr_bytes_completed += hw->xfer_size;
				return SUCCESS;
			} else if ((*comp & DSA_COMP_STATUS_MASK) == DSA_COMP_PAGE_FAULT_NOBOF) {
				thr_bytes_completed += thr_comp.bytes_completed;
				return PAGE_FAULT;
			}
			LOG_ERROR("failed status %x xfersz %x\n", *comp, hw->xfer_size);
			return FAIL_OTHERS;
		}
	}
	return RETRIES;
}

#ifdef DTO_STATS_SUPPORT
static void update_stats(int op, size_t n, size_t bytes_completed,
		uint64_t elapsed_ns, int group, int error_code)
{
	int bucket = (n / HIST_BUCKET_SIZE);

	if (bucket >= HIST_NO_BUCKETS)  /* last bucket includes remaining sizes */
		bucket = HIST_NO_BUCKETS-1;
	++op_counter[bucket][group][op];
	bytes_counter[bucket][group] += bytes_completed;
	lat_counter[bucket][group][op] += elapsed_ns;
	if (group == DSA_CALL_FAILED)
		++fail_counter[bucket][error_code];

}

static void print_stats(void)
{
	struct timespec dto_end_time;

	if (likely(!collect_stats))
		return;

	clock_gettime(CLOCK_BOOTTIME, &dto_end_time);

	LOG_TRACE("DTO Run Time: %ld ms\n", TS_NS(dto_start_time, dto_end_time)/1000000);

	// display stats
	for (int t = 0; t < 2; ++t) {
		if (t == 0)
			LOG_TRACE("\n******** Number of Memory Operations ********\n");
		else
			LOG_TRACE("\n******** Average Memory Operation Latency (us)  ********\n");

		LOG_TRACE("%17s    ", "");
		for (int g = 0; g < MAX_STAT_GROUP; ++g) {
			if (t == 0) {
				if (g == DSA_FAIL_CODES)
					LOG_TRACE("<***** %-13s *****> ", stat_group_names[g]);
				else
					LOG_TRACE("<*************** %-13s ***************> ", stat_group_names[g]);
			} else {
				if (g != DSA_FAIL_CODES)
					LOG_TRACE("<******** %-13s ********> ", stat_group_names[g]);

			}
		}
		LOG_TRACE("\n");

		LOG_TRACE("%-17s -- ", "Byte Range");
		for (int g = 0; g < MAX_STAT_GROUP - 1; ++g) {
			for (int o = 0; o < MAX_MEMOP; ++o)
				LOG_TRACE("%-8s ", memop_names[o]);
			if (t == 0)
				LOG_TRACE("%-12s ", "bytes");
		}
		if (t == 0)
			for (int o = 1; o < MAX_FAILURES; ++o)
				LOG_TRACE("%-6s ", failure_names[o]);
		LOG_TRACE("\n");

		for (int b = 0; b < HIST_NO_BUCKETS; ++b) {
			bool empty = true;

			for (int g = 0; g < MAX_STAT_GROUP; ++g) {
				for (int o = 0; o < MAX_MEMOP; ++o) {
					if (op_counter[b][g][o] != 0) {
						empty = false;
						break;
					}
				}
				if (!empty)
					break;
			}
			if (empty)
				continue;

			if (b < (HIST_NO_BUCKETS-1))
				LOG_TRACE("% 8d-%-8d -- ", b*4096, ((b+1)*4096)-1);
			else
				LOG_TRACE("   >=%-12d -- ", b*4096);

			for (int g = 0; g < MAX_STAT_GROUP - 1; ++g) {
				for (int o = 0; o < MAX_MEMOP; ++o) {
					if (t == 0) {
						LOG_TRACE("%-8d ", op_counter[b][g][o]);
						continue;
					}
					if (op_counter[b][g][o] != 0) {
						double avg_us = ((double) lat_counter[b][g][o])/(((double) op_counter[b][g][o]) * 1000.0);

						LOG_TRACE("%-8.2f ", avg_us);
					} else {
						LOG_TRACE("%-8d ", 0);
					}
				}
				if (t == 0)
					LOG_TRACE("%-12lld ", bytes_counter[b][g]);
			}
			if (t == 0)
				for (int o = 1; o < MAX_FAILURES; ++o)
					LOG_TRACE("%-6d ", fail_counter[b][o]);
			LOG_TRACE("\n");
		}
	}
}
#endif

#define DTO_MAX_PARAM_LEN 16
static void dto_get_param_string(int dir_fd, char *path, char *out)
{
	int fd;
	char buffer[DTO_MAX_PARAM_LEN];
	int bytes;

	out[0] = '\0';
	fd = openat(dir_fd, path, O_RDONLY);

	if (fd < 0)
		return;

	bytes = read(fd, buffer, DTO_MAX_PARAM_LEN - 1);

	if (bytes <= 0) {
		close(fd);
		return;
	}

	if (buffer[bytes - 1] == '\n')
		buffer[bytes - 1] = '\0';
	else
		buffer[bytes] = '\0';

	strncpy(out, buffer, DTO_MAX_PARAM_LEN);
	close(fd);
}

static unsigned long long dto_get_param_ullong(int dir_fd, char *path, int *err)
{
	int fd;
	char buffer[DTO_MAX_PARAM_LEN];
	int bytes;
	unsigned long long val;

	fd = openat(dir_fd, path, O_RDONLY);

	if (fd < 0) {
		*err = -errno;
		return -errno;
	}

	bytes = read(fd, buffer, DTO_MAX_PARAM_LEN - 1);

	if (bytes <= 0) {
		*err = -errno;
		close(fd);
		return -errno;
	}

	errno = 0;
	val = strtoull(buffer, NULL, 0);

	*err = -errno;
	close(fd);

	return val;
}

static struct dto_device* get_dto_device(int dev_numa_node) {
	struct dto_device* dev = NULL;

	if (devices[dev_numa_node] == NULL) {
		dev = calloc(1, sizeof(struct dto_device));
		devices[dev_numa_node] = dev;
	} else {
		dev = devices[dev_numa_node];
	}

	return dev;
}

static void correct_devices_list() {
/* 	Fill NULL gaps in devices list.
	For SNC configurations there are less DSA devices then numa nodes
	ex. SNC4: 8 numa nodes, 2 DSA devices:
	dsa0 device has numa_node = 0, dsa2 device has numa_node = 4
	Then we should use dsa0 device for numa_nodes = 0,1,2,3 and dsa2 device for numa_nodes = 4,5,6,7
	and model the same in devices list.
 */
	struct dto_device* dev = NULL;
	for (uint8_t i = 0; i < MAX_NUMA_NODES; i++) {
		if (devices[i] != NULL) {
			dev = devices[i];
		} else {
			devices[i] = dev;
		}
	}
}

static __always_inline  int get_numa_node(void* buf) {
	int numa_node = -1;

	switch (is_numa_aware) {
        case NA_BUFFER_CENTRIC: {
			if (buf != NULL) {
				int status[1] = {-1};

				// get numa node of memory pointed by buf
				if (move_pages(0, 1, &buf, NULL, status, 0) == 0) {
					numa_node = status[0];
				} else {
					LOG_ERROR("move_pages call error: %d - %s", errno, strerror(errno));
				}

				// alternatively get_mempolicy can be used
				// if (get_mempolicy(&numa_node, NULL, 0, (void *)buf, MPOL_F_NODE | MPOL_F_ADDR) != 0) {
				// 	LOG_ERROR("get_mempolicy call error: %d - %s", errno, strerror(errno));
				// }
			} else {
				LOG_ERROR("NULL buffer delivered. Unable to detect numa node");
			}
		}
		break;
		case NA_CPU_CENTRIC: {
			const int cpu = sched_getcpu();
			if (cpu != -1) {
				numa_node = numa_node_of_cpu(sched_getcpu());
			}
			else {
				LOG_ERROR("sched_getcpu call error: %d - %s", errno, strerror(errno));
			}
		}
		break;
        default:
		break;
        }

        return numa_node;
}

static void cleanup_devices() {
	struct dto_device* dev = NULL;
	for (uint i = 0; i < MAX_NUMA_NODES; i++) {
		if (devices[i] != dev) {
			dev = devices[i];
			free(devices[i]);
		}
		devices[i] = NULL;
	}
}

static int dsa_init_from_wq_list(char *wq_list)
{
	char *wq;
	char file_path[PATH_MAX];
	int dsa_id, wq_id;
	int dir_fd;
	int rc;

	num_wqs = 0;

	wq = strtok(wq_list, ";");
	while (wq != NULL) {
		char wq_mode[DTO_MAX_PARAM_LEN];

		if (sscanf(wq, "wq%d.%d", &dsa_id, &wq_id) != 2) {
			LOG_ERROR("Invalid WQ format %s\n", wq);
			rc = -EINVAL;
			goto fail_wq;
		}

		snprintf(file_path, PATH_MAX, "/sys/bus/dsa/devices/dsa%d", dsa_id);

		dir_fd = open(file_path, O_PATH);
		if (dir_fd == -1) {
			LOG_ERROR("dir %s open failed: %s\n", file_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}

		wqs[num_wqs].dsa_gencap = dto_get_param_ullong(dir_fd, "gen_cap", &rc);
		if (rc) {
			close(dir_fd);
			goto fail_wq;
		}

		const int dev_numa_node = (int)dto_get_param_ullong(dir_fd, "numa_node", &rc);
		if (rc) {
			close(dir_fd);
			goto fail_wq;
		}

		close(dir_fd);

		snprintf(file_path, PATH_MAX, "/sys/bus/dsa/devices/%s", wq);

		dir_fd = open(file_path, O_PATH);
		if (dir_fd == -1) {
			LOG_ERROR("dir %s open failed: %s\n", file_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}

		wqs[num_wqs].max_transfer_size = dto_get_param_ullong(dir_fd, "max_transfer_size", &rc);
		if (rc) {
			close(dir_fd);
			goto fail_wq;
		}

		dto_get_param_string(dir_fd, "mode", wq_mode);

        if (wq_mode[0] == '\0') {
			close(dir_fd);
			rc = -ENOTSUP;
			goto fail_wq;
		}

		if (strcmp(wq_mode, "shared") != 0) {
			continue;
		}

		wqs[num_wqs].wq_size = dto_get_param_ullong(dir_fd, "size", &rc);
		close(dir_fd);

		if (rc)
			goto fail_wq;

		snprintf(wqs[num_wqs].wq_path, PATH_MAX, "/dev/dsa/%s", wq);

		// open DSA WQ
		wqs[num_wqs].wq_fd = open(wqs[num_wqs].wq_path, O_RDWR);
		if (wqs[num_wqs].wq_fd < 0) {
			LOG_ERROR("DSA WQ %s open error: %s\n", wqs[num_wqs].wq_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}

		// map DSA WQ portal
		wqs[num_wqs].wq_portal = mmap(NULL, 0x1000, PROT_WRITE, MAP_SHARED | MAP_POPULATE,
				wqs[num_wqs].wq_fd, 0);
		close(wqs[num_wqs].wq_fd);

		if (wqs[num_wqs].wq_portal == MAP_FAILED) {
			LOG_ERROR("mmap error for DSA wq: %s, error: %s\n", wqs[num_wqs].wq_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}

		if (is_numa_aware) {
			struct dto_device* dev = get_dto_device(dev_numa_node);
			if (dev != NULL &&
				dev->num_wqs < MAX_WQS) {
				dev->wqs[dev->num_wqs++] = &wqs[num_wqs];
			}
		}

		++num_wqs;
		if (num_wqs == MAX_WQS)
			break;

		wq = strtok(NULL, ";");
	}

	if (num_wqs == 0) {
		rc = -EINVAL;
		goto fail;
	}

	if (is_numa_aware) {
		correct_devices_list();
	}

	return 0;

fail_wq:
	for (int j = 0; j < num_wqs; j++)
		munmap(wqs[j].wq_portal, 0x1000);
	num_wqs = 0;

	cleanup_devices();

fail:
	return rc;
}

static int dsa_init_from_accfg(void)
{
	int used_devids[MAX_WQS];
	struct accfg_device *device;
	struct accfg_wq *wq;
	struct accfg_ctx *dto_ctx = NULL;
	int rc;
	int i;

	for (i = 0; i < MAX_WQS; i++) {
		wqs[i].acc_wq = NULL;
		used_devids[i] = -1;
	}

	rc = accfg_new(&dto_ctx);
	if (rc < 0)
		return rc;
	num_wqs = 0;

	accfg_device_foreach(dto_ctx, device) {
		enum accfg_device_state dstate;

		/* use dsa devices only*/
		if (strncmp(accfg_device_get_devname(device), "dsa", 3)!= 0)
			continue;

		/* Make sure that the device is enabled */
		dstate = accfg_device_get_state(device);
		if (dstate != ACCFG_DEVICE_ENABLED)
			continue;

		/* Check if we have already used a wq on this device */
		for (i = 0; i < num_wqs; i++)
			if (accfg_device_get_id(device) == used_devids[i])
				break;
		if (i != num_wqs)
			continue;

		struct dto_device* dev = NULL;

		if (is_numa_aware) {
			const int dev_numa_node = accfg_device_get_numa_node(device);
			dev = get_dto_device(dev_numa_node);
		}

		accfg_wq_foreach(device, wq) {
			enum accfg_wq_state wstate;
			enum accfg_wq_mode mode;
			enum accfg_wq_type type;

			/* Get a workqueue that's enabled */
			wstate = accfg_wq_get_state(wq);
			if (wstate != ACCFG_WQ_ENABLED)
				continue;

			/* The wq type should be user */
			type = accfg_wq_get_type(wq);
			if (type != ACCFG_WQT_USER)
				continue;

			/* the wq mode should be shared work queue */
			mode = accfg_wq_get_mode(wq);
			if (mode != ACCFG_WQ_SHARED)
				continue;

			wqs[num_wqs].wq_size = accfg_wq_get_size(wq);
			wqs[num_wqs].max_transfer_size = accfg_wq_get_max_transfer_size(wq);

			wqs[num_wqs].acc_wq = wq;
			wqs[num_wqs].dsa_gencap = accfg_device_get_gen_cap(device);

			used_devids[num_wqs] = accfg_device_get_id(device);

			if (is_numa_aware &&
				dev != NULL &&
				dev->num_wqs < MAX_WQS) {
				dev->wqs[dev->num_wqs++] = &wqs[num_wqs];
			}

			num_wqs++;
		}

		if (num_wqs == MAX_WQS)
			break;
	}

	if (num_wqs == 0) {
		rc = -EINVAL;
		goto fail;
	}

	for (i = 0; i < num_wqs; i++) {
		struct accfg_wq *acc_wq = wqs[i].acc_wq;

		rc = accfg_wq_get_user_dev_path(acc_wq, wqs[i].wq_path, PATH_MAX);
		if (rc) {
			LOG_ERROR("Error getting device path\n");
			goto fail_wq;
		}

		// open DSA WQ
		wqs[i].wq_fd = open(wqs[i].wq_path, O_RDWR);
		if (wqs[i].wq_fd < 0) {
			LOG_ERROR("DSA WQ %s open error: %s\n", wqs[i].wq_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}

		// map DSA WQ portal
		wqs[i].wq_portal = mmap(NULL, 0x1000, PROT_WRITE, MAP_SHARED | MAP_POPULATE, wqs[i].wq_fd, 0);
		close(wqs[i].wq_fd);

		if (wqs[i].wq_portal == MAP_FAILED) {
			LOG_ERROR("mmap error for DSA wq: %s, error: %s\n", wqs[i].wq_path, strerror(errno));
			rc = -errno;
			goto fail_wq;
		}
	}

	if (is_numa_aware) {
		correct_devices_list();
	}

	accfg_unref(dto_ctx);
	return 0;

fail_wq:
	for (int j = 0; j < i; j++)
		munmap(wqs[j].wq_portal, 0x1000);
	num_wqs = 0;

	cleanup_devices();
fail:
	accfg_unref(dto_ctx);
	return rc;
}

static int dsa_init(void)
{
	unsigned int unused[2];
	unsigned int leaf, waitpkg;
	const char *env_str;
	char wq_list[256];

	/* detect umwait support */
	leaf = 7;
	waitpkg = 0;
	if (__get_cpuid(0, &leaf, unused, &waitpkg, unused + 1)) {
		if (waitpkg & 0x20) {
			LOG_TRACE("umwait supported\n");
			umwait_support = 1;
		}
	}

	env_str = getenv("DTO_WAIT_METHOD");
	if (env_str != NULL) {
		if (!strncmp(env_str, wait_names[WAIT_BUSYPOLL], strlen(wait_names[WAIT_BUSYPOLL]))) {
			wait_method = WAIT_BUSYPOLL;
			min_avg_waits = MIN_AVG_POLL_WAITS;
			max_avg_waits = MAX_AVG_POLL_WAITS;
		} else if (!strncmp(env_str, wait_names[WAIT_UMWAIT], strlen(wait_names[WAIT_UMWAIT]))) {
			if (umwait_support) {
				wait_method = WAIT_UMWAIT;
				/* Use the same waits as busypoll for now */
				min_avg_waits = MIN_AVG_POLL_WAITS;
				max_avg_waits = MAX_AVG_POLL_WAITS;
			} else
				LOG_ERROR("umwait not supported. Falling back to default wait method\n");
		}
	}

	env_str = getenv("DTO_WQ_LIST");
	if (env_str == NULL)
		return dsa_init_from_accfg();

	strncpy(wq_list, env_str, sizeof(wq_list) - 1);
	/* ensure wq_list is null terminated */
	wq_list[sizeof(wq_list) - 1] = '\0';

	return dsa_init_from_wq_list(wq_list);
}

static int init_dto(void)
{
	uint8_t init_notcomplete = 0;

	if (atomic_compare_exchange_strong(&dto_initializing, &init_notcomplete, 1)) {
		char *env_str;

		env_str = getenv("DTO_LOG_FILE");
		if (env_str != NULL) {
			char temp[PATH_MAX];
			struct stat st;

			strncpy(dto_log_path, env_str, PATH_MAX - 1);
			/* ensure dto_log_path is null terminated */
			dto_log_path[PATH_MAX - 1] = '\0';

			snprintf(temp, sizeof(temp), ".%s.%d", __progname, getpid());
			strncat(dto_log_path, temp, PATH_MAX - strlen(dto_log_path) - 1);

			/* Open the log file only if it doesn't exist or if it is a regular file */
			if (lstat(dto_log_path, &st) == -1 ||
					(st.st_mode & S_IFMT) == S_IFREG)
				log_fd = open(dto_log_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
			/* No need to handle the open() error. It will automatically fallback
			 * to using standard output if log file open failed
			 */
		}

		env_str = getenv("DTO_LOG_LEVEL");
		if (env_str != NULL) {
			errno = 0;
			log_level = strtoul(env_str, NULL, 10);
			if (errno)
				log_level = LOG_LEVEL_FATAL;

			if (log_level > LOG_LEVEL_TRACE)
				log_level = LOG_LEVEL_TRACE;
		}

		// save std c lib function pointers
		orig_memset = dlsym(RTLD_NEXT, "memset");
		orig_memcpy = dlsym(RTLD_NEXT, "memcpy");
		orig_memmove = dlsym(RTLD_NEXT, "memmove");
		orig_memcmp = dlsym(RTLD_NEXT, "memcmp");

		env_str = getenv("DTO_USESTDC_CALLS");
		if (env_str != NULL) {
			errno = 0;
			use_std_lib_calls = strtoul(env_str, NULL, 10);
			if (errno)
				use_std_lib_calls = 0;

			use_std_lib_calls = !!use_std_lib_calls;
		}

		if (numa_available() != -1) {
			env_str = getenv("DTO_IS_NUMA_AWARE");
			if (env_str != NULL) {
				errno = 0;
				is_numa_aware = strtoul(env_str, NULL, 10);
				if (errno || is_numa_aware >= NA_LAST_ENTRY) {
					is_numa_aware = NA_NONE;
				}
			}
		}

#ifdef DTO_STATS_SUPPORT
		env_str = getenv("DTO_COLLECT_STATS");
		if (env_str != NULL) {
			errno = 0;
			collect_stats = strtoul(env_str, NULL, 10);
			if (errno)
				collect_stats = 0;

			collect_stats = !!collect_stats;
		}

		if (collect_stats) {
			clock_gettime(CLOCK_BOOTTIME, &dto_start_time);
			/* Change the log level to 'trace' so that the
			 * stats can be logged
			 */
			log_level = LOG_LEVEL_TRACE;
		}
#endif

		/* Register fork handler for the child process */
		if (!fork_handler_registered) {
			/* If pthread_atfork fails, and process calls fork,
			 * the child may crash when using DSA offload. Dont
			 * take that risk and disable DSA offload for parent
			 * as well.
			 */
			if (!pthread_atfork(NULL, NULL, child))
				fork_handler_registered = 1;
			else {
				LOG_ERROR("Setting fork() handler failed. "
					"Falling back to using CPUs.\n");
				use_std_lib_calls = 1;
			}
		}

		// initialize DSA
		if (!use_std_lib_calls) {
			// check environment variables
			env_str = getenv("DTO_MIN_BYTES");

			if (env_str != NULL) {
				errno = 0;
				dsa_min_size = strtoul(env_str, NULL, 10);
				if (errno)
					dsa_min_size = DTO_DEFAULT_MIN_SIZE;
			}

			env_str = getenv("DTO_CPU_SIZE_FRACTION");

			if (env_str != NULL) {
				errno = 0;
				cpu_size_fraction = strtod(env_str, NULL);

				if (errno || cpu_size_fraction < 0 || cpu_size_fraction >= 1) {
					LOG_ERROR("Invalid DTO_CPU_SIZE_FRACTION %s, "
						"Must be >= 0 and < 1. "
						"Falling back to default 0.0\n", env_str);
					cpu_size_fraction = 0.0;
				}
				/* Use only 2 digits after decimal point */
				cpu_size_fraction = 100 * cpu_size_fraction;
				cpu_size_fraction = (double) ((uint64_t)cpu_size_fraction/(double)100);
			}

			env_str = getenv("DTO_AUTO_ADJUST_KNOBS");

			if (env_str != NULL) {
				errno = 0;
				auto_adjust_knobs = strtoul(env_str, NULL, 10);
				if (errno)
					auto_adjust_knobs = 1;

				auto_adjust_knobs = !!auto_adjust_knobs;
			}

			if (dsa_init()) {
				LOG_ERROR("Didn't find any usable DSAs. Falling back to using CPUs.\n");
				use_std_lib_calls = 1;
			}

			// display configuration
			LOG_TRACE("log_level: %d, collect_stats: %d, use_std_lib_calls: %d, dsa_min_size: %lu, "
				"cpu_size_fraction: %.2f, wait_method: %s, auto_adjust_knobs: %d, numa_awareness: %s\n",
				log_level, collect_stats, use_std_lib_calls, dsa_min_size,
				cpu_size_fraction, wait_names[wait_method], auto_adjust_knobs, numa_aware_names[is_numa_aware]);
			for (int i = 0; i < num_wqs; i++)
				LOG_TRACE("[%d] wq_path: %s, wq_size: %d, dsa_cap: %lx\n", i,
					wqs[i].wq_path, wqs[i].wq_size, wqs[i].dsa_gencap);
		}
		dto_initialized = 1;

		return DTO_INITIALIZED;
	}

	return DTO_INITIALIZING;
}

static void cleanup_dto(void)
{
	// unmap and close wq portal
	for (int i = 0; i < num_wqs; i++) {
		if (wqs[i].wq_portal != NULL)
			munmap(wqs[i].wq_portal, 0x1000);
	}
#ifdef DTO_STATS_SUPPORT
	print_stats();
#endif
	if (log_fd != -1)
		close(log_fd);

	cleanup_devices();
}

static __always_inline  struct dto_wq *get_wq(void* buf)
{
	struct dto_wq* wq = NULL;

	if (is_numa_aware) {
		int status[1] = {-1};

		// get the numa node for the target DSA device
		const int numa_node = get_numa_node(buf);
		if (numa_node >= 0 && numa_node < MAX_NUMA_NODES) {
			struct dto_device* dev = devices[numa_node];
			if (dev != NULL &&
				dev->num_wqs > 0) {
				wq = dev->wqs[dev->next_wq++ % dev->num_wqs];
			}
		}
	}

	if (wq == NULL) {
		wq = &wqs[next_wq++ % num_wqs];
	}

	return wq;
}

static void dto_memset(void *s, int c, size_t n, int *result)
{
	uint64_t memset_pattern;
	size_t cpu_size, dsa_size;
	struct dto_wq *wq = get_wq(s);

	for (int i = 0; i < 8; ++i)
		((uint8_t *) &memset_pattern)[i] = (uint8_t) c;

	thr_desc.opcode = DSA_OPCODE_MEMFILL;
	thr_desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (wq->dsa_gencap & GENCAP_CC_MEMORY)
		thr_desc.flags |= IDXD_OP_FLAG_CC;
	thr_desc.completion_addr = (uint64_t)&thr_comp;
	thr_desc.pattern = memset_pattern;

	/* cpu_size_fraction gauranteed to be >= 0 and < 1 */
	cpu_size = n * cpu_size_fraction;
	dsa_size = n - cpu_size;

	thr_bytes_completed = 0;
	if (dsa_size <= wq->max_transfer_size) {
		thr_desc.dst_addr = (uint64_t) s + cpu_size;
		thr_desc.xfer_size = (uint32_t) dsa_size;
		thr_comp.status = 0;
		*result = dsa_submit(wq, &thr_desc);
		if (likely(*result == SUCCESS)) {
			if (cpu_size) {
				orig_memset(s, c, cpu_size);
				thr_bytes_completed = cpu_size;
			}
			*result = dsa_wait(wq, &thr_desc, &thr_comp.status);
		}
	} else {
		uint32_t threshold;
		double fraction_snapshot = cpu_size_fraction;

		threshold = wq->max_transfer_size / (1 - fraction_snapshot);

		do {
			size_t len;

			len = n <= threshold ? n : threshold;

			cpu_size = len * fraction_snapshot;
			dsa_size = len - cpu_size;

			thr_desc.dst_addr = (uint64_t) s + cpu_size + thr_bytes_completed;
			thr_desc.xfer_size = (uint32_t) dsa_size;
			thr_comp.status = 0;
			*result = dsa_submit(wq, &thr_desc);
			if (*result == SUCCESS) {
				if (cpu_size) {
					void *s1 = s + thr_bytes_completed;

					orig_memset(s1, c, cpu_size);
					thr_bytes_completed += cpu_size;
				}
				*result = dsa_wait(wq, &thr_desc, &thr_comp.status);
			}

			if (*result != SUCCESS)
				break;
			n -= len;
			/* If remaining bytes are less than dsa_min_size,
			 * dont submit to DSA. Instead, complete remaining
			 * bytes on CPU
			 */
		} while (n >= dsa_min_size);
	}
}

/* For overlapping src & dest buffers in memmove API, we can't split the memmove
 * job. Otherwise, it may lead to incorrect copy operation.
 */
static bool is_overlapping_buffers (void *dest, const void *src, size_t n)
{
	if ((dest + n) < src || (src + n) < dest)
		return false;

	return true;
}

static void dto_memcpymove(void *dest, const void *src, size_t n, bool is_memcpy, int *result)
{
	struct dto_wq *wq = get_wq(dest);
	size_t cpu_size, dsa_size;

	thr_desc.opcode = DSA_OPCODE_MEMMOVE;
	thr_desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (wq->dsa_gencap & GENCAP_CC_MEMORY)
		thr_desc.flags |= IDXD_OP_FLAG_CC;
	thr_desc.completion_addr = (uint64_t)&thr_comp;

	/* cpu_size_fraction gauranteed to be >= 0 and < 1 */
	if (!is_memcpy && is_overlapping_buffers(dest, src, n))
		cpu_size = 0;
	else
		cpu_size = n * cpu_size_fraction;

	dsa_size = n - cpu_size;

	thr_bytes_completed = 0;

	if (dsa_size <= wq->max_transfer_size) {
		thr_desc.src_addr = (uint64_t) src + cpu_size;
		thr_desc.dst_addr = (uint64_t) dest + cpu_size;
		thr_desc.xfer_size = (uint32_t) dsa_size;
		thr_comp.status = 0;
		*result = dsa_submit(wq, &thr_desc);
		if (*result == SUCCESS) {
			if (cpu_size) {
				if (is_memcpy)
					orig_memcpy(dest, src, cpu_size);
				else
					orig_memmove(dest, src, cpu_size);
				thr_bytes_completed += cpu_size;
			}
			*result = dsa_wait(wq, &thr_desc, &thr_comp.status);
		}
	} else {
		uint32_t threshold;
		double fraction_snapshot = cpu_size_fraction;

		threshold = wq->max_transfer_size / (1 - fraction_snapshot);

		do {
			size_t len;

			len = n <= threshold ? n : threshold;

			if (!is_memcpy && is_overlapping_buffers(dest, src, len))
				cpu_size = 0;
			else
				cpu_size = len * cpu_size_fraction;

			dsa_size = len - cpu_size;

			thr_desc.src_addr = (uint64_t) src + cpu_size + thr_bytes_completed;
			thr_desc.dst_addr = (uint64_t) dest + cpu_size + thr_bytes_completed;
			thr_desc.xfer_size = (uint32_t) dsa_size;
			thr_comp.status = 0;
			*result = dsa_submit(wq, &thr_desc);
			if (*result == SUCCESS) {
				if (cpu_size) {
					const void *src1 = src + thr_bytes_completed;
					void *dest1 = dest + thr_bytes_completed;

					if (is_memcpy)
						orig_memcpy(dest1, src1, cpu_size);
					else
						orig_memmove(dest1, src1, cpu_size);
					thr_bytes_completed += cpu_size;
				}
				*result = dsa_wait(wq, &thr_desc, &thr_comp.status);
			}

			if (*result != SUCCESS)
				break;
			n -= len;
			/* If remaining bytes are less than dsa_min_size,
			 * dont submit to DSA. Instead, complete remaining
			 * bytes on CPU
			 */
		} while (n >= dsa_min_size);
	}
}

static int dto_memcmp(const void *s1, const void *s2, size_t n, int *result)
{
	struct dto_wq *wq = get_wq((void*)s2);
	int cmp_result = 0;
	size_t orig_n = n;

	thr_desc.opcode = DSA_OPCODE_COMPARE;
	thr_desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	thr_desc.completion_addr = (uint64_t)&thr_comp;
	thr_comp.result = 0;

	thr_bytes_completed = 0;

	if (n <= wq->max_transfer_size) {
		thr_desc.src_addr = (uint64_t) s1;
		thr_desc.src2_addr = (uint64_t) s2;
		thr_desc.xfer_size = (uint32_t) n;
		*result = dsa_execute(wq, &thr_desc, &thr_comp.status);
	} else {
		do {
			size_t len;

			len = n <= wq->max_transfer_size ? n : wq->max_transfer_size;

			thr_desc.src_addr = (uint64_t) s1 + thr_bytes_completed;
			thr_desc.src2_addr = (uint64_t) s2 + thr_bytes_completed;
			thr_desc.xfer_size = (uint32_t) len;
			*result = dsa_execute(wq, &thr_desc, &thr_comp.status);

			if (*result != SUCCESS || thr_comp.result)
				break;

			n -= len;
			/* If remaining bytes are less than dsa_min_size,
			 * dont submit to DSA. Instead, complete remaining
			 * bytes on CPU
			 */
		} while (n >= dsa_min_size);
	}

	if (thr_comp.result) {
		/* cmp returned mismatch. determine the return value */
		uint8_t *t1 = (uint8_t *)s1 + thr_bytes_completed;
		uint8_t *t2 = (uint8_t *)s2 + thr_bytes_completed;

		cmp_result = *t1 - *t2;
		/* Inform the caller than the job is done even though
		 * we didnt process all the bytes
		 */
		thr_bytes_completed = orig_n;
	}
	return cmp_result;
}

/* The dto_internal_mem* APIs are used only when mem* APIs are
 * called before DTO is properly initialized. So these
 * implementations dont have to be performant
 */
static void *dto_internal_memset(void *s1, int c, size_t n)
{
	char *dest = s1;
	size_t i;

	for (i = 0; i < n; i++)
		dest[i] = (char)c;

	return s1;
}

static void *dto_internal_memcpymove(void *dest, const void *src, size_t n)
{
	char *d = dest;
	const char *s = (const char *)src;
	ssize_t i;

	if (s >= d) {
		/* go from beginning to end */
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		/* go from end to beginning */
		for (i = n - 1; i >= 0; i--)
			d[i] = s[i];
	}

	return dest;
}

static int dto_internal_memcmp(const void *s1, const void *s2, size_t n)
{
	const unsigned char *src1 = (const unsigned char *)s1;
	const unsigned char *src2 = (const unsigned char *)s2;
	size_t i;

	for (i = 0; i < n; i++) {
		if (src1[i] != src2[i])
			return src1[i] - src2[i];
	}
	return 0;
}

void *memset(void *s1, int c, size_t n)
{
	int result = 0;
	void *ret = s1;
	int use_orig_func = USE_ORIG_FUNC(n);
#ifdef DTO_STATS_SUPPORT
	struct timespec st, et;
	size_t orig_n = n;
#endif

	if (unlikely(dto_initialized == 0)) {
		/* If there are other constructors in the same binary,
		 * they may run before DTO's constructor. Just use
		 * internal CPU-based implementation if DTO is not
		 * initialized yet.
		 */
		return dto_internal_memset(s1, c, n);
	}

	if (!use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif
		dto_memset(s1, c, n, &result);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMSET, n, thr_bytes_completed, result);
#endif
		if (thr_bytes_completed != n) {
			/* fallback to std call if job is only partially completed */
			use_orig_func = 1;
			n -= thr_bytes_completed;
			s1 = (void *)((uint64_t)s1 + thr_bytes_completed);
		}
	}

	if (use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif

		orig_memset(s1, c, n);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_CPU_END(collect_stats, st, et, MEMSET, n, orig_n);
#endif
	}
	return ret;
}

void *memcpy(void *dest, const void *src, size_t n)
{
	int result = 0;
	void *ret = dest;
	int use_orig_func = USE_ORIG_FUNC(n);
#ifdef DTO_STATS_SUPPORT
	struct timespec st, et;
	size_t orig_n = n;
#endif

	if (unlikely(dto_initialized == 0)) {
		/* If there are other constructors in the same binary,
		 * they may run before DTO's constructor. Just use
		 * internal CPU-based implementation if DTO is not
		 * initialized yet.
		 */
		return dto_internal_memcpymove(dest, src, n);
	}

	if (!use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif
		dto_memcpymove(dest, src, n, 1, &result);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMCOPY, n, thr_bytes_completed, result);
#endif
		if (thr_bytes_completed != n) {
			/* fallback to std call if job is only partially completed */
			use_orig_func = 1;
			n -= thr_bytes_completed;
			if (thr_comp.result == 0) {
				dest = (void *)((uint64_t)dest + thr_bytes_completed);
				src = (const void *)((uint64_t)src + thr_bytes_completed);
			}
		}
	}

	if (use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif

		orig_memcpy(dest, src, n);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_CPU_END(collect_stats, st, et, MEMCOPY, n, orig_n);
#endif
	}
	return ret;
}

void *memmove(void *dest, const void *src, size_t n)
{
	int result = 0;
	void *ret = dest;
	int use_orig_func = USE_ORIG_FUNC(n);
#ifdef DTO_STATS_SUPPORT
	struct timespec st, et;
	size_t orig_n = n;
#endif

	if (unlikely(dto_initialized == 0)) {
		/* If there are other constructors in the same binary,
		 * they may run before DTO's constructor. Just use
		 * internal CPU-based implementation if DTO is not
		 * initialized yet.
		 */
		return dto_internal_memcpymove(dest, src, n);
	}

	if (!use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif
		dto_memcpymove(dest, src, n, 0, &result);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMMOVE, n, thr_bytes_completed, result);
#endif
		if (thr_bytes_completed != n) {
			/* fallback to std call if job is only partially completed */
			use_orig_func = 1;
			n -= thr_bytes_completed;
			if (thr_comp.result == 0) {
				dest = (void *)((uint64_t)dest + thr_bytes_completed);
				src = (const void *)((uint64_t)src + thr_bytes_completed);
			}
		}
	}

	if (use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif

		orig_memmove(dest, src, n);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_CPU_END(collect_stats, st, et, MEMMOVE, n, orig_n);
#endif
	}
	return ret;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	int result = 0;
	int ret;
	int use_orig_func = USE_ORIG_FUNC(n);
#ifdef DTO_STATS_SUPPORT
	struct timespec st, et;
	size_t orig_n = n;
#endif

	if (unlikely(dto_initialized == 0)) {
		/* If there are other constructors in the same binary,
		 * they may run before DTO's constructor. Just use
		 * internal CPU-based implementation if DTO is not
		 * initialized yet.
		 */
		return dto_internal_memcmp(s1, s2, n);
	}

	if (!use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif
		ret = dto_memcmp(s1, s2, n, &result);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMCMP, n, thr_bytes_completed, result);
#endif
		if (thr_bytes_completed != n) {
			/* fallback to std call if job is only partially completed */
			use_orig_func = 1;
			n -= thr_bytes_completed;
			s1 = (const void *)((uint64_t)s1 + thr_bytes_completed);
			s2 = (const void *)((uint64_t)s2 + thr_bytes_completed);
		}
	}

	if (use_orig_func) {
#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_START(collect_stats, st);
#endif

		ret = orig_memcmp(s1, s2, n);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_CPU_END(collect_stats, st, et, MEMCMP, n, orig_n);
#endif
	}
	return ret;
}
