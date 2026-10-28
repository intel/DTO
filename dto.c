/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/limits.h>
#include <cpuid.h>
#include <linux/idxd.h>

#include "dto.h"
#include <x86intrin.h>
#include <sched.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <dlfcn.h>
#ifdef DTO_ACCEL_CONFIG_SUPPORT
#include <accel-config/libaccel_config.h>
#endif
#ifdef DTO_NUMA_SUPPORT
#include <numaif.h>
#include <numa.h>
#endif
#include <signal.h>

#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)

// DSA capabilities
#define GENCAP_CC_MEMORY  0x4

#define UMWAIT_DELAY_DEFAULT 100000 //cycles until umwait timeout

#define C01_STATE 1
#define C02_STATE 0
#define TPAUSE_DELAY 1000

#define USE_ORIG_FUNC(n, use_dsa) (use_std_lib_calls == 1 || !use_dsa || n < dsa_min_size)
#define TS_NS(s, e) (((e.tv_sec*1000000000) + e.tv_nsec) - ((s.tv_sec*1000000000) + s.tv_nsec))

/* Maximum WQs that DTO will use. It is rather an arbitrary limit
 * to keep things simple and avoid having to dynamically allocate memory.
 * Allocating memory dynamically may create cyclic dependency and may cause
 * a hang (e.g., memset --> malloc --> alloc library calls memset --> memset)
 */
#define MAX_WQS 32
#define MAX_NUMA_NODES 32
#define DTO_DEFAULT_MIN_SIZE 65536
#define DTO_INITIALIZED 0
#define DTO_INITIALIZING 1


#define NSEC_PER_SEC (1000000000)
#define MSEC_PER_SEC (1000)
#define NSEC_PER_MSEC (NSEC_PER_SEC/MSEC_PER_SEC)

// thread specific variables
#ifndef DTO_NO_LIBC_INTERPOSE
static __thread struct dsa_hw_desc thr_desc;
#endif
static __thread struct dsa_completion_record thr_comp __attribute__((aligned(32)));
static __thread uint64_t thr_bytes_completed;

// original std memory functions
static void * (*orig_memset)(void *s, int c, size_t n);
static void * (*orig_memcpy)(void *dest, const void *src, size_t n);
static void * (*orig_memmove)(void *dest, const void *src, size_t n);
static int (*orig_memcmp)(const void *s1, const void *s2, size_t n);

struct dto_wq {
#ifdef DTO_ACCEL_CONFIG_SUPPORT
	struct accfg_wq *acc_wq;
#endif
	char wq_path[PATH_MAX];
	uint64_t dsa_gencap;
	int wq_size;
	uint32_t max_transfer_size;
	uint32_t max_batch_size;
	int wq_fd;
	void *wq_portal;
	bool wq_mmapped;
};

#ifdef DTO_NUMA_SUPPORT
struct dto_device {
	struct dto_wq* wqs[MAX_WQS];
	uint8_t num_wqs;
};
#endif

enum wait_options {
	WAIT_BUSYPOLL = 0,
	WAIT_UMWAIT,
	WAIT_YIELD,
	WAIT_TPAUSE
};

enum numa_aware {
	NA_NONE = 0,
	NA_BUFFER_CENTRIC,
	NA_CPU_CENTRIC,
	NA_LAST_ENTRY
};

enum overlapping_memmove_actions {
	OVERLAPPING_CPU = 0,
	OVERLAPPING_DSA,
	OVERLAPPING_LAST_ENTRY
};

static const char * const numa_aware_names[] = {
	[NA_NONE] = "none",
	[NA_BUFFER_CENTRIC] = "buffer-centric",
	[NA_CPU_CENTRIC] = "cpu-centric"
};

// global workqueue variables
static struct dto_wq wqs[MAX_WQS];
#ifdef DTO_NUMA_SUPPORT
static struct dto_device* devices[MAX_NUMA_NODES];
#endif
static uint8_t num_wqs;
/* round-robin start position for the next thread that picks a WQ (get_wq) */
static atomic_uint next_thread_wq;
/* this thread's round-robin position */
static __thread unsigned int thr_wq_rr;
static __thread bool thr_wq_rr_set;
static atomic_uchar dto_initialized;
static atomic_uchar dto_initializing;
static uint8_t use_std_lib_calls;
static enum numa_aware is_numa_aware;
static size_t dsa_min_size = DTO_DEFAULT_MIN_SIZE;
/* DTO_CRC_MIN_BYTES: smallest dto_crc32c_with_seed() sent to DSA; 0 follows
 * dsa_min_size */
static size_t crc_min_size;
static int wait_method = WAIT_BUSYPOLL;
static size_t cpu_size_fraction;   // range of values is 0 to 99

static uint8_t dto_dsa_memcpy = 1;
static uint8_t dto_dsa_memmove = 1;
static uint8_t dto_dsa_memset = 1;
static uint8_t dto_dsa_memcmp = 1;

static uint8_t dto_dsa_cc = 1;
static bool dto_use_c02 = true; //C02 state is default -
                            //C02 avg exit latency is ~500 ns
                            //and C01 is about ~240 ns on SPR

#define TPAUSE_C02_DELAY_NS 6000 //in this case we are offloading so delay can
                                 //be ~6 us as this is around the time a > 64KB 
                                 //copy takes to complete

#define TPAUSE_C01_DELAY_NS 1000 //keep smaller because we want to wake up
                                 //with lower latency

static uint64_t tpause_wait_time = TPAUSE_C02_DELAY_NS;

static unsigned long dto_umwait_delay = UMWAIT_DELAY_DEFAULT;

static uint8_t dto_overlapping_memmove_action = OVERLAPPING_CPU;

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

// memory stats: a histogram by call size, with bucket 0 for calls under
// 64 bytes, then four buckets per power of two up to 4 GiB, and a last
// bucket for anything larger
#define HIST_MIN_SHIFT 6
#define HIST_MAX_SHIFT 32
#define HIST_SUBS 4
#define HIST_NO_BUCKETS (2 + (HIST_MAX_SHIFT - HIST_MIN_SHIFT) * HIST_SUBS)
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
	RETRY,
	PAGE_FAULT,
	FAIL_OTHERS,
	MAX_FAILURES,
};

static const char * const failure_names[] = {
	[SUCCESS] = "Success",
	[RETRY] = "Retry",
	[PAGE_FAULT] = "PFs",
	[FAIL_OTHERS] = "Others",
};

static const char * const wait_names[] = {
	[WAIT_BUSYPOLL] = "busypoll",
	[WAIT_UMWAIT] = "umwait",
	[WAIT_YIELD] = "yield",
        [WAIT_TPAUSE] = "tpause"
};

static int collect_stats;
static char dto_log_path[PATH_MAX];
static int log_fd = -1;
static int stats_fd = -1;

#ifdef DTO_STATS_SUPPORT
static struct timespec dto_start_time;

#define DTO_COLLECT_STATS_START(cs, st)				\
	do {							\
		if (unlikely(cs)) {				\
			clock_gettime(CLOCK_BOOTTIME, &st);	\
		}						\
	} while (0)						\


#define DTO_COLLECT_STATS_DSA_END(cs, st, et, op, n, overlap, tbc, r)				\
	do {										\
		if (unlikely(cs)) {							\
			uint64_t t;							\
			clock_gettime(CLOCK_BOOTTIME, &et);				\
			t = (((et.tv_sec*1000000000) + et.tv_nsec) -			\
					((st.tv_sec*1000000000) + st.tv_nsec));		\
			if (unlikely(r != SUCCESS))					\
				update_stats(op, n, overlap, tbc, t, DSA_CALL_FAILED, r);	\
			else								\
				update_stats(op, n, overlap, tbc, t, DSA_CALL_SUCCESS, 0);	\
		}									\
	} while (0)									\

#define DTO_COLLECT_STATS_CPU_END(cs, st, et, op, n, orig_n)			\
	do {									\
		if (unlikely(cs)) {						\
			uint64_t t;						\
			clock_gettime(CLOCK_BOOTTIME, &et);			\
			t = (((et.tv_sec*1000000000) + et.tv_nsec) -		\
				((st.tv_sec*1000000000) + st.tv_nsec));		\
			update_stats(op, orig_n, false, n, t, STDC_CALL, 0);		\
		}								\
	} while (0)								\

/* the groups that have per-operation counters */
#define STAT_GROUPS DSA_FAIL_CODES

struct dto_op_stats {
	uint64_t calls;
	uint64_t bytes;
	uint64_t ns;
};

struct dto_bucket_stats {
	struct dto_op_stats op[STAT_GROUPS][MAX_MEMOP];
	uint64_t fails[MAX_FAILURES];
};

struct dto_stats {
	struct dto_op_stats total[STAT_GROUPS][MAX_MEMOP];
	uint64_t fails[MAX_FAILURES];
	struct dto_bucket_stats hist[HIST_NO_BUCKETS];
};

/* one per counting thread (see update_stats) */
struct dto_thread_stats {
	struct dto_thread_stats *prev, *next;
	struct dto_stats s;
};

static struct dto_thread_stats *stats_threads;	/* live threads' blocks */
static struct dto_stats stats_retired;		/* exited threads, summed */
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t stats_key;
static bool stats_key_created;
static __thread struct dto_thread_stats *thr_stats;
static __thread bool thr_stats_off;	/* don't count this thread's calls */

/* output: one formatter at a time (sampler, exit, SIGINT) */
static pthread_mutex_t stats_out_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dto_stats stats_scratch;
enum { STATS_FORMAT_TEXT, STATS_FORMAT_JSON };
static int stats_format;

static bool stats_file_per_pid;	/* DTO_STATS_FILE has a %p */

/* DTO_STATS_INTERVAL_MS */
static unsigned long stats_interval_ms;
static atomic_uchar sampler_started;
static bool stats_shutdown;	/* cleanup_dto ran: no new sampler */
static struct dto_stats sampler_prev;	/* totals at the previous record */
static uint64_t sampler_prev_ns;

static void print_stats(void);
static void print_stats_from_signal(void);
static void stats_fork_child(void);
#endif

/* call initialize/cleanup functions when library is loaded/unloaded */
static int init_dto(void) __attribute__((constructor));
static void cleanup_dto(void) __attribute__((destructor));

static int waitpkg_support;

static enum {
	LOG_LEVEL_FATAL,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_TRACE
} log_levels;

#define LOG_FATAL(...) dto_log(LOG_LEVEL_FATAL, __VA_ARGS__)
#define LOG_ERROR(...) dto_log(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_TRACE(...) dto_log(LOG_LEVEL_TRACE, __VA_ARGS__)

static unsigned int log_level = LOG_LEVEL_FATAL;

#ifdef DTO_STATS_SUPPORT
static struct sigaction previous_sigint_action;
static volatile sig_atomic_t sigint_handler_installed;
static int previous_sigint_action_valid;

static void restore_sigint_handler(void)
{
        if (!sigint_handler_installed)
                return;

        if (previous_sigint_action_valid)
                sigaction(SIGINT, &previous_sigint_action, NULL);
        else {
                struct sigaction default_action = {0};

                default_action.sa_handler = SIG_DFL;
                sigaction(SIGINT, &default_action, NULL);
        }

        sigint_handler_installed = 0;
}

static void dto_handle_sigint(int signo)
{
        print_stats_from_signal();

        restore_sigint_handler();

        if (previous_sigint_action_valid && previous_sigint_action.sa_handler == SIG_IGN)
                return;

        raise(signo);
}

static void install_sigint_handler(void)
{
        struct sigaction action = {0};

        if (sigint_handler_installed)
                return;

        action.sa_handler = dto_handle_sigint;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;

        if (sigaction(SIGINT, NULL, &previous_sigint_action) == 0)
                previous_sigint_action_valid = 1;
        else
                previous_sigint_action_valid = 0;

        if (sigaction(SIGINT, &action, NULL) == 0)
                sigint_handler_installed = 1;
}
#endif

/* Auto tune heuristics magic numbers */
#define DESCS_PER_RUN 0xF0
#define NUM_DESCS 16
#define MIN_AVG_YIELD_WAITS 1.0
#define MAX_AVG_YIELD_WAITS 2.0
#define MIN_AVG_POLL_WAITS 5.0
#define MAX_AVG_POLL_WAITS 20.0
#define MAX_CPU_SIZE_FRACTION 90  // specified in percent (e.g., 90 is 0.90)
#define CSF_STEP_INCREMENT 1
#define CSF_STEP_DECREMENT 1
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
	stats_fork_child();
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

static __always_inline void dsa_wait_yield(const volatile uint8_t *comp)
{
	while (*comp == 0) {
	    sched_yield();
	}
}

static __always_inline void dsa_wait_busy_poll(const volatile uint8_t *comp)
{
	while (*comp == 0) {
	    _mm_pause();
	}
}

static __always_inline void dsa_wait_tpause(const volatile uint8_t *comp)
{
	while (*comp == 0) {
            uint64_t delay = _rdtsc() + tpause_wait_time;
            _tpause(C02_STATE, delay);
        }
}

static __always_inline void __dsa_wait_umwait(const volatile uint8_t *comp)
{
	_umonitor((void*)comp);

        uint64_t delay = _rdtsc() + UMWAIT_DELAY_DEFAULT;
	_umwait(C02_STATE, delay);
}

static __always_inline void dsa_wait_umwait(const volatile uint8_t *comp)
{
	while (*comp == 0) {
	    __dsa_wait_umwait(comp);
        }
}

static __always_inline void __dsa_wait(const volatile uint8_t *comp)
{
        switch(wait_method) {
            case WAIT_YIELD:
		sched_yield();
                break;
            case WAIT_UMWAIT:
                __dsa_wait_umwait(comp);
                break;
            case WAIT_TPAUSE:
                _tpause( C01_STATE, _rdtsc() + TPAUSE_C01_DELAY_NS); 
                break;
            default:
                 _mm_pause();
        }
}

static __always_inline void dsa_wait_no_adjust(const volatile uint8_t *comp)
{
    switch (wait_method) {
        case WAIT_YIELD:
            dsa_wait_yield(comp);
            break;
        case WAIT_UMWAIT:
            dsa_wait_umwait(comp);
            break;
        case WAIT_BUSYPOLL:
            dsa_wait_busy_poll(comp);
            break;
        case WAIT_TPAUSE:
            dsa_wait_tpause(comp);
            break;
        default:
            dsa_wait_busy_poll(comp);
    }
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
		while (*comp == 0) {
			__dsa_wait(comp);
                }

		return;
	}

	/* Run the heuristics as well as wait for DSA */
	while (*comp == 0) {
		__dsa_wait(comp);
		local_num_waits++;
	}

	// operations that have failed (mostly due to page fault) return very quickly and cause the algorithm
	// to think that the DSA operation was faster than it really was. We exclude them from the calculation.
	if (*comp != DSA_COMP_SUCCESS) {
		return;
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
	int ret;
	//LOG_TRACE("desc flags: 0x%x, opcode: 0x%x\n", hw->flags, hw->opcode);
	__builtin_ia32_sfence();

	if (wq->wq_mmapped) {
		ret = enqcmd(hw, wq->wq_portal);
		if (!ret)
			return SUCCESS;
	} else {
		ret = write(wq->wq_fd, hw, sizeof(*hw));
		if (ret == sizeof(*hw))
			return SUCCESS;
		else
			return FAIL_OTHERS;
	}
	return RETRY;
}

static __always_inline int dsa_execute(struct dto_wq *wq,
	struct dsa_hw_desc *hw, volatile uint8_t *comp)
{
	int ret;
	*comp = 0;
	//LOG_TRACE("desc flags: 0x%x, opcode: 0x%x\n", hw->flags, hw->opcode);
	__builtin_ia32_sfence();

	if (wq->wq_mmapped)
		ret = enqcmd(hw, wq->wq_portal);

	else {
		ret = write(wq->wq_fd, hw, sizeof(*hw));
		if (ret != sizeof(*hw))
			return FAIL_OTHERS;
		else
			ret = 0;
	}
	if (!ret) {
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
	return RETRY;
}

#ifdef DTO_STATS_SUPPORT
/*
 * Statistics (DTO_COLLECT_STATS).
 *
 * Each thread counts into its own block, so an update is a few plain loads
 * and stores with no shared cache line; the blocks are summed when stats are
 * printed. A block is mmap'd on the thread's first update (only the pages it
 * touches are ever allocated), registered in a list, and on thread exit
 * folded into stats_retired and unmapped. The totals sit at the start of the
 * block, so the interval sampler reads one page per thread.
 */

/* Histogram bucket of a call of n bytes. */
static __always_inline int stats_bucket(size_t n)
{
	int e;

	if (n < (1UL << HIST_MIN_SHIFT))
		return 0;
	e = 63 - __builtin_clzl(n);
	if (e >= HIST_MAX_SHIFT)
		return HIST_NO_BUCKETS - 1;
	return 1 + (e - HIST_MIN_SHIFT) * HIST_SUBS +
		(int)((n >> (e - 2)) & (HIST_SUBS - 1));
}

/* Smallest size counted in bucket b. */
static uint64_t stats_bucket_min(int b)
{
	int e;

	if (b == 0)
		return 0;
	if (b == HIST_NO_BUCKETS - 1)
		return 1ULL << HIST_MAX_SHIFT;
	e = HIST_MIN_SHIFT + (b - 1) / HIST_SUBS;
	return (uint64_t)(HIST_SUBS + (b - 1) % HIST_SUBS) << (e - 2);
}

/* Relaxed load-add-store: the owning thread is the only writer, and
 * readers summing blocks only need each counter to be read whole. */
#define STAT_ADD(field, v) \
	__atomic_store_n(&(field), __atomic_load_n(&(field), __ATOMIC_RELAXED) + (v), \
			 __ATOMIC_RELAXED)
#define STAT_GET(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)

static __always_inline void stats_add_op(struct dto_op_stats *s, uint64_t bytes,
					 uint64_t ns)
{
	STAT_ADD(s->calls, 1);
	STAT_ADD(s->bytes, bytes);
	STAT_ADD(s->ns, ns);
}

/* dst += src for the totals, or for everything with hist */
static void stats_sum(struct dto_stats *dst, struct dto_stats *src, bool hist)
{
	for (int g = 0; g < STAT_GROUPS; g++)
		for (int o = 0; o < MAX_MEMOP; o++) {
			dst->total[g][o].calls += STAT_GET(src->total[g][o].calls);
			dst->total[g][o].bytes += STAT_GET(src->total[g][o].bytes);
			dst->total[g][o].ns += STAT_GET(src->total[g][o].ns);
		}
	for (int f = 0; f < MAX_FAILURES; f++)
		dst->fails[f] += STAT_GET(src->fails[f]);
	if (!hist)
		return;
	for (int b = 0; b < HIST_NO_BUCKETS; b++) {
		struct dto_bucket_stats *d = &dst->hist[b], *s = &src->hist[b];

		for (int g = 0; g < STAT_GROUPS; g++)
			for (int o = 0; o < MAX_MEMOP; o++) {
				d->op[g][o].calls += STAT_GET(s->op[g][o].calls);
				d->op[g][o].bytes += STAT_GET(s->op[g][o].bytes);
				d->op[g][o].ns += STAT_GET(s->op[g][o].ns);
			}
		for (int f = 0; f < MAX_FAILURES; f++)
			d->fails[f] += STAT_GET(s->fails[f]);
	}
}

/* Sum of every thread's stats, live and exited, into *out (zeroed first).
 * The caller holds stats_lock. */
static void stats_collect(struct dto_stats *out, bool hist)
{
	size_t len = hist ? sizeof(*out) : offsetof(struct dto_stats, hist);

	orig_memset(out, 0, len);
	stats_sum(out, &stats_retired, hist);
	for (struct dto_thread_stats *t = stats_threads; t; t = t->next)
		stats_sum(out, &t->s, hist);
}

static void stats_snapshot(struct dto_stats *out, bool hist)
{
	pthread_mutex_lock(&stats_lock);
	stats_collect(out, hist);
	pthread_mutex_unlock(&stats_lock);
}

static void stats_start_sampler(void);

static void stats_thread_exit(void *arg)
{
	struct dto_thread_stats *ts = arg;

	/* memcpy and friends may still run during the rest of thread exit */
	thr_stats_off = true;
	thr_stats = NULL;
	pthread_mutex_lock(&stats_lock);
	stats_sum(&stats_retired, &ts->s, true);
	if (ts->prev)
		ts->prev->next = ts->next;
	else
		stats_threads = ts->next;
	if (ts->next)
		ts->next->prev = ts->prev;
	pthread_mutex_unlock(&stats_lock);
	munmap(ts, sizeof(*ts));
}

/* First update on this thread: give it a block. NULL leaves the thread
 * uncounted (thr_stats_off stays set, so it is not retried every call). */
static struct dto_thread_stats *stats_thread_register(void)
{
	struct dto_thread_stats *ts;

	/* mmap, the registry and pthread_setspecific must not count themselves */
	thr_stats_off = true;
	ts = mmap(NULL, sizeof(*ts), PROT_READ | PROT_WRITE,
		  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ts == MAP_FAILED)
		return NULL;

	pthread_mutex_lock(&stats_lock);
	ts->prev = NULL;
	ts->next = stats_threads;
	if (stats_threads)
		stats_threads->prev = ts;
	stats_threads = ts;
	pthread_mutex_unlock(&stats_lock);
	pthread_setspecific(stats_key, ts);
	thr_stats = ts;

	if (stats_interval_ms && !stats_shutdown &&
	    !atomic_load_explicit(&sampler_started,
						       memory_order_acquire))
		stats_start_sampler();
	thr_stats_off = false;
	return ts;
}

static void update_stats(int op, size_t n, bool overlapping, size_t bytes_completed,
		uint64_t elapsed_ns, int group, int error_code)
{
	struct dto_thread_stats *ts;
	struct dto_bucket_stats *b;

	// dto_memcpymove didn't actually submit the request to DSA, so there is nothing to log. This will be captured by a second call
	if (op == MEMMOVE && overlapping && dto_overlapping_memmove_action == OVERLAPPING_CPU && group == DSA_CALL_SUCCESS) {
		return;
	}
	if (unlikely(thr_stats_off))
		return;
	ts = thr_stats;
	if (unlikely(ts == NULL)) {
		ts = stats_thread_register();
		if (ts == NULL)
			return;
	}

	b = &ts->s.hist[stats_bucket(n)];
	stats_add_op(&ts->s.total[group][op], bytes_completed, elapsed_ns);
	stats_add_op(&b->op[group][op], bytes_completed, elapsed_ns);
	if (group == DSA_CALL_FAILED) {
		STAT_ADD(ts->s.fails[error_code], 1);
		STAT_ADD(b->fails[error_code], 1);
	}
}

/* ---- Output ----
 *
 * Records are formatted into stats_out and written with as few write()s as
 * possible: to DTO_STATS_FILE (opened O_APPEND, so records from several
 * processes sharing it stay whole), else DTO_LOG_FILE, else stdout. */

/* Holds a whole record, so a summary (up to ~1 KB for each of the 106
 * histogram buckets in JSON) goes out in one write() and records from
 * several processes sharing an O_APPEND file stay whole. */
static char stats_out[1 << 18];
static size_t stats_out_len;

static void stats_flush(void)
{
	int fd = stats_fd != -1 ? stats_fd : log_fd != -1 ? log_fd : STDOUT_FILENO;
	size_t off = 0;

	while (off < stats_out_len) {
		ssize_t r = write(fd, stats_out + off, stats_out_len - off);

		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && errno == EAGAIN) {
			struct pollfd pfd = { .fd = fd, .events = POLLOUT };

			if (poll(&pfd, 1, -1) > 0 || errno == EINTR)
				continue;
		}
		if (r <= 0)
			break;
		off += r;
	}
	stats_out_len = 0;
}

__attribute__((format(printf, 1, 2)))
static void stats_printf(const char *fmt, ...)
{
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(stats_out + stats_out_len, sizeof(stats_out) - stats_out_len,
			fmt, args);
	va_end(args);
	if (len < 0)
		return;
	if ((size_t)len >= sizeof(stats_out) - stats_out_len) {
		/* no room: write out what is there and format again */
		stats_flush();
		va_start(args, fmt);
		len = vsnprintf(stats_out, sizeof(stats_out), fmt, args);
		va_end(args);
		if (len < 0)
			return;
		if ((size_t)len >= sizeof(stats_out))
			len = sizeof(stats_out) - 1;
	}
	stats_out_len += len;
}

/* DTO_STATS_FILE, with each %p replaced by the process ID. It is opened
 * O_APPEND, not truncated: processes started from this one (fork, exec)
 * inherit the setting, and with %p each gets a file of its own, without it
 * they append whole records to a shared one. */
static void stats_open_file(const char *name)
{
	char path[PATH_MAX];
	size_t j = 0;
	struct stat st;

	for (const char *c = name; *c && j < sizeof(path) - 1; c++) {
		if (c[0] == '%' && c[1] == 'p') {
			stats_file_per_pid = true;
			int n = snprintf(path + j, sizeof(path) - j, "%d", getpid());

			if (n < 0 || (size_t)n >= sizeof(path) - j)
				break;
			j += n;
			c++;
		} else {
			path[j++] = *c;
		}
	}
	path[j] = '\0';

	if (lstat(path, &st) == 0 && (st.st_mode & S_IFMT) != S_IFREG) {
		LOG_ERROR("DTO_STATS_FILE '%s' must reference a regular file\n", path);
		return;
	}
	stats_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (stats_fd == -1)
		LOG_ERROR("Failed to open DTO_STATS_FILE '%s': %s\n", path, strerror(errno));
}

static const char * const memop_json[] = {
	[MEMSET] = "memset",
	[MEMCOPY] = "memcpy",
	[MEMMOVE] = "memmove",
	[MEMCMP] = "memcmp",
};

static const char * const group_json[] = {
	[STDC_CALL] = "cpu",
	[DSA_CALL_SUCCESS] = "dsa",
	[DSA_CALL_FAILED] = "dsa_failed",
};

static const char * const failure_json[] = {
	[RETRY] = "retry",
	[PAGE_FAULT] = "page_fault",
	[FAIL_OTHERS] = "other",
};

static uint64_t stats_now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_BOOTTIME, &now);
	return TS_NS(dto_start_time, now);
}

/* {"memcpy":{"dsa":{"calls":..,"bytes":..,"ns":..},..},..} without zeroes */
static void stats_json_ops(struct dto_op_stats op[STAT_GROUPS][MAX_MEMOP])
{
	bool first_op = true;

	stats_printf("{");
	for (int o = 0; o < MAX_MEMOP; o++) {
		bool first_group = true;

		for (int g = 0; g < STAT_GROUPS; g++) {
			if (!op[g][o].calls)
				continue;
			if (first_group)
				stats_printf("%s\"%s\":{", first_op ? "" : ",", memop_json[o]);
			stats_printf("%s\"%s\":{\"calls\":%lu,\"bytes\":%lu,\"ns\":%lu}",
				     first_group ? "" : ",", group_json[g], op[g][o].calls,
				     op[g][o].bytes, op[g][o].ns);
			first_group = false;
			first_op = false;
		}
		if (!first_group)
			stats_printf("}");
	}
	stats_printf("}");
}

static void stats_json_fails(uint64_t fails[MAX_FAILURES])
{
	stats_printf("{");
	for (int f = 1; f < MAX_FAILURES; f++)
		stats_printf("%s\"%s\":%lu", f == 1 ? "" : ",", failure_json[f], fails[f]);
	stats_printf("}");
}

/* Text: "memcpy dsa 120 calls 7864320 B 421.3 us, cpu ..." */
static void stats_text_ops(struct dto_op_stats op[STAT_GROUPS][MAX_MEMOP])
{
	bool any = false;

	for (int o = 0; o < MAX_MEMOP; o++)
		for (int g = 0; g < STAT_GROUPS; g++) {
			if (!op[g][o].calls)
				continue;
			stats_printf("%s%s %s %lu calls %lu B %.1f us", any ? ", " : "",
				     memop_json[o], group_json[g], op[g][o].calls,
				     op[g][o].bytes, op[g][o].ns / 1000.0);
			any = true;
		}
	if (!any)
		stats_printf("no calls");
}

/* One interval record: the change in the totals since the previous one.
 * The caller holds stats_out_lock. */
static void stats_emit_interval(void)
{
	struct dto_stats *now = &stats_scratch;	/* totals part only */
	struct dto_op_stats delta[STAT_GROUPS][MAX_MEMOP];
	uint64_t fails[MAX_FAILURES];
	uint64_t t = stats_now_ns();

	stats_snapshot(now, false);
	for (int g = 0; g < STAT_GROUPS; g++)
		for (int o = 0; o < MAX_MEMOP; o++) {
			delta[g][o].calls = now->total[g][o].calls - sampler_prev.total[g][o].calls;
			delta[g][o].bytes = now->total[g][o].bytes - sampler_prev.total[g][o].bytes;
			delta[g][o].ns = now->total[g][o].ns - sampler_prev.total[g][o].ns;
		}
	for (int f = 0; f < MAX_FAILURES; f++)
		fails[f] = now->fails[f] - sampler_prev.fails[f];
	orig_memcpy(&sampler_prev, now, offsetof(struct dto_stats, hist));

	if (stats_format == STATS_FORMAT_JSON) {
		stats_printf("{\"type\":\"interval\",\"pid\":%d,\"t_ms\":%.3f,\"dt_ms\":%.3f,\"ops\":",
			     getpid(), t / 1e6, (t - sampler_prev_ns) / 1e6);
		stats_json_ops(delta);
		stats_printf(",\"failures\":");
		stats_json_fails(fails);
		stats_printf("}\n");
	} else {
		stats_printf("DTO stats t=%.3f ms (+%.3f ms): ", t / 1e6, (t - sampler_prev_ns) / 1e6);
		stats_text_ops(delta);
		if (fails[RETRY] || fails[PAGE_FAULT] || fails[FAIL_OTHERS])
			stats_printf("; failures retry %lu page_fault %lu other %lu",
				     fails[RETRY], fails[PAGE_FAULT], fails[FAIL_OTHERS]);
		stats_printf("\n");
	}
	sampler_prev_ns = t;
	stats_flush();
}

static bool stats_bucket_empty(struct dto_bucket_stats *b)
{
	for (int g = 0; g < STAT_GROUPS; g++)
		for (int o = 0; o < MAX_MEMOP; o++)
			if (b->op[g][o].calls)
				return false;
	return true;
}

static void stats_print_json(struct dto_stats *s, uint64_t run_ns)
{
	char prog[64];
	bool first = true;
	size_t j = 0;

	/* the program name as a JSON string: drop quotes, backslashes and
	 * control characters rather than escape them */
	for (const char *c = __progname; *c && j < sizeof(prog) - 1; c++)
		if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20)
			prog[j++] = *c;
	prog[j] = '\0';
	stats_printf("{\"type\":\"summary\",\"pid\":%d,\"program\":\"%s\",\"run_ms\":%.3f,",
		     getpid(), prog, run_ns / 1e6);
	stats_printf("\"config\":{\"stdc_only\":%d,\"wait_method\":\"%s\",\"min_bytes\":%lu,"
		     "\"cpu_size_fraction\":%.2f,\"auto_adjust\":%d,\"numa_aware\":%d,\"wqs\":%d},",
		     use_std_lib_calls, wait_names[wait_method], dsa_min_size,
		     cpu_size_fraction / 100.0, auto_adjust_knobs, (int)is_numa_aware, num_wqs);
	stats_printf("\"totals\":");
	stats_json_ops(s->total);
	stats_printf(",\"failures\":");
	stats_json_fails(s->fails);
	stats_printf(",\"histogram\":[");
	for (int b = 0; b < HIST_NO_BUCKETS; b++) {
		if (stats_bucket_empty(&s->hist[b]))
			continue;
		stats_printf("%s{\"min\":%lu,", first ? "" : ",", stats_bucket_min(b));
		if (b < HIST_NO_BUCKETS - 1)
			stats_printf("\"max\":%lu,", stats_bucket_min(b + 1) - 1);
		stats_printf("\"ops\":");
		stats_json_ops(s->hist[b].op);
		stats_printf(",\"failures\":");
		stats_json_fails(s->hist[b].fails);
		stats_printf("}");
		first = false;
	}
	stats_printf("]}\n");
}

static void stats_print_text(struct dto_stats *s, uint64_t run_ns)
{
	stats_printf("DTO Run Time: %lu ms\n", run_ns / 1000000);

	for (int t = 0; t < 2; ++t) {
		if (t == 0)
			stats_printf("\n******** Number of Memory Operations ********\n");
		else
			stats_printf("\n******** Average Memory Operation Latency (us)  ********\n");

		stats_printf("%23s    ", "");
		for (int g = 0; g < MAX_STAT_GROUP; ++g) {
			if (t == 0) {
				if (g == DSA_FAIL_CODES)
					stats_printf("<***** %-13s *****> ", stat_group_names[g]);
				else
					stats_printf("<*************** %-13s ***************> ", stat_group_names[g]);
			} else if (g != DSA_FAIL_CODES) {
				stats_printf("<******** %-13s ********> ", stat_group_names[g]);
			}
		}
		stats_printf("\n%-23s -- ", "Byte Range");
		for (int g = 0; g < STAT_GROUPS; ++g) {
			for (int o = 0; o < MAX_MEMOP; ++o)
				stats_printf("%-8s ", memop_names[o]);
			if (t == 0)
				stats_printf("%-12s ", "bytes");
		}
		if (t == 0)
			for (int o = 1; o < MAX_FAILURES; ++o)
				stats_printf("%-6s ", failure_names[o]);
		stats_printf("\n");

		for (int b = 0; b < HIST_NO_BUCKETS; ++b) {
			struct dto_bucket_stats *h = &s->hist[b];

			if (stats_bucket_empty(h))
				continue;
			if (b < HIST_NO_BUCKETS - 1)
				stats_printf("%11lu-%-11lu -- ", stats_bucket_min(b),
					     stats_bucket_min(b + 1) - 1);
			else
				stats_printf("   >=%-18lu -- ", stats_bucket_min(b));

			for (int g = 0; g < STAT_GROUPS; ++g) {
				uint64_t bytes = 0;

				for (int o = 0; o < MAX_MEMOP; ++o) {
					struct dto_op_stats *op = &h->op[g][o];

					bytes += op->bytes;
					if (t == 0)
						stats_printf("%-8lu ", op->calls);
					else if (op->calls)
						stats_printf("%-8.2f ", op->ns / (op->calls * 1000.0));
					else
						stats_printf("%-8d ", 0);
				}
				if (t == 0)
					stats_printf("%-12lu ", bytes);
			}
			if (t == 0)
				for (int o = 1; o < MAX_FAILURES; ++o)
					stats_printf("%-6lu ", h->fails[o]);
			stats_printf("\n");
		}
	}
}

/* The end-of-run summary of *s. The caller holds stats_out_lock. */
static void stats_print_summary(struct dto_stats *s)
{
	if (stats_format == STATS_FORMAT_JSON)
		stats_print_json(s, stats_now_ns());
	else
		stats_print_text(s, stats_now_ns());
	stats_flush();
}

static void print_stats(void)
{
	bool off = thr_stats_off;

	if (likely(!collect_stats))
		return;
	thr_stats_off = true;
	pthread_mutex_lock(&stats_out_lock);
	stats_snapshot(&stats_scratch, true);
	stats_print_summary(&stats_scratch);
	pthread_mutex_unlock(&stats_out_lock);
	thr_stats_off = off;
}

/* From the SIGINT handler: the interrupted thread may hold a stats lock, so
 * print only if both can be taken right away. */
static void print_stats_from_signal(void)
{
	bool off = thr_stats_off;

	if (!collect_stats)
		return;
	thr_stats_off = true;
	if (pthread_mutex_trylock(&stats_out_lock) == 0) {
		if (pthread_mutex_trylock(&stats_lock) == 0) {
			stats_collect(&stats_scratch, true);
			pthread_mutex_unlock(&stats_lock);
			stats_print_summary(&stats_scratch);
		}
		pthread_mutex_unlock(&stats_out_lock);
	}
	thr_stats_off = off;
}

/* ---- Interval sampler (DTO_STATS_INTERVAL_MS) ---- */

static pthread_t sampler_thread;
static pthread_mutex_t sampler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sampler_cv;
static bool sampler_stop;

static void *stats_sampler(void *arg)
{
	struct timespec next;

	thr_stats_off = true;	/* its own formatting is not the program's */
	clock_gettime(CLOCK_MONOTONIC, &next);
	pthread_mutex_lock(&sampler_lock);
	while (!sampler_stop) {
		next.tv_nsec += (long)(stats_interval_ms % 1000) * 1000000;
		next.tv_sec += stats_interval_ms / 1000 + next.tv_nsec / 1000000000;
		next.tv_nsec %= 1000000000;
		while (!sampler_stop &&
		       pthread_cond_timedwait(&sampler_cv, &sampler_lock, &next) != ETIMEDOUT)
			;
		if (sampler_stop)
			break;
		pthread_mutex_unlock(&sampler_lock);
		pthread_mutex_lock(&stats_out_lock);
		stats_emit_interval();
		pthread_mutex_unlock(&stats_out_lock);
		pthread_mutex_lock(&sampler_lock);
	}
	pthread_mutex_unlock(&sampler_lock);
	return NULL;
}

/* Started by the first counted call (not from the constructor or the fork
 * handler, where creating a thread is not safe), at most once per process. */
static void stats_start_sampler(void)
{
	unsigned char expected = 0;
	pthread_condattr_t attr;
	sigset_t all, old;

	if (!atomic_compare_exchange_strong(&sampler_started, &expected, 1))
		return;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&sampler_cv, &attr);
	pthread_condattr_destroy(&attr);
	sampler_stop = false;
	/* the sampler must not take the program's signals */
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	if (pthread_create(&sampler_thread, NULL, stats_sampler, NULL))
		atomic_store(&sampler_started, 0);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void stats_stop_sampler(void)
{
	if (!atomic_load(&sampler_started))
		return;
	pthread_mutex_lock(&sampler_lock);
	sampler_stop = true;
	pthread_cond_signal(&sampler_cv);
	pthread_mutex_unlock(&sampler_lock);
	pthread_join(sampler_thread, NULL);
	atomic_store(&sampler_started, 0);
	/* the part of the last interval since the sampler's final record */
	pthread_mutex_lock(&stats_out_lock);
	stats_emit_interval();
	pthread_mutex_unlock(&stats_out_lock);
}

/* ---- fork ----
 *
 * The forking thread holds both stats locks across fork() so the child never
 * inherits one held by a thread that does not exist there. */
static void stats_fork_prepare(void)
{
	pthread_mutex_lock(&stats_out_lock);
	pthread_mutex_lock(&stats_lock);
	pthread_mutex_lock(&sampler_lock);
}

static void stats_fork_parent(void)
{
	pthread_mutex_unlock(&sampler_lock);
	pthread_mutex_unlock(&stats_lock);
	pthread_mutex_unlock(&stats_out_lock);
}

/* In the child: drop every block (their threads are gone, and the child
 * counts from zero) and forget the parent's sampler. */
static void stats_fork_child(void)
{
	struct dto_thread_stats *t = stats_threads, *next;

	pthread_mutex_unlock(&sampler_lock);
	pthread_mutex_unlock(&stats_lock);
	pthread_mutex_unlock(&stats_out_lock);
	for (; t; t = next) {
		next = t->next;
		munmap(t, sizeof(*t));
	}
	stats_threads = NULL;
	orig_memset(&stats_retired, 0, sizeof(stats_retired));
	orig_memset(&sampler_prev, 0, sizeof(sampler_prev));
	sampler_prev_ns = 0;
	if (stats_key_created)
		pthread_setspecific(stats_key, NULL);
	thr_stats = NULL;
	thr_stats_off = false;
	sampler_stop = false;
	atomic_store(&sampler_started, 0);
	stats_out_len = 0;
	/* with %p the child writes a file of its own: init_dto opens it */
	if (stats_file_per_pid && stats_fd != -1) {
		close(stats_fd);
		stats_fd = -1;
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

#ifdef DTO_NUMA_SUPPORT
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
	struct dto_device* dev = NULL;
	for (uint8_t i = 0; i < MAX_NUMA_NODES; i++) {
		if (devices[i] != NULL) {
			dev = devices[i];
		} else {
			devices[i] = dev;
		}
	}
}
#endif

#ifdef DTO_NUMA_SUPPORT
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
#endif

#ifdef DTO_NUMA_SUPPORT
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
#endif

static bool test_write_syscall(struct dto_wq *wq)
{
	struct dsa_hw_desc desc = {0};
	struct dsa_completion_record comp __attribute__((aligned(32)));
	int retry = 0;

	desc.opcode = DSA_OPCODE_NOOP;
	desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	comp.status = 0;

	desc.completion_addr = (unsigned long)&comp;

	if (dsa_submit(wq, &desc) == SUCCESS) {
		while (comp.status == 0 && retry++ < 10000)
			_mm_pause();

		if (comp.status == DSA_COMP_SUCCESS)
			return true;
	}

	return false;
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

#ifdef DTO_NUMA_SUPPORT
		const int dev_numa_node = (int)dto_get_param_ullong(dir_fd, "numa_node", &rc);
		if (rc) {
			close(dir_fd);
			goto fail_wq;
		}
#endif

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

		wqs[num_wqs].max_batch_size = dto_get_param_ullong(dir_fd, "max_batch_size", &rc);
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

		if (wqs[num_wqs].wq_portal == MAP_FAILED) {
			/* In case the driver doesn't support mmap, test if it
			 * supports write system call for work submission, and
			 * if yes, fallback to using write syscall.
			 */

			rc = -errno;
			if (test_write_syscall(&wqs[num_wqs]))
				wqs[num_wqs].wq_mmapped = false;
			else {
				LOG_ERROR("mmap error for DSA wq: %s, error: %s\n", wqs[num_wqs].wq_path, strerror(errno));
				goto fail_wq;
			}
		} else {
			wqs[num_wqs].wq_mmapped = true;
			close(wqs[num_wqs].wq_fd);
		}

#ifdef DTO_NUMA_SUPPORT
		if (is_numa_aware) {
			struct dto_device* dev = get_dto_device(dev_numa_node);
			if (dev != NULL &&
				dev->num_wqs < MAX_WQS) {
				dev->wqs[dev->num_wqs++] = &wqs[num_wqs];
			}
		}
#endif

		++num_wqs;
		if (num_wqs == MAX_WQS)
			break;

		wq = strtok(NULL, ";");
	}

	if (num_wqs == 0) {
		rc = -EINVAL;
		goto fail;
	}

#ifdef DTO_NUMA_SUPPORT
	if (is_numa_aware) {
		correct_devices_list();
	}
#endif

	return 0;

fail_wq:
	for (int j = 0; j < num_wqs; j++)
		munmap(wqs[j].wq_portal, 0x1000);
	num_wqs = 0;

#ifdef DTO_NUMA_SUPPORT
	cleanup_devices();
#endif

fail:
	return rc;
}

#ifdef DTO_ACCEL_CONFIG_SUPPORT
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

#ifdef DTO_NUMA_SUPPORT
		struct dto_device* dev = NULL;

		if (is_numa_aware) {
			const int dev_numa_node = accfg_device_get_numa_node(device);
			dev = get_dto_device(dev_numa_node);
		}
#endif

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
			wqs[num_wqs].max_batch_size = accfg_wq_get_max_batch_size(wq);

			wqs[num_wqs].acc_wq = wq;
			wqs[num_wqs].dsa_gencap = accfg_device_get_gen_cap(device);

			used_devids[num_wqs] = accfg_device_get_id(device);

#ifdef DTO_NUMA_SUPPORT
			if (is_numa_aware &&
				dev != NULL &&
				dev->num_wqs < MAX_WQS) {
				dev->wqs[dev->num_wqs++] = &wqs[num_wqs];
			}
#endif

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

		if (wqs[i].wq_portal == MAP_FAILED) {
			/* In case the driver doesn't support mmap, test if it
			 * supports write system call for work submission, and
			 * if yes, fallback to using write syscall.
			 */
			rc = -errno;
			if (test_write_syscall(&wqs[i]))
				wqs[num_wqs].wq_mmapped = false;
			else {
				LOG_ERROR("mmap error for DSA wq: %s, error: %s\n", wqs[i].wq_path, strerror(errno));
				goto fail_wq;
			}
		} else {
			wqs[i].wq_mmapped = true;
			close(wqs[i].wq_fd);
		}
	}

#ifdef DTO_NUMA_SUPPORT
	if (is_numa_aware) {
		correct_devices_list();
	}
#endif

	accfg_unref(dto_ctx);
	return 0;

fail_wq:
	for (int j = 0; j < i; j++)
		munmap(wqs[j].wq_portal, 0x1000);
	num_wqs = 0;

#ifdef DTO_NUMA_SUPPORT
	cleanup_devices();
#endif
fail:
	accfg_unref(dto_ctx);
	return rc;
}
#endif /* DTO_ACCEL_CONFIG_SUPPORT */

static int dsa_init(void)
{
	unsigned int unused[2];
	unsigned int leaf, waitpkg;
	const char *env_str;
	char wq_list[256];

	/* detect waitpkg support: CPUID.(EAX=7,ECX=0):ECX bit 5 */
	leaf = 0;
	waitpkg = 0;
	if (__get_cpuid_count(7, 0, &leaf, unused, &waitpkg, unused + 1)) {
		if (waitpkg & 0x20) {
			LOG_TRACE("waitpkg supported\n");
			waitpkg_support = 1;
		}
	}

	env_str = getenv("DTO_WAIT_METHOD");
	if (env_str != NULL) {
		if (!strncmp(env_str, wait_names[WAIT_BUSYPOLL], strlen(wait_names[WAIT_BUSYPOLL]))) {
			wait_method = WAIT_BUSYPOLL;
			min_avg_waits = MIN_AVG_POLL_WAITS;
			max_avg_waits = MAX_AVG_POLL_WAITS;
		} else if (!strncmp(env_str, wait_names[WAIT_UMWAIT], strlen(wait_names[WAIT_UMWAIT]))) {
			if (waitpkg_support) {
				wait_method = WAIT_UMWAIT;
				/* Use the same waits as busypoll for now */
				min_avg_waits = MIN_AVG_POLL_WAITS;
				max_avg_waits = MAX_AVG_POLL_WAITS;
			} else
				LOG_ERROR("umwait not supported. Falling back to default wait method\n");
		} else if (!strncmp(env_str, wait_names[WAIT_YIELD], strlen(wait_names[WAIT_YIELD]))) {
			wait_method = WAIT_YIELD;
			min_avg_waits = MIN_AVG_YIELD_WAITS;
			max_avg_waits = MAX_AVG_YIELD_WAITS;
		} else if (!strncmp(env_str, wait_names[WAIT_TPAUSE], strlen(wait_names[WAIT_TPAUSE]))) {
		    if (waitpkg_support) {
			wait_method = WAIT_TPAUSE;
                    } else {
			LOG_ERROR("tpause not supported. Falling back to busypoll\n");
                        wait_method = WAIT_BUSYPOLL;
                    }
                }
	}

	env_str = getenv("DTO_WQ_LIST");
	if (env_str == NULL) {
#ifdef DTO_ACCEL_CONFIG_SUPPORT
		return dsa_init_from_accfg();
#else
		LOG_ERROR("DTO_WQ_LIST environment variable must be set when accel-config support is disabled.\n");
		LOG_ERROR("Example: export DTO_WQ_LIST=\"wq0.0;wq1.0\"\n");
		return -EINVAL;
#endif
	}

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

		env_str = getenv("DTO_DSA_MEMCPY");
		if (env_str != NULL) {
			errno = 0;
			dto_dsa_memcpy = strtoul(env_str, NULL, 10);
			if (errno)
				dto_dsa_memcpy = 0;

			dto_dsa_memcpy = !!dto_dsa_memcpy;
		}

		env_str = getenv("DTO_DSA_CC");
		if (env_str != NULL) {
			errno = 0;
			dto_dsa_cc = strtoul(env_str, NULL, 10);
			if (errno)
				dto_dsa_cc = 0;

			dto_dsa_cc = !!dto_dsa_cc;
		}

		env_str = getenv("DTO_DSA_MEMMOVE");
		if (env_str != NULL) {
			errno = 0;
			dto_dsa_memmove = strtoul(env_str, NULL, 10);
			if (errno)
				dto_dsa_memmove = 0;

			dto_dsa_memmove = !!dto_dsa_memmove;
		}

		env_str = getenv("DTO_DSA_MEMSET");
		if (env_str != NULL) {
			errno = 0;
			dto_dsa_memset = strtoul(env_str, NULL, 10);
			if (errno)
				dto_dsa_memset = 0;

			dto_dsa_memset = !!dto_dsa_memset;
		}

		env_str = getenv("DTO_DSA_MEMCMP");
		if (env_str != NULL) {
			errno = 0;
			dto_dsa_memcmp = strtoul(env_str, NULL, 10);
			if (errno)
				dto_dsa_memcmp = 0;

			dto_dsa_memcmp = !!dto_dsa_memcmp;
		}

		env_str = getenv("DTO_OVERLAPPING_MEMMOVE_ACTION");
		if (env_str != NULL) {
			errno = 0;
			dto_overlapping_memmove_action = strtoul(env_str, NULL, 10);
			if (errno)
				dto_overlapping_memmove_action = OVERLAPPING_CPU;
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
			env_str = getenv("DTO_STATS_FORMAT");
			if (env_str != NULL && !strcmp(env_str, "json"))
				stats_format = STATS_FORMAT_JSON;
			else if (env_str != NULL && strcmp(env_str, "text"))
				LOG_ERROR("Unknown DTO_STATS_FORMAT '%s', using text\n", env_str);

			env_str = getenv("DTO_STATS_INTERVAL_MS");
			if (env_str != NULL) {
				char *end;

				errno = 0;
				stats_interval_ms = strtoul(env_str, &end, 10);
				/* a day: beyond that it is a typo, and the timespec math stays exact */
				if (errno || end == env_str || *end || *env_str == '-' ||
				    stats_interval_ms > 86400000UL) {
					LOG_ERROR("Invalid DTO_STATS_INTERVAL_MS '%s', no interval records\n", env_str);
					stats_interval_ms = 0;
				}
			}

			/* A forked child keeps the parent's (O_APPEND) file,
			 * unless the name has a %p (see stats_fork_child). */
			env_str = getenv("DTO_STATS_FILE");
			if (env_str != NULL && stats_fd == -1)
				stats_open_file(env_str);

			if (!stats_key_created &&
			    pthread_key_create(&stats_key, stats_thread_exit) == 0)
				stats_key_created = true;
			if (!stats_key_created) {
				LOG_ERROR("Stats collection disabled: no thread key\n");
				collect_stats = 0;
			}
		}

		if (collect_stats) {
			clock_gettime(CLOCK_BOOTTIME, &dto_start_time);
			if (stats_format == STATS_FORMAT_TEXT) {
				stats_printf("Stats collection enabled, some latencies may be higher due to collection overhead.\n");
				stats_flush();
			}
			install_sigint_handler();
		}
#endif

		/* Register fork handler for the child process */
		if (!fork_handler_registered) {
			/* If pthread_atfork fails, and process calls fork,
			 * the child may crash when using DSA offload. Dont
			 * take that risk and disable DSA offload for parent
			 * as well.
			 */
#ifdef DTO_STATS_SUPPORT
			if (!pthread_atfork(stats_fork_prepare, stats_fork_parent, child))
#else
			if (!pthread_atfork(NULL, NULL, child))
#endif
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

			env_str = getenv("DTO_CRC_MIN_BYTES");

			if (env_str != NULL) {
				errno = 0;
				crc_min_size = strtoul(env_str, NULL, 10);
				if (errno)
					crc_min_size = 0;
			}

			double cpu_size_fraction_float = 0.0;
			env_str = getenv("DTO_CPU_SIZE_FRACTION");

			if (env_str != NULL) {
				errno = 0;
				cpu_size_fraction_float = strtod(env_str, NULL);

				if (errno || cpu_size_fraction_float < 0 || cpu_size_fraction_float >= 1) {
					LOG_ERROR("Invalid DTO_CPU_SIZE_FRACTION %s, "
						"Must be >= 0 and < 1. "
						"Falling back to default 0.0\n", env_str);
					cpu_size_fraction_float = 0.0;
				}
				/* Use only 2 digits after decimal point */
				cpu_size_fraction = cpu_size_fraction_float * 100;
			}

			env_str = getenv("DTO_AUTO_ADJUST_KNOBS");

			if (env_str != NULL) {
				errno = 0;
				auto_adjust_knobs = strtoul(env_str, NULL, 10);
				if (errno)
					auto_adjust_knobs = 1;

				auto_adjust_knobs = !!auto_adjust_knobs;
			}

#ifdef DTO_NUMA_SUPPORT
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
#endif

			env_str = getenv("DTO_UMWAIT_DELAY");

			if (env_str != NULL) {
				errno = 0;
				dto_umwait_delay = strtoul(env_str, NULL, 10);
				if (errno || dto_umwait_delay == 0)
					dto_umwait_delay = UMWAIT_DELAY_DEFAULT;
			}

			if (dsa_init()) {
				LOG_ERROR("Didn't find any usable DSAs. Falling back to using CPUs.\n");
				use_std_lib_calls = 1;
			}

			/*
			 * Calculate the wait time for TPAUSE. This converts ns to TSC
			 * cycles using CPUID leaf 0x15, which may be absent or report a
			 * zero denominator/crystal frequency (common in VMs and on
			 * older parts), so verify it before dividing.
			 */
			if (!use_std_lib_calls && wait_method == WAIT_TPAUSE) {
				unsigned int num = 0, den = 0, freq = 0;
				unsigned int empty;
				unsigned long long tmp;

				if (!__get_cpuid(0x15, &den, &num, &freq, &empty) ||
				    den == 0 || num == 0 || freq == 0) {
					LOG_ERROR("CPUID leaf 0x15 unavailable (den=%u, num=%u, freq=%u); "
						"cannot compute TSC frequency for tpause. Falling back to busypoll\n",
						den, num, freq);
					wait_method = WAIT_BUSYPOLL;
					min_avg_waits = MIN_AVG_POLL_WAITS;
					max_avg_waits = MAX_AVG_POLL_WAITS;
				} else {
					freq /= 1000;
					LOG_TRACE("Core Freq = %u kHz\n", freq);
					LOG_TRACE("TSC Mult  = %u\n", num);
					LOG_TRACE("TSC Den   = %u\n", den);
					freq *= num;
					freq /= den;
					LOG_TRACE("CPU freq = %u kHz\n", freq);

					LOG_TRACE("Requested wait: %llu nsec\n", tpause_wait_time);
					tmp = tpause_wait_time;
					tmp *= freq;
					tpause_wait_time = tmp / NSEC_PER_MSEC;
					LOG_TRACE("Requested wait duration: %llu cycles\n", tpause_wait_time);
				}
			}

			// display configuration
			LOG_TRACE("log_level: %d, collect_stats: %d, use_std_lib_calls: %d, dsa_min_size: %lu, "
				"cpu_size_fraction: %.2f, wait_method: %s, auto_adjust_knobs: %d, numa_awareness: %s, dto_dsa_cc: %d\n",
				log_level, collect_stats, use_std_lib_calls, dsa_min_size,
				cpu_size_fraction_float, wait_names[wait_method], auto_adjust_knobs, numa_aware_names[is_numa_aware], dto_dsa_cc);
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
		if (wqs[i].wq_mmapped) {
			munmap(wqs[i].wq_portal, 0x1000);
			wqs[i].wq_mmapped = false;
		}
		close(wqs[i].wq_fd);
	}
#ifdef DTO_STATS_SUPPORT
	if (collect_stats) {
		stats_shutdown = true;
		stats_stop_sampler();
		print_stats();
	}
        restore_sigint_handler();
#endif
        if (log_fd != -1)
                close(log_fd);
	if (stats_fd != -1) {
		close(stats_fd);
		stats_fd = -1;
	}

#ifdef DTO_NUMA_SUPPORT
	cleanup_devices();
#endif
}

/* Next round-robin position for this thread. Each thread keeps its own, so
 * picking a WQ writes no cache line shared with other threads; a counter
 * incremented by every submit on every thread was measurable on small
 * operations. Threads start at successive positions, so concurrent threads
 * still spread across the WQs. */
static __always_inline unsigned int thread_wq_rr(void)
{
	if (unlikely(!thr_wq_rr_set)) {
		thr_wq_rr = atomic_fetch_add_explicit(&next_thread_wq, 1,
						      memory_order_relaxed);
		thr_wq_rr_set = true;
	}
	return thr_wq_rr++;
}

static __always_inline  struct dto_wq *get_wq(void* buf)
{
	struct dto_wq* wq = NULL;

#ifdef DTO_NUMA_SUPPORT
	if (is_numa_aware) {
		int status[1] = {-1};

		// get the numa node for the target DSA device
		const int numa_node = get_numa_node(buf);
		if (numa_node >= 0 && numa_node < MAX_NUMA_NODES) {
			struct dto_device* dev = devices[numa_node];
			if (dev != NULL &&
				dev->num_wqs > 0) {
				wq = dev->wqs[thread_wq_rr() % dev->num_wqs];
			}
		}
	}
#endif

	if (wq == NULL) {
		wq = &wqs[thread_wq_rr() % num_wqs];
	}

	return wq;
}

/* 8-byte MEMFILL pattern for the byte value c (like memset) */
static __always_inline uint64_t memfill_pattern(int c)
{
	return 0x0101010101010101ULL * (uint8_t)c;
}

/* libdto_explicit (DTO_NO_LIBC_INTERPOSE) leaves libc's memset, memcpy,
 * memmove and memcmp alone: programs that use it call the dto_* API below
 * directly. */
#ifndef DTO_NO_LIBC_INTERPOSE

static void dto_memset(void *s, int c, size_t n, int *result)
{
	uint64_t memset_pattern = memfill_pattern(c);
	size_t cpu_size, dsa_size;
	struct dto_wq *wq = get_wq(s);

	thr_desc.opcode = DSA_OPCODE_MEMFILL;
	thr_desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (dto_dsa_cc && (wq->dsa_gencap & GENCAP_CC_MEMORY))
		thr_desc.flags |= IDXD_OP_FLAG_CC;
	thr_desc.completion_addr = (uint64_t)&thr_comp;
	thr_desc.pattern = memset_pattern;

	/* cpu_size_fraction guaranteed to be >= 0 and < 100 */
	cpu_size = n * cpu_size_fraction / 100;
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
		size_t current_cpu_size_fraction = cpu_size_fraction;  // the cpu_size_fraction might be changed by the auto tune algorithm 
		threshold = wq->max_transfer_size * 100 / (100 - current_cpu_size_fraction);

		do {
			size_t len;

			len = n <= threshold ? n : threshold;

			cpu_size = len * current_cpu_size_fraction / 100;
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

static bool dto_memcpymove(void *dest, const void *src, size_t n, bool is_memcpy, int *result)
{
	struct dto_wq *wq;
	size_t cpu_size, dsa_size;
	bool is_overlapping;

	thr_bytes_completed = 0;

	if (!is_memcpy && is_overlapping_buffers(dest, src, n)) {
		cpu_size = 0;
		is_overlapping = true;
	} else {
		/* cpu_size_fraction guaranteed to be >= 0 and < 1 */
		cpu_size = n * cpu_size_fraction / 100;
		is_overlapping = false;
	}

	// If this is an overlapping memmove and the action is to perform on CPU, return having done nothing and
	// memmove will perform the copy and correctly attribute statistics to stdlib call group
	if (is_overlapping && dto_overlapping_memmove_action == OVERLAPPING_CPU) {
		*result = SUCCESS;
		return true;
	}

	dsa_size = n - cpu_size;
	wq = get_wq(dest);

	thr_desc.opcode = DSA_OPCODE_MEMMOVE;
	thr_desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (dto_dsa_cc && (wq->dsa_gencap & GENCAP_CC_MEMORY))
		thr_desc.flags |= IDXD_OP_FLAG_CC;
	thr_desc.completion_addr = (uint64_t)&thr_comp;

	if (dsa_size <= wq->max_transfer_size) {
		thr_desc.src_addr = (uint64_t) src + cpu_size;
		thr_desc.dst_addr = (uint64_t) dest + cpu_size;
		thr_desc.xfer_size = (uint32_t) dsa_size;
		thr_comp.status = 0;
		if (is_overlapping) {
			*result = dsa_execute(wq, &thr_desc, &thr_comp.status);
		} else {
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
		}
	} else {
		uint32_t threshold;
		size_t current_cpu_size_fraction = cpu_size_fraction;  // the cpu_size_fraction might be changed by the auto tune algorithm 
		if (is_overlapping) {
			threshold = wq->max_transfer_size;
		} else {
			threshold = wq->max_transfer_size * 100 / (100 - current_cpu_size_fraction);
		}

		do {
			size_t len;

			len = n <= threshold ? n : threshold;

			if (!is_overlapping)
				cpu_size = len * current_cpu_size_fraction / 100;

			dsa_size = len - cpu_size;

			thr_desc.src_addr = (uint64_t) src + cpu_size + thr_bytes_completed;
			thr_desc.dst_addr = (uint64_t) dest + cpu_size + thr_bytes_completed;
			thr_desc.xfer_size = (uint32_t) dsa_size;
			thr_comp.status = 0;
			if (is_overlapping){
				*result = dsa_execute(wq, &thr_desc, &thr_comp.status);
			} else {
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

	return is_overlapping;
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
		/* cmp returned mismatch. dsa_execute() already added
		 * xfer_size to thr_bytes_completed, so subtract it back
		 * to get the base of the last descriptor's range, then
		 * add thr_comp.bytes_completed (offset of the first
		 * differing byte within that range).
		 */
		uint64_t off = thr_bytes_completed - thr_desc.xfer_size +
				thr_comp.bytes_completed;
		uint8_t *t1 = (uint8_t *)s1 + off;
		uint8_t *t2 = (uint8_t *)s2 + off;

		cmp_result = *t1 - *t2;
		/* Inform the caller that the job is done even though
		 * we didn't process all the bytes
		 */
		thr_bytes_completed = orig_n;
	}
	return cmp_result;
}

/* The dto_internal_mem* APIs are used only when mem* APIs are
 * called before DTO is properly initialized. So these
 * implementations dont have to be performant.
 *
 * The pointers below MUST stay volatile. At -O2 and above, GCC's
 * loop-idiom recognition (-ftree-loop-distribute-patterns) rewrites
 * these byte loops into calls to memset()/memcpy()/memcmp(). Because
 * DTO exports its own memset()/memcpy()/memmove()/memcmp(), those
 * calls go through the PLT and bind back to DTO's own interposed
 * versions. The only way to reach dto_internal_mem*() is with
 * dto_initialized == 0, so the interposed function would call the
 * internal helper again, recursing until the stack guard page is hit
 * (SIGSEGV during early startup or in a forked child, where the
 * atfork handler resets dto_initialized). volatile accesses cannot be
 * folded into a library call, which blocks the transform.
 */
static void *dto_internal_memset(void *s1, int c, size_t n)
{
	volatile char *dest = s1;
	size_t i;

	for (i = 0; i < n; i++)
		dest[i] = (char)c;

	return s1;
}

static void *dto_internal_memcpymove(void *dest, const void *src, size_t n)
{
	volatile char *d = dest;
	const volatile char *s = (const volatile char *)src;
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
	const volatile unsigned char *src1 = (const volatile unsigned char *)s1;
	const volatile unsigned char *src2 = (const volatile unsigned char *)s2;
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c1 = src1[i];
		unsigned char c2 = src2[i];

		if (c1 != c2)
			return c1 - c2;
	}
	return 0;
}

void *memset(void *s1, int c, size_t n)
{
	int result = 0;
	void *ret = s1;
	int use_orig_func = USE_ORIG_FUNC(n, dto_dsa_memset);
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
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMSET, n, false, thr_bytes_completed, result);
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
	int use_orig_func = USE_ORIG_FUNC(n, dto_dsa_memcpy);
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
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMCOPY, n, false, thr_bytes_completed, result);
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
	int use_orig_func = USE_ORIG_FUNC(n, dto_dsa_memmove);
	bool is_overlapping;
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
		is_overlapping = dto_memcpymove(dest, src, n, 0, &result);

#ifdef DTO_STATS_SUPPORT
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMMOVE, n, is_overlapping, thr_bytes_completed, result);
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
	int use_orig_func = USE_ORIG_FUNC(n, dto_dsa_memcmp);
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
		DTO_COLLECT_STATS_DSA_END(collect_stats, st, et, MEMCMP, n, false, thr_bytes_completed, result);
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

#endif /* DTO_NO_LIBC_INTERPOSE */

/*******************************************************************************
 * Explicit asynchronous submit/poll API (see dto.h).
 ******************************************************************************/

/* CRC presentations. With its default CRC flags the device inverts both the
 * seed and the result: the iSCSI presentation, in which seed 0 gives the
 * standard CRC32C and a previous result chains. The crc32c calls return it
 * unchanged. The raw CRC32C of dto_async_crc_val/dto_batch_crc -- the SSE4.2
 * crc32 loop from 0, no final inversion -- is the same computation seeded
 * with ~0 and inverted back. */
#define DSA_CRC_SEED_RAW	0xFFFFFFFFu
#define DSA_CRC_VAL_TO_RAW(v)	((uint32_t)~(uint32_t)(v))

/* ENQCMD to a shared WQ can transiently fail when the queue is full;
 * retry briefly before giving up so momentary bursts don't push work
 * back onto the CPU. A batch carries more work, so it tries longer. */
#define ASYNC_SUBMIT_RETRIES 16
#define BATCH_SUBMIT_RETRIES 64

/* Failed async completions are logged at most this many times per process:
 * poll may legitimately be called repeatedly on a failed op. */
#define ASYNC_FAIL_LOG_LIMIT 3

/* dto_async_wait() must not wait for a status byte no device will ever
 * write: an op whose submit fell back, or one never submitted at all (ops
 * are usually stack variables, so their storage starts out as garbage).
 * Every submit clears the mark first and sets it once the descriptor is
 * enqueued; garbage matching it by chance is a 2^-32 event. */
#define DTO_OP_SUBMITTED_MAGIC 0x64746F53u	/* "dtoS" */

struct dto_async_op_impl {
	struct dsa_hw_desc desc;	/* 64 bytes, 64-aligned via dto_async_op */
	struct dsa_completion_record comp __attribute__((aligned(32)));
	uint32_t submitted;
};
_Static_assert(sizeof(struct dto_async_op_impl) <= sizeof(dto_async_op),
	       "dto_async_op opaque storage too small");
_Static_assert(sizeof(struct dsa_hw_desc) == 64, "unexpected descriptor size");

typedef uint64_t __attribute__((may_alias, aligned(1))) unaligned_u64;

/* CRC32C on the CPU in the device's presentation for the given seed, so a
 * value the library computes matches what the device returns for the same
 * descriptor. */
__attribute__((target("sse4.2")))
static uint32_t cpu_crc32c(uint32_t seed, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	uint64_t crc = (uint32_t)~seed;

	for (; n >= 8; n -= 8, p += 8)
		crc = _mm_crc32_u64(crc, *(const unaligned_u64 *)p);
	for (; n; n--)
		crc = _mm_crc32_u8((uint32_t)crc, *p++);
	return ~(uint32_t)crc;
}

static __always_inline uint32_t async_desc_flags(const struct dto_wq *wq,
						 unsigned int flags,
						 bool has_dest)
{
	uint32_t f = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;

	/* Passed as given: a work queue configured without block-on-fault
	 * fails the descriptor with DSA_COMP_INVALID_FLAGS, and without the
	 * flag a page fault stops the operation partway (see dto.h). */
	if (flags & DTO_SUBMIT_BOF)
		f |= IDXD_OP_FLAG_BOF;
	/* CC is only legal for operations with a destination; CRC Generation
	 * would be failed by the device with DSA_COMP_INVALID_FLAGS. */
	if ((flags & DTO_SUBMIT_CC) && has_dest &&
	    (wq->dsa_gencap & GENCAP_CC_MEMORY))
		f |= IDXD_OP_FLAG_CC;
	return f;
}

static int async_submit(struct dto_wq *wq, struct dsa_hw_desc *desc,
			int retries)
{
	for (int attempt = 0; attempt < retries; attempt++) {
		int rc = dsa_submit(wq, desc);

		if (rc == SUCCESS)
			return DTO_ASYNC_SUBMITTED;
		if (rc != RETRY)
			break;
		_mm_pause();
	}
	return DTO_ASYNC_FALLBACK;
}

static void async_log_failure(uint8_t status, uint32_t opcode, uint32_t size)
{
	static int fail_logged;

	if (fail_logged < ASYNC_FAIL_LOG_LIMIT &&
	    __atomic_fetch_add(&fail_logged, 1, __ATOMIC_RELAXED) <
	    ASYNC_FAIL_LOG_LIMIT)
		LOG_ERROR("async op failed with status %x (opcode %x, size %x)\n",
			  status, opcode, size);
}

/* Completion record status -> DTO_ASYNC_{PENDING,DONE,FAILED} */
static int async_comp_result(const struct dsa_completion_record *comp,
			     uint32_t opcode, uint32_t size)
{
	uint8_t status = __atomic_load_n((const uint8_t *)&comp->status,
					 __ATOMIC_ACQUIRE);

	if (status == 0)
		return DTO_ASYNC_PENDING;
	if (likely((status & DSA_COMP_STATUS_MASK) == DSA_COMP_SUCCESS))
		return DTO_ASYNC_DONE;
	async_log_failure(status, opcode, size);
	return DTO_ASYNC_FAILED;
}

/* Common part of every single-op submit. NULL means fall back, with nothing
 * submitted; otherwise the descriptor is set up but for its operands. hint
 * picks the work queue (NUMA-aware selection uses its node). */
static struct dto_wq *async_prepare(struct dto_async_op_impl *impl,
				    uint32_t opcode, const void *hint,
				    size_t n, unsigned int flags, bool has_dest)
{
	struct dto_wq *wq;

	impl->submitted = 0;
	if (!dto_initialized || use_std_lib_calls)
		return NULL;
	/* The completion record must be 32-byte aligned. dto_async_op is
	 * declared 64-byte aligned, but a malloc'ed op need not be. */
	if (n == 0 || ((uintptr_t)impl & 31) != 0)
		return NULL;
	wq = get_wq((void *)hint);
	if (unlikely(wq == NULL) || n > wq->max_transfer_size)
		return NULL;

	orig_memset(&impl->desc, 0, sizeof(impl->desc));
	impl->desc.opcode = opcode;
	impl->desc.flags = async_desc_flags(wq, flags, has_dest);
	impl->desc.completion_addr = (uint64_t)&impl->comp;
	impl->desc.xfer_size = (uint32_t)n;
	impl->comp.status = 0;
	return wq;
}

static int async_enqueue(struct dto_async_op_impl *impl, struct dto_wq *wq)
{
	if (async_submit(wq, &impl->desc, ASYNC_SUBMIT_RETRIES) !=
	    DTO_ASYNC_SUBMITTED)
		return DTO_ASYNC_FALLBACK;
	impl->submitted = DTO_OP_SUBMITTED_MAGIC;
	return DTO_ASYNC_SUBMITTED;
}

__attribute__((visibility("default")))
int dto_submit_memcpy(dto_async_op *op, void *dest, const void *src,
		      size_t n, unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, DSA_OPCODE_MEMMOVE, dest, n,
					  flags, true);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	impl->desc.src_addr = (uint64_t)src;
	impl->desc.dst_addr = (uint64_t)dest;
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_submit_memfill(dto_async_op *op, void *dest, uint64_t pattern,
		       size_t n, unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, DSA_OPCODE_MEMFILL, dest, n,
					  flags, true);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	impl->desc.pattern = pattern;
	impl->desc.dst_addr = (uint64_t)dest;
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_submit_memset(dto_async_op *op, void *dest, int c, size_t n,
		      unsigned int flags)
{
	return dto_submit_memfill(op, dest, memfill_pattern(c), n, flags);
}

static int submit_crc_common(dto_async_op *op, uint32_t opcode, void *dest,
			     const void *src, size_t n, uint32_t seed,
			     unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, opcode, dest ? dest : src, n,
					  flags, dest != NULL);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	impl->desc.src_addr = (uint64_t)src;
	impl->desc.dst_addr = (uint64_t)dest;
	impl->desc.crc_seed = seed;
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_submit_memcpy_crc(dto_async_op *op, void *dest, const void *src,
			  size_t n, unsigned int flags)
{
	return submit_crc_common(op, DSA_OPCODE_COPY_CRC, dest, src, n,
				 DSA_CRC_SEED_RAW, flags);
}

__attribute__((visibility("default")))
int dto_submit_crc(dto_async_op *op, const void *src, size_t n,
		   unsigned int flags)
{
	return submit_crc_common(op, DSA_OPCODE_CRCGEN, NULL, src, n,
				 DSA_CRC_SEED_RAW, flags);
}

__attribute__((visibility("default")))
int dto_submit_memcpy_crc32c(dto_async_op *op, void *dest, const void *src,
			     size_t n, uint32_t seed, unsigned int flags)
{
	return submit_crc_common(op, DSA_OPCODE_COPY_CRC, dest, src, n, seed,
				 flags);
}

__attribute__((visibility("default")))
int dto_submit_crc32c(dto_async_op *op, const void *src, size_t n,
		      uint32_t seed, unsigned int flags)
{
	return submit_crc_common(op, DSA_OPCODE_CRCGEN, NULL, src, n, seed,
				 flags);
}

__attribute__((visibility("default")))
int dto_submit_compare(dto_async_op *op, const void *src1, const void *src2,
		       size_t n, unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, DSA_OPCODE_COMPARE, src1, n,
					  flags, false);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	impl->desc.src_addr = (uint64_t)src1;
	impl->desc.src2_addr = (uint64_t)src2;
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_submit_compval(dto_async_op *op, const void *src, uint64_t pattern,
		       size_t n, unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, DSA_OPCODE_COMPVAL, src, n,
					  flags, false);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	impl->desc.src_addr = (uint64_t)src;
	/* Compare Pattern takes its pattern from the second operand slot;
	 * the first one is the source (it is Memory Fill's pattern slot). */
	impl->desc.comp_pattern = pattern;
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_submit_transl_fetch(dto_async_op *op, const void *addr, size_t n,
			    unsigned int flags)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;
	struct dto_wq *wq = async_prepare(impl, DSA_OPCODE_TRANSL_FETCH, addr,
					  n, flags, false);

	if (wq == NULL)
		return DTO_ASYNC_FALLBACK;
	/* region_size shares the xfer_size slot async_prepare filled in */
	impl->desc.transl_fetch_addr = (uint64_t)addr;
	impl->desc.region_stride = 0;	/* one contiguous region */
	return async_enqueue(impl, wq);
}

__attribute__((visibility("default")))
int dto_async_poll(dto_async_op *op)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;

	return async_comp_result(&impl->comp, impl->desc.opcode,
				 impl->desc.xfer_size);
}

__attribute__((visibility("default")))
void dto_async_wait(dto_async_op *op)
{
	struct dto_async_op_impl *impl = (struct dto_async_op_impl *)op;

	if (impl->submitted != DTO_OP_SUBMITTED_MAGIC)
		return;
	dsa_wait_no_adjust(&impl->comp.status);
	/* order the status byte ahead of the caller's reads of the record */
	atomic_thread_fence(memory_order_acquire);
}

__attribute__((visibility("default")))
int dto_wait_blocks(void)
{
	return 0;
}

__attribute__((visibility("default")))
uint32_t dto_async_crc_val(const dto_async_op *op)
{
	const struct dto_async_op_impl *impl =
		(const struct dto_async_op_impl *)op;
	return DSA_CRC_VAL_TO_RAW(impl->comp.crc_val);
}

__attribute__((visibility("default")))
uint32_t dto_async_crc32c_val(const dto_async_op *op)
{
	const struct dto_async_op_impl *impl =
		(const struct dto_async_op_impl *)op;
	return (uint32_t)impl->comp.crc_val;
}

__attribute__((visibility("default")))
int dto_async_result(const dto_async_op *op)
{
	const struct dto_async_op_impl *impl =
		(const struct dto_async_op_impl *)op;
	return impl->comp.result;
}

__attribute__((visibility("default")))
uint32_t dto_async_bytes_completed(const dto_async_op *op)
{
	const struct dto_async_op_impl *impl =
		(const struct dto_async_op_impl *)op;
	return impl->comp.bytes_completed;
}

__attribute__((visibility("default")))
int dto_async_status(const dto_async_op *op)
{
	const struct dto_async_op_impl *impl =
		(const struct dto_async_op_impl *)op;
	return __atomic_load_n((const uint8_t *)&impl->comp.status,
			       __ATOMIC_ACQUIRE) & DSA_COMP_STATUS_MASK;
}

__attribute__((visibility("default")))
uint32_t dto_crc32c_with_seed(uint32_t seed, const void *src, size_t n)
{
	size_t min_size = crc_min_size ? crc_min_size : dsa_min_size;
	dto_async_op op;

	/* No DTO_SUBMIT_BOF, so this works on any work queue: a page fault
	 * stops the device and the CRC is computed on the CPU instead. */
	if (n >= min_size &&
	    dto_submit_crc32c(&op, src, n, seed, 0) == DTO_ASYNC_SUBMITTED) {
		dto_async_wait(&op);
		if (dto_async_poll(&op) == DTO_ASYNC_DONE)
			return dto_async_crc32c_val(&op);
	}
	return cpu_crc32c(seed, src, n);
}

/* ---- Batches (see dto.h) ---- */

/* Zero storage the library owns. Once DTO is up that is libc's memset, so it
 * costs no detour through the interposer and a tiny DTO_MIN_BYTES never sends
 * it to the device; before that, memset() itself, which copes with an
 * uninitialized DTO (and is libc's in libdto_explicit). */
static void lib_zero(void *p, size_t n)
{
	if (orig_memset)
		orig_memset(p, 0, n);
	else
		memset(p, 0, n);
}

enum batch_state {
	BATCH_BUILDING = 0,	/* accepting operations */
	BATCH_SUBMITTED,	/* in flight */
	BATCH_REPAIRING,	/* one poller is redoing failed operations */
	BATCH_DONE,
};

struct dto_batch {
	struct dsa_hw_desc desc __attribute__((aligned(64)));	/* the Batch descriptor */
	struct dsa_completion_record comp __attribute__((aligned(32)));
	struct dsa_hw_desc *descs;		/* [cap], 64-byte aligned */
	struct dsa_completion_record *comps;	/* [cap], 32-byte aligned */
	const void *hint;			/* first operand: picks the WQ */
	int cap;
	int count;
	_Atomic int state;
};

/* The record that completes the batch: the Batch descriptor's, or for a
 * single operation, sent as a plain descriptor, that operation's own. */
static __always_inline volatile uint8_t *batch_status(dto_batch *b)
{
	return b->count == 1 ? &b->comps[0].status : &b->comp.status;
}

/* Wait out a batch the device may still be writing to. */
static void batch_quiesce(dto_batch *b)
{
	if (atomic_load_explicit(&b->state, memory_order_acquire) ==
	    BATCH_SUBMITTED)
		dsa_wait_no_adjust(batch_status(b));
	while (atomic_load_explicit(&b->state, memory_order_acquire) ==
	       BATCH_REPAIRING)
		_mm_pause();
}

__attribute__((visibility("default")))
dto_batch *dto_batch_create(int capacity)
{
	void *b = NULL, *descs = NULL, *comps = NULL;

	if (capacity < 1)
		return NULL;
	if (capacity > DTO_BATCH_MAX)
		capacity = DTO_BATCH_MAX;
	if (posix_memalign(&b, 64, sizeof(struct dto_batch)) ||
	    posix_memalign(&descs, 64, sizeof(struct dsa_hw_desc) * capacity) ||
	    posix_memalign(&comps, 32,
			   sizeof(struct dsa_completion_record) * capacity)) {
		free(b);
		free(descs);
		return NULL;
	}
	/* operations zero their own descriptor and record when added */
	lib_zero(b, sizeof(struct dto_batch));
	((struct dto_batch *)b)->descs = descs;
	((struct dto_batch *)b)->comps = comps;
	((struct dto_batch *)b)->cap = capacity;
	return b;
}

__attribute__((visibility("default")))
void dto_batch_destroy(dto_batch *b)
{
	if (b == NULL)
		return;
	batch_quiesce(b);
	free(b->descs);
	free(b->comps);
	free(b);
}

__attribute__((visibility("default")))
void dto_batch_reset(dto_batch *b)
{
	batch_quiesce(b);
	b->count = 0;
	b->hint = NULL;
	atomic_store_explicit(&b->state, BATCH_BUILDING, memory_order_relaxed);
}

__attribute__((visibility("default")))
int dto_batch_count(const dto_batch *b)
{
	return b->count;
}

/* Descriptor for the next operation, or NULL (with *index unset) when the
 * batch is full or submitted or n is out of range. BOF is set as requested;
 * CC is recorded as requested and settled at submit, once the work queue
 * (and so the device's cache-control support) is known. */
static struct dsa_hw_desc *batch_add(dto_batch *b, uint32_t opcode,
				     const void *hint, size_t n,
				     unsigned int flags, bool has_dest,
				     int *index)
{
	struct dsa_hw_desc *d;
	int i;

	if (atomic_load_explicit(&b->state, memory_order_relaxed) !=
	    BATCH_BUILDING || b->count >= b->cap || n == 0 || n > UINT32_MAX)
		return NULL;
	i = b->count++;
	d = &b->descs[i];
	lib_zero(d, sizeof(*d));
	d->opcode = opcode;
	d->flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if (flags & DTO_SUBMIT_BOF)
		d->flags |= IDXD_OP_FLAG_BOF;
	if ((flags & DTO_SUBMIT_CC) && has_dest)
		d->flags |= IDXD_OP_FLAG_CC;
	d->completion_addr = (uint64_t)&b->comps[i];
	d->xfer_size = (uint32_t)n;
	lib_zero(&b->comps[i], sizeof(b->comps[i]));
	if (b->hint == NULL)
		b->hint = hint;
	*index = i;
	return d;
}

__attribute__((visibility("default")))
int dto_batch_add_memmove(dto_batch *b, void *dst, const void *src, size_t n,
			  unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_MEMMOVE, dst, n, flags,
					  true, &i);

	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src;
	d->dst_addr = (uint64_t)dst;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_memfill(dto_batch *b, void *dst, uint64_t pattern, size_t n,
			  unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_MEMFILL, dst, n, flags,
					  true, &i);

	if (d == NULL)
		return -1;
	d->pattern = pattern;
	d->dst_addr = (uint64_t)dst;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_dualcast(dto_batch *b, void *dst1, void *dst2,
			   const void *src, size_t n, unsigned int flags)
{
	struct dsa_hw_desc *d;
	int i;

	/* the device requires both destinations to agree in bits 11:0 */
	if ((((uintptr_t)dst1 ^ (uintptr_t)dst2) & 0xfff) != 0)
		return -1;
	d = batch_add(b, DSA_OPCODE_DUALCAST, dst1, n, flags, true, &i);
	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src;
	d->dst_addr = (uint64_t)dst1;
	d->dest2 = (uint64_t)dst2;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_crc(dto_batch *b, const void *src, size_t n,
		      unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_CRCGEN, src, n, flags,
					  false, &i);

	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src;
	d->crc_seed = DSA_CRC_SEED_RAW;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_memcpy_crc(dto_batch *b, void *dst, const void *src,
			     size_t n, unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_COPY_CRC, dst, n, flags,
					  true, &i);

	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src;
	d->dst_addr = (uint64_t)dst;
	d->crc_seed = DSA_CRC_SEED_RAW;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_compare(dto_batch *b, const void *src1, const void *src2,
			  size_t n, unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_COMPARE, src1, n, flags,
					  false, &i);

	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src1;
	d->src2_addr = (uint64_t)src2;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_add_compval(dto_batch *b, const void *src, uint64_t pattern,
			  size_t n, unsigned int flags)
{
	int i;
	struct dsa_hw_desc *d = batch_add(b, DSA_OPCODE_COMPVAL, src, n, flags,
					  false, &i);

	if (d == NULL)
		return -1;
	d->src_addr = (uint64_t)src;
	d->comp_pattern = pattern;
	return i;
}

__attribute__((visibility("default")))
int dto_batch_submit(dto_batch *b)
{
	struct dsa_hw_desc *desc;
	struct dto_wq *wq;

	if (atomic_load_explicit(&b->state, memory_order_relaxed) !=
	    BATCH_BUILDING)
		return DTO_ASYNC_FALLBACK;
	if (b->count == 0) {
		atomic_store_explicit(&b->state, BATCH_DONE,
				      memory_order_release);
		return DTO_ASYNC_SUBMITTED;
	}
	if (!dto_initialized || use_std_lib_calls)
		return DTO_ASYNC_FALLBACK;
	wq = get_wq((void *)b->hint);
	if (unlikely(wq == NULL) ||
	    (b->count > 1 && (uint32_t)b->count > wq->max_batch_size))
		return DTO_ASYNC_FALLBACK;
	for (int i = 0; i < b->count; i++) {
		if (b->descs[i].xfer_size > wq->max_transfer_size)
			return DTO_ASYNC_FALLBACK;
	}

	for (int i = 0; i < b->count; i++) {
		struct dsa_hw_desc *d = &b->descs[i];

		if (!(wq->dsa_gencap & GENCAP_CC_MEMORY))
			d->flags &= ~IDXD_OP_FLAG_CC;
		b->comps[i].status = 0;
	}
	if (b->count == 1) {
		desc = &b->descs[0];
	} else {
		orig_memset(&b->desc, 0, sizeof(b->desc));
		b->desc.opcode = DSA_OPCODE_BATCH;
		b->desc.flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
		b->desc.desc_list_addr = (uint64_t)b->descs;
		b->desc.desc_count = (uint32_t)b->count;
		b->desc.completion_addr = (uint64_t)&b->comp;
		b->comp.status = 0;
		desc = &b->desc;
	}

	if (async_submit(wq, desc, BATCH_SUBMIT_RETRIES) != DTO_ASYNC_SUBMITTED)
		return DTO_ASYNC_FALLBACK;
	atomic_store_explicit(&b->state, BATCH_SUBMITTED, memory_order_release);
	return DTO_ASYNC_SUBMITTED;
}

/* Fill n bytes with the 8-byte pattern, as Memory Fill does. */
static void cpu_fill(void *dst, uint64_t pattern, size_t n)
{
	uint8_t *p = dst;
	size_t i = 0;

	if (pattern == memfill_pattern((uint8_t)pattern)) {
		orig_memset(dst, (uint8_t)pattern, n);
		return;
	}
	for (; i + 8 <= n; i += 8)
		*(unaligned_u64 *)(p + i) = pattern;
	for (; i < n; i++)
		p[i] = (uint8_t)(pattern >> (8 * (i & 7)));
}

/* Fill in a compare's result the way the device does: result 1 and the
 * offset of the first difference, or result 0 with every byte compared.
 * With src2 NULL, src1 is compared against the 8-byte pattern instead. */
static void cpu_compare(struct dsa_completion_record *comp, const void *src1,
			const void *src2, uint64_t pattern, size_t n)
{
	const uint8_t *a = src1, *b = src2;
	size_t i = 0;

	/* a word at a time up to the first differing word, then bytewise */
	for (; i + 8 <= n; i += 8) {
		uint64_t want = b ? *(const unaligned_u64 *)(b + i) : pattern;

		if (*(const unaligned_u64 *)(a + i) != want)
			break;
	}
	for (; i < n; i++) {
		uint8_t want = b ? b[i] : (uint8_t)(pattern >> (8 * (i & 7)));

		if (a[i] != want)
			break;
	}
	comp->result = i < n;
	comp->bytes_completed = (uint32_t)i;
}

/* A move the device stopped partway (a page fault without BOF) cannot be
 * redone from the start when the buffers overlap: the device has already
 * overwritten part of the source. bytes_completed counts from the end the
 * copy started at, which bit 0 of result gives (1: descending). */
static void cpu_redo_memmove(struct dsa_completion_record *comp, void *dst,
			     const void *src, size_t n)
{
	size_t done = comp->bytes_completed;

	if ((comp->status & DSA_COMP_STATUS_MASK) != DSA_COMP_PAGE_FAULT_NOBOF ||
	    done >= n)
		done = 0;
	if (comp->result & 1)
		orig_memmove(dst, src, n - done);
	else
		orig_memmove((uint8_t *)dst + done, (const uint8_t *)src + done,
			     n - done);
}

/* Redo every operation the device did not complete on the CPU. Each status
 * is left as the device wrote it, so dto_batch_status() still says who did
 * the work; result, bytes_completed and crc_val are filled in as the device
 * would have. */
static void batch_repair(dto_batch *b)
{
	for (int i = 0; i < b->count; i++) {
		const struct dsa_hw_desc *d = &b->descs[i];
		struct dsa_completion_record *c = &b->comps[i];
		void *dst = (void *)(uintptr_t)d->dst_addr;
		const void *src = (const void *)(uintptr_t)d->src_addr;
		size_t n = d->xfer_size;

		if ((c->status & DSA_COMP_STATUS_MASK) == DSA_COMP_SUCCESS)
			continue;
		switch (d->opcode) {
		case DSA_OPCODE_MEMMOVE:
			cpu_redo_memmove(c, dst, src, n);
			break;
		case DSA_OPCODE_MEMFILL:
			cpu_fill(dst, d->pattern, n);
			break;
		case DSA_OPCODE_DUALCAST:
			orig_memcpy(dst, src, n);
			orig_memcpy((void *)(uintptr_t)d->dest2, src, n);
			break;
		case DSA_OPCODE_COPY_CRC:
			orig_memcpy(dst, src, n);
			c->crc_val = cpu_crc32c(d->crc_seed, src, n);
			break;
		case DSA_OPCODE_CRCGEN:
			c->crc_val = cpu_crc32c(d->crc_seed, src, n);
			break;
		case DSA_OPCODE_COMPARE:
			cpu_compare(c, src, (const void *)(uintptr_t)d->src2_addr,
				    0, n);
			break;
		case DSA_OPCODE_COMPVAL:
			cpu_compare(c, src, NULL, d->comp_pattern, n);
			break;
		}
	}
}

__attribute__((visibility("default")))
int dto_batch_poll(dto_batch *b)
{
	int state = atomic_load_explicit(&b->state, memory_order_acquire);
	uint8_t status;

	if (state == BATCH_DONE)
		return DTO_ASYNC_DONE;
	if (state == BATCH_BUILDING)
		return DTO_ASYNC_FAILED;
	if (state == BATCH_REPAIRING)
		return DTO_ASYNC_PENDING;

	status = __atomic_load_n(batch_status(b), __ATOMIC_ACQUIRE);
	if (status == 0)
		return DTO_ASYNC_PENDING;
	if ((status & DSA_COMP_STATUS_MASK) != DSA_COMP_SUCCESS) {
		/* exactly one poller repairs; the others see PENDING meanwhile */
		if (!atomic_compare_exchange_strong(&b->state, &state,
						    BATCH_REPAIRING))
			return state == BATCH_DONE ? DTO_ASYNC_DONE :
						     DTO_ASYNC_PENDING;
		async_log_failure(status, b->count == 1 ? b->descs[0].opcode :
				  DSA_OPCODE_BATCH, b->count);
		batch_repair(b);
	}
	atomic_store_explicit(&b->state, BATCH_DONE, memory_order_release);
	return DTO_ASYNC_DONE;
}

__attribute__((visibility("default")))
void dto_batch_wait(dto_batch *b)
{
	if (atomic_load_explicit(&b->state, memory_order_acquire) !=
	    BATCH_SUBMITTED)
		return;
	dsa_wait_no_adjust(batch_status(b));
	atomic_thread_fence(memory_order_acquire);
}

__attribute__((visibility("default")))
int dto_batch_status(const dto_batch *b, int i)
{
	if (i < 0 || i >= b->count)
		return 0;
	return __atomic_load_n((const uint8_t *)&b->comps[i].status,
			       __ATOMIC_ACQUIRE) & DSA_COMP_STATUS_MASK;
}

__attribute__((visibility("default")))
int dto_batch_result(const dto_batch *b, int i)
{
	if (i < 0 || i >= b->count)
		return 0;
	return b->comps[i].result;
}

__attribute__((visibility("default")))
uint32_t dto_batch_bytes_completed(const dto_batch *b, int i)
{
	if (i < 0 || i >= b->count)
		return 0;
	return b->comps[i].bytes_completed;
}

__attribute__((visibility("default")))
uint32_t dto_batch_crc(const dto_batch *b, int i)
{
	if (i < 0 || i >= b->count)
		return 0;
	return DSA_CRC_VAL_TO_RAW(b->comps[i].crc_val);
}

__attribute__((visibility("default")))
int dto_submit_batch_copy(dto_batch *b, void **dst, void **src,
			  size_t *sizes, int count, unsigned int flags)
{
	if (count < 1 || count > b->cap)
		return DTO_ASYNC_FALLBACK;
	dto_batch_reset(b);
	for (int i = 0; i < count; i++) {
		if (dto_batch_add_memmove(b, dst[i], src[i], sizes[i],
					  flags) < 0)
			return DTO_ASYNC_FALLBACK;
	}
	return dto_batch_submit(b);
}
