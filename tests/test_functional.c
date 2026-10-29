/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 * test_functional.c - Functional correctness tests for DTO.
 *
 * Tests memset, memcpy, memmove, memcmp across a range of buffer sizes
 * spanning below and above the DSA offload threshold (64KB default).
 *
 * When linked against libdto, these functions are intercepted by DTO.
 * In CI (no DSA hardware), DTO falls back to the CPU path transparently.
 * All tests should pass regardless of whether DSA hardware is present.
 *
 * Verification is done byte-by-byte to avoid relying on DTO's own memcmp
 * for checking results.
 *
 * Labels: functional (safe to run in GitHub Actions without DSA)
 ******************************************************************************/

#include "dto_test_utils.h"
#include "../dto.h"
#include <nmmintrin.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>

/* Buffer sizes spanning below and above the DSA threshold (65536) */
static const size_t test_sizes[] = {
	0, 1, 7, 15, 16, 31, 32, 63, 64,
	127, 128, 255, 256, 511, 512, 1023, 1024,
	4096, 8192, 16384, 32768,
	65536,		/* DTO_DEFAULT_MIN_SIZE - DSA threshold */
	131072,		/* 128K - above threshold, triggers DSA */
	262144,		/* 256K */
};
#define NUM_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))
#define MAX_SIZE 262144

/*
 * Helper functions for verification.
 *
 * These use byte-by-byte loops so the compiler cannot optimize them into
 * memset/memcpy/memmove/memcmp calls. DTO interposes all four, so a folded
 * loop would check DTO against itself instead of verifying it independently.
 *
 * The volatile pointers are what enforce that. A volatile access is an
 * observable side effect the compiler may not elide, reorder or rewrite, so
 * neither GCC's -ftree-loop-distribute-patterns nor LLVM's loop-idiom
 * recognition can fold these loops into a library call. It must not be done
 * with __attribute__((optimize("no-tree-loop-distribute-patterns"))): that is
 * GCC-only and clang ignores it silently, which let clang turn clear_buf()
 * into a call to DTO's own memset().
 */
static int verify_set(const uint8_t *buf, uint8_t val, size_t n)
{
	const volatile uint8_t *p = buf;

	for (size_t i = 0; i < n; i++) {
		uint8_t got = p[i];

		if (got != val) {
			fprintf(stderr, "    byte[%zu] = 0x%02x, expected 0x%02x\n",
				i, got, val);
			return 0;
		}
	}
	return 1;
}

static int verify_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	const volatile uint8_t *pa = a;
	const volatile uint8_t *pb = b;

	for (size_t i = 0; i < n; i++) {
		uint8_t x = pa[i];
		uint8_t y = pb[i];

		if (x != y) {
			fprintf(stderr, "    byte[%zu]: got 0x%02x, expected 0x%02x\n",
				i, x, y);
			return 0;
		}
	}
	return 1;
}

static void fill_pattern(uint8_t *buf, size_t n)
{
	volatile uint8_t *p = buf;

	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i & 0xFF);
}

static void clear_buf(uint8_t *buf, size_t n)
{
	volatile uint8_t *p = buf;

	for (size_t i = 0; i < n; i++)
		p[i] = 0;
}

/* Forward byte copy used to build expected results. Same volatile
 * requirement as above: this must never become a memcpy/memmove call.
 */
static void copy_forward(uint8_t *dst, const uint8_t *src, size_t n)
{
	volatile uint8_t *d = dst;
	const volatile uint8_t *s = src;

	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
}

/* ---- memset tests ---- */

static int test_memset_correctness(void)
{
	uint8_t *buf = alloc_aligned(MAX_SIZE);

	ASSERT_TRUE(buf != NULL);

	for (size_t s = 0; s < NUM_SIZES; s++) {
		size_t n = test_sizes[s];

		/* Test with 0xAA */
		clear_buf(buf, n);
		memset(buf, 0xAA, n);
		if (!verify_set(buf, 0xAA, n)) {
			fprintf(stderr, "    memset(0xAA) failed at size %zu\n", n);
			free(buf);
			return 1;
		}

		/* Test with 0x00 */
		fill_pattern(buf, n);
		memset(buf, 0x00, n);
		if (!verify_set(buf, 0x00, n)) {
			fprintf(stderr, "    memset(0x00) failed at size %zu\n", n);
			free(buf);
			return 1;
		}

		/* Test with 0xFF */
		clear_buf(buf, n);
		memset(buf, 0xFF, n);
		if (!verify_set(buf, 0xFF, n)) {
			fprintf(stderr, "    memset(0xFF) failed at size %zu\n", n);
			free(buf);
			return 1;
		}
	}

	free(buf);
	return 0;
}

/* ---- memcpy tests ---- */

static int test_memcpy_correctness(void)
{
	uint8_t *src = alloc_aligned(MAX_SIZE);
	uint8_t *dst = alloc_aligned(MAX_SIZE);

	ASSERT_TRUE(src != NULL && dst != NULL);

	fill_pattern(src, MAX_SIZE);

	for (size_t s = 0; s < NUM_SIZES; s++) {
		size_t n = test_sizes[s];

		clear_buf(dst, n);
		memcpy(dst, src, n);
		if (!verify_equal(dst, src, n)) {
			fprintf(stderr, "    memcpy failed at size %zu\n", n);
			free(src);
			free(dst);
			return 1;
		}
	}

	free(src);
	free(dst);
	return 0;
}

/* ---- memmove tests ---- */

static int test_memmove_nonoverlap(void)
{
	uint8_t *src = alloc_aligned(MAX_SIZE);
	uint8_t *dst = alloc_aligned(MAX_SIZE);

	ASSERT_TRUE(src != NULL && dst != NULL);

	fill_pattern(src, MAX_SIZE);

	for (size_t s = 0; s < NUM_SIZES; s++) {
		size_t n = test_sizes[s];

		clear_buf(dst, n);
		memmove(dst, src, n);
		if (!verify_equal(dst, src, n)) {
			fprintf(stderr, "    memmove (non-overlap) failed at size %zu\n", n);
			free(src);
			free(dst);
			return 1;
		}
	}

	free(src);
	free(dst);
	return 0;
}

static int test_memmove_overlap_forward(void)
{
	/* Overlapping forward: dst > src, dst within [src, src+n) */
	size_t buf_size = MAX_SIZE + 4096;
	uint8_t *buf = alloc_aligned(buf_size);
	uint8_t *ref = alloc_aligned(buf_size);

	ASSERT_TRUE(buf != NULL && ref != NULL);

	size_t overlap_sizes[] = {128, 1024, 4096, 65536, 131072};
	int num = sizeof(overlap_sizes) / sizeof(overlap_sizes[0]);

	for (int s = 0; s < num; s++) {
		size_t n = overlap_sizes[s];
		size_t offset = n / 4;

		fill_pattern(buf, n + offset);
		fill_pattern(ref, n + offset);

		/* Reference: manual backward copy (correct for forward overlap) */
		for (size_t i = n; i > 0; i--)
			ref[offset + i - 1] = ref[i - 1];

		memmove(buf + offset, buf, n);

		if (!verify_equal(buf + offset, ref + offset, n)) {
			fprintf(stderr, "    memmove (overlap forward) failed at size %zu\n", n);
			free(buf);
			free(ref);
			return 1;
		}
	}

	free(buf);
	free(ref);
	return 0;
}

static int test_memmove_overlap_backward(void)
{
	/* Overlapping backward: dst < src, src within [dst, dst+n) */
	size_t buf_size = MAX_SIZE + 4096;
	uint8_t *buf = alloc_aligned(buf_size);
	uint8_t *ref = alloc_aligned(buf_size);

	ASSERT_TRUE(buf != NULL && ref != NULL);

	size_t overlap_sizes[] = {128, 1024, 4096, 65536, 131072};
	int num = sizeof(overlap_sizes) / sizeof(overlap_sizes[0]);

	for (int s = 0; s < num; s++) {
		size_t n = overlap_sizes[s];
		size_t offset = n / 4;

		fill_pattern(buf, n + offset);
		fill_pattern(ref, n + offset);

		/* Reference: forward copy (correct for backward overlap) */
		copy_forward(ref, ref + offset, n);

		memmove(buf, buf + offset, n);

		if (!verify_equal(buf, ref, n)) {
			fprintf(stderr, "    memmove (overlap backward) failed at size %zu\n", n);
			free(buf);
			free(ref);
			return 1;
		}
	}

	free(buf);
	free(ref);
	return 0;
}

/* ---- memcmp tests ---- */

static int test_memcmp_equal(void)
{
	uint8_t *a = alloc_aligned(MAX_SIZE);
	uint8_t *b = alloc_aligned(MAX_SIZE);

	ASSERT_TRUE(a != NULL && b != NULL);

	fill_pattern(a, MAX_SIZE);
	fill_pattern(b, MAX_SIZE);

	for (size_t s = 0; s < NUM_SIZES; s++) {
		size_t n = test_sizes[s];

		if (memcmp(a, b, n) != 0) {
			fprintf(stderr, "    memcmp(equal) returned non-zero at size %zu\n", n);
			free(a);
			free(b);
			return 1;
		}
	}

	free(a);
	free(b);
	return 0;
}

static int test_memcmp_differ(void)
{
	uint8_t *a = alloc_aligned(MAX_SIZE);
	uint8_t *b = alloc_aligned(MAX_SIZE);

	ASSERT_TRUE(a != NULL && b != NULL);

	for (size_t s = 0; s < NUM_SIZES; s++) {
		size_t n = test_sizes[s];

		if (n == 0)
			continue;

		fill_pattern(a, n);
		fill_pattern(b, n);

		/* Make last byte different */
		b[n - 1] = (uint8_t)(~a[n - 1]);

		int result = memcmp(a, b, n);

		if (result == 0) {
			fprintf(stderr, "    memcmp returned 0 for different buffers at size %zu\n", n);
			free(a);
			free(b);
			return 1;
		}

		/* Verify sign matches expectation */
		int expected_sign = (a[n - 1] > b[n - 1]) ? 1 : -1;
		int actual_sign = (result > 0) ? 1 : -1;

		if (actual_sign != expected_sign) {
			fprintf(stderr, "    memcmp sign mismatch at size %zu: got %d, expected %d\n",
				n, actual_sign, expected_sign);
			free(a);
			free(b);
			return 1;
		}
	}

	free(a);
	free(b);
	return 0;
}

/* ---- Edge case tests ---- */

static int test_zero_length(void)
{
	uint8_t a[16], b[16], save_a[16];

	fill_pattern(a, 16);
	clear_buf(b, 16);

	/* Save original a */
	copy_forward(save_a, a, 16);

	/* memset with n=0 should not modify buffer */
	memset(a, 0xFF, 0);
	ASSERT_TRUE(verify_equal(a, save_a, 16));

	/* memcpy with n=0 should not modify dst */
	memcpy(b, a, 0);
	ASSERT_TRUE(verify_set(b, 0, 16));

	/* memmove with n=0 should not modify dst */
	memmove(b, a, 0);
	ASSERT_TRUE(verify_set(b, 0, 16));

	/* memcmp with n=0 should return 0 */
	ASSERT_EQ(memcmp(a, b, 0), 0);

	return 0;
}

static int test_memcpy_unaligned(void)
{
	size_t buf_size = MAX_SIZE + 64;
	uint8_t *src_base = alloc_aligned(buf_size);
	uint8_t *dst_base = alloc_aligned(buf_size);

	ASSERT_TRUE(src_base != NULL && dst_base != NULL);

	int offsets[] = {1, 3, 7, 13, 31, 63};
	int num_offsets = sizeof(offsets) / sizeof(offsets[0]);
	size_t sizes[] = {64, 1024, 4096, 65536, 131072};
	int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

	for (int o = 0; o < num_offsets; o++) {
		uint8_t *src = src_base + offsets[o];
		uint8_t *dst = dst_base + offsets[o];

		fill_pattern(src, MAX_SIZE);

		for (int s = 0; s < num_sizes; s++) {
			size_t n = sizes[s];

			clear_buf(dst, n);
			memcpy(dst, src, n);

			if (!verify_equal(dst, src, n)) {
				fprintf(stderr, "    memcpy (unaligned offset=%d) failed at size %zu\n",
					offsets[o], n);
				free(src_base);
				free(dst_base);
				return 1;
			}
		}
	}

	free(src_base);
	free(dst_base);
	return 0;
}

/* ---- Multithreaded correctness test ---- */

#define MT_NUM_THREADS 4
#define MT_BUF_SIZE (128 * 1024)
#define MT_ITERS 100

struct mt_result {
	int passed;
	int thread_id;
};

static void *mt_worker(void *arg)
{
	struct mt_result *result = (struct mt_result *)arg;
	uint8_t *src = alloc_aligned(MT_BUF_SIZE);
	uint8_t *dst = alloc_aligned(MT_BUF_SIZE);

	result->passed = 1;

	if (!src || !dst) {
		result->passed = 0;
		free(src);
		free(dst);
		return NULL;
	}

	for (int i = 0; i < MT_ITERS; i++) {
		uint8_t pattern = (uint8_t)(i & 0xFF);

		memset(src, pattern, MT_BUF_SIZE);
		if (!verify_set(src, pattern, MT_BUF_SIZE)) {
			fprintf(stderr, "    thread %d: memset failed at iter %d\n",
				result->thread_id, i);
			result->passed = 0;
			break;
		}

		memcpy(dst, src, MT_BUF_SIZE);
		if (!verify_equal(dst, src, MT_BUF_SIZE)) {
			fprintf(stderr, "    thread %d: memcpy failed at iter %d\n",
				result->thread_id, i);
			result->passed = 0;
			break;
		}
	}

	free(src);
	free(dst);
	return NULL;
}

static int test_multithread(void)
{
	pthread_t threads[MT_NUM_THREADS];
	struct mt_result results[MT_NUM_THREADS];

	for (int i = 0; i < MT_NUM_THREADS; i++) {
		results[i].thread_id = i;
		pthread_create(&threads[i], NULL, mt_worker, &results[i]);
	}

	for (int i = 0; i < MT_NUM_THREADS; i++)
		pthread_join(threads[i], NULL);

	for (int i = 0; i < MT_NUM_THREADS; i++) {
		if (!results[i].passed) {
			fprintf(stderr, "    Thread %d failed\n", i);
			return 1;
		}
	}

	return 0;
}

/* ---- Test runner ---- */

/* ---- Explicit asynchronous API (dto.h) ----
 *
 * Without DSA every submit falls back and the tests check the documented
 * fallback paths. With DTO_TEST_EXPECT_DSA set (a usable work queue
 * configured), operations on faulted-in buffers must be submitted and must
 * complete on the device, so a regression that quietly pushes work back onto
 * the CPU fails instead of passing.
 */

static int expect_dsa;

/* CRC32C loop over the SSE4.2 instruction: crc32c_loop(0, ...) is the raw
 * presentation and ~crc32c_loop(~seed, ...) the seeded (iSCSI) one. */
__attribute__((target("sse4.2")))
static uint32_t crc32c_loop(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	for (size_t i = 0; i < n; i++)
		crc = _mm_crc32_u8(crc, p[i]);
	return crc;
}

static uint32_t ref_crc_raw(const void *buf, size_t n)
{
	return crc32c_loop(0, buf, n);
}

static uint32_t ref_crc32c(uint32_t seed, const void *buf, size_t n)
{
	return ~crc32c_loop(~seed, buf, n);
}

/* Poll until poll_expr leaves PENDING and yield its result. A submitted op
 * owns its (stack-allocated) state until it completes, so a stall past the
 * 10s safety timeout aborts the process rather than returning into a test
 * that would let the device write into a dead stack frame. */
#define ASYNC_WAIT(poll_expr) ({ \
	struct timespec _start, _now; \
	int _rc; \
	clock_gettime(CLOCK_MONOTONIC, &_start); \
	while ((_rc = (poll_expr)) == DTO_ASYNC_PENDING) { \
		clock_gettime(CLOCK_MONOTONIC, &_now); \
		if (_now.tv_sec - _start.tv_sec > 10) { \
			fprintf(stderr, "    FATAL: async op still pending after 10s at %s:%d\n", \
				__FILE__, __LINE__); \
			abort(); \
		} \
	} \
	_rc; })

/* The outcome of a submit, then of its poll: SUBMITTED and DONE when DSA is
 * expected, any documented outcome otherwise. */
#define CHECK_SUBMIT(rc) do { \
	if (expect_dsa) \
		ASSERT_EQ((rc), DTO_ASYNC_SUBMITTED); \
	else \
		ASSERT_TRUE((rc) == DTO_ASYNC_SUBMITTED || (rc) == DTO_ASYNC_FALLBACK); \
} while (0)

#define CHECK_DONE(rc) do { \
	if (expect_dsa) \
		ASSERT_EQ((rc), DTO_ASYNC_DONE); \
	else \
		ASSERT_TRUE((rc) == DTO_ASYNC_DONE || (rc) == DTO_ASYNC_FAILED); \
} while (0)

/* Wait for a submitted op the documented way: wait, then poll. */
static int finish(dto_async_op *op)
{
	dto_async_wait(op);
	return ASYNC_WAIT(dto_async_poll(op));
}

/* Submit a memcpy with the given flags and verify the data afterwards,
 * whichever of the three documented outcomes (DONE, FAILED + CPU redo,
 * FALLBACK + CPU copy) occurs. */
static int check_async_memcpy(unsigned int flags)
{
	const size_t n = 1 << 20;
	uint8_t *src = malloc(n), *dst = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(src && dst);
	fill_pattern(src, n);
	clear_buf(dst, n);

	rc = dto_submit_memcpy(&op, dst, src, n, flags);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		CHECK_DONE(rc);
		if (rc != DTO_ASYNC_DONE)
			memcpy(dst, src, n);	/* documented caller fallback */
	} else {
		memcpy(dst, src, n);
	}
	ASSERT_TRUE(verify_equal(dst, src, n));
	free(src);
	free(dst);
	return 0;
}

/* The general tests use faulted-in buffers and no DTO_SUBMIT_BOF, so they
 * complete on any work queue; test_async_bof covers block-on-fault. */
static int test_async_memcpy(void)
{
	return check_async_memcpy(DTO_SUBMIT_CC);
}

static int test_async_memcpy_no_cc(void)
{
	return check_async_memcpy(0);
}

static int test_async_memset(void)
{
	const size_t n = (1 << 20) + 3;
	uint8_t *dst = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(dst != NULL);
	clear_buf(dst, n);

	rc = dto_submit_memset(&op, dst, 0x5A, n, DTO_SUBMIT_CC);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		CHECK_DONE(rc);
	}
	if (rc != DTO_ASYNC_DONE)
		memset(dst, 0x5A, n);
	ASSERT_TRUE(verify_set(dst, 0x5A, n));
	free(dst);
	return 0;
}

static int test_async_memfill(void)
{
	const uint64_t pattern = 0x0123456789ABCDEFULL;
	const size_t n = (256 * 1024) + 5;	/* not a multiple of 8 */
	uint8_t *dst = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(dst != NULL);
	clear_buf(dst, n);

	rc = dto_submit_memfill(&op, dst, pattern, n, DTO_SUBMIT_CC);
	CHECK_SUBMIT(rc);
	if (rc != DTO_ASYNC_SUBMITTED)
		return 0;
	rc = finish(&op);
	CHECK_DONE(rc);
	if (rc == DTO_ASYNC_DONE) {
		for (size_t i = 0; i < n; i++)
			ASSERT_EQ(dst[i], (uint8_t)(pattern >> (8 * (i & 7))));
	}
	free(dst);
	return 0;
}

static int test_async_crc(void)
{
	const size_t n = 256 * 1024 + 3;
	uint8_t *src = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(src != NULL);
	fill_pattern(src, n);

	rc = dto_submit_crc(&op, src, n, 0);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		CHECK_DONE(rc);
		if (rc == DTO_ASYNC_DONE)
			ASSERT_EQ(dto_async_crc_val(&op), ref_crc_raw(src, n));
	}
	free(src);
	return 0;
}

static int test_async_memcpy_crc(void)
{
	const size_t n = 256 * 1024;
	uint8_t *src = malloc(n), *dst = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(src && dst);
	fill_pattern(src, n);
	clear_buf(dst, n);

	rc = dto_submit_memcpy_crc(&op, dst, src, n, DTO_SUBMIT_CC);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		CHECK_DONE(rc);
	}
	if (rc == DTO_ASYNC_DONE)
		ASSERT_EQ(dto_async_crc_val(&op), ref_crc_raw(src, n));
	else
		memcpy(dst, src, n);
	ASSERT_TRUE(verify_equal(dst, src, n));
	free(src);
	free(dst);
	return 0;
}

/* Seeded (iSCSI) CRC32C: seed 0 is the standard CRC32C, and chaining two
 * halves gives the CRC of the whole. */
static int test_async_crc32c(void)
{
	const size_t n = 128 * 1024, half = n / 2;
	uint8_t *src = malloc(n), *dst = malloc(n);
	uint32_t first;
	dto_async_op op;
	int rc;

	ASSERT_TRUE(src && dst);
	fill_pattern(src, n);
	clear_buf(dst, n);

	/* the reference itself: "123456789" is the CRC32C check string */
	ASSERT_EQ(ref_crc32c(0, "123456789", 9), 0xE3069283u);

	rc = dto_submit_crc32c(&op, src, half, 0, 0);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		CHECK_DONE(rc);
		if (rc == DTO_ASYNC_DONE) {
			first = dto_async_crc32c_val(&op);
			ASSERT_EQ(first, ref_crc32c(0, src, half));
			rc = dto_submit_memcpy_crc32c(&op, dst + half, src + half,
						      n - half, first, DTO_SUBMIT_CC);
			CHECK_SUBMIT(rc);
			if (rc == DTO_ASYNC_SUBMITTED && finish(&op) == DTO_ASYNC_DONE) {
				ASSERT_EQ(dto_async_crc32c_val(&op),
					  ref_crc32c(0, src, n));
				ASSERT_TRUE(verify_equal(dst + half, src + half, n - half));
			}
		}
	}

	/* the synchronous call is always right: CPU below DTO_CRC_MIN_BYTES,
	 * DSA above, and the seed alone for an empty buffer */
	ASSERT_EQ(dto_crc32c_with_seed(0, src, 100), ref_crc32c(0, src, 100));
	ASSERT_EQ(dto_crc32c_with_seed(0, src, n), ref_crc32c(0, src, n));
	ASSERT_EQ(dto_crc32c_with_seed(dto_crc32c_with_seed(0, src, half),
				       src + half, n - half),
		  ref_crc32c(0, src, n));
	ASSERT_EQ(dto_crc32c_with_seed(0x12345678u, src, 0), 0x12345678u);
	free(src);
	free(dst);
	return 0;
}

/* Offset of the first difference, as compare reports it */
static int check_compare(const uint8_t *a, const uint8_t *b, size_t n,
			 int expect_result, uint32_t expect_offset)
{
	dto_async_op op;
	int rc = dto_submit_compare(&op, a, b, n, 0);

	CHECK_SUBMIT(rc);
	if (rc != DTO_ASYNC_SUBMITTED)
		return 0;
	rc = finish(&op);
	CHECK_DONE(rc);
	if (rc == DTO_ASYNC_DONE) {
		ASSERT_EQ(dto_async_result(&op), expect_result);
		if (expect_result)
			ASSERT_EQ(dto_async_bytes_completed(&op), expect_offset);
	}
	return 0;
}

static int test_async_compare(void)
{
	static const size_t offsets[] = { 0, 1, 7, 8, 63, 4095, 4096, 100003 };
	const size_t n = 128 * 1024;
	uint8_t *a = malloc(n), *b = malloc(n);

	ASSERT_TRUE(a && b);
	fill_pattern(a, n);
	fill_pattern(b, n);
	if (check_compare(a, b, n, 0, 0))
		return 1;
	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		b[offsets[i]] ^= 0x40;
		if (check_compare(a, b, n, 1, offsets[i]))
			return 1;
		b[offsets[i]] ^= 0x40;
	}
	b[n - 1] ^= 1;
	if (check_compare(a, b, n, 1, n - 1))
		return 1;
	free(a);
	free(b);
	return 0;
}

static int test_async_compval(void)
{
	const uint64_t pattern = 0xCCCCCCCC00000000ULL | 0x5A5A0F0F;
	const size_t n = 64 * 1024;
	uint8_t *buf = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(buf != NULL);
	for (size_t i = 0; i < n; i++)
		((volatile uint8_t *)buf)[i] = (uint8_t)(pattern >> (8 * (i & 7)));

	for (int round = 0; round < 2; round++) {
		/* round 0: buf is the pattern; round 1: one byte differs */
		const size_t off = 12345;

		if (round == 1)
			buf[off] ^= 0xFF;
		rc = dto_submit_compval(&op, buf, pattern, n, 0);
		CHECK_SUBMIT(rc);
		if (rc != DTO_ASYNC_SUBMITTED)
			continue;
		rc = finish(&op);
		CHECK_DONE(rc);
		if (rc != DTO_ASYNC_DONE)
			continue;
		ASSERT_EQ(dto_async_result(&op), round);
		if (round == 1)
			ASSERT_EQ(dto_async_bytes_completed(&op), off);
	}
	free(buf);
	return 0;
}

static int test_async_transl_fetch(void)
{
	const size_t n = 1 << 20;
	uint8_t *buf = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(buf != NULL);
	clear_buf(buf, n);
	rc = dto_submit_transl_fetch(&op, buf, n, 0);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = finish(&op);
		/* devices without the operation report BAD_OPCODE */
		if (rc != DTO_ASYNC_DONE) {
			ASSERT_EQ(rc, DTO_ASYNC_FAILED);
			ASSERT_EQ(dto_async_status(&op), 0x10);
		}
	}
	free(buf);
	return 0;
}

/* DTO_SUBMIT_BOF is honored only on a work queue with block-on-fault; on one
 * without it DTO drops the flag rather than let the device reject the
 * descriptor (dto.h). Copy into a destination whose first pages are touched
 * and the rest not:
 *   BOF set, WQ with block-on-fault:    the device waits; the copy completes
 *   BOF set, WQ without block-on-fault: flag dropped: stops at the first
 *                                       untouched page, status 0x03
 *   BOF not set, any WQ:                stops at the first untouched page,
 *                                       status 0x03, the touched part done
 * Whether the work queue has block-on-fault is learned from that first BOF
 * copy; a BOF submit is never rejected with 0x11 (invalid flags) any more.
 * Assumes every work queue DTO uses has the same block-on-fault setting. */
static int test_async_bof(void)
{
	const size_t page = 4096, n = 16 * page, warm = 4 * page;
	uint8_t *src = malloc(n), *hot = malloc(n), *cold;
	dto_batch *b = dto_batch_create(2);
	dto_async_op op;
	int rc, wq_bof = -1;

	ASSERT_TRUE(src && hot && b);
	fill_pattern(src, n);
	clear_buf(hot, n);

	/* On faulted-in memory a BOF submit completes on any work queue. */
	rc = dto_submit_memcpy(&op, hot, src, n, DTO_SUBMIT_BOF);
	CHECK_SUBMIT(rc);
	if (rc != DTO_ASYNC_SUBMITTED)
		return 0;
	ASSERT_EQ(finish(&op), DTO_ASYNC_DONE);
	ASSERT_TRUE(verify_equal(hot, src, n));

	for (int bof = 1; bof >= 0; bof--) {
		uint8_t *dst = mmap(NULL, n, PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		ASSERT_TRUE(dst != MAP_FAILED);
		clear_buf(dst, warm);	/* touch the first pages only */
		rc = dto_submit_memcpy(&op, dst, src, n, bof ? DTO_SUBMIT_BOF : 0);
		CHECK_SUBMIT(rc);
		if (rc == DTO_ASYNC_SUBMITTED) {
			rc = finish(&op);
			if (bof) {
				wq_bof = rc == DTO_ASYNC_DONE;
				printf("    (work queue block-on-fault: %s)\n",
				       wq_bof ? "enabled" : "disabled");
			}
			if (bof && wq_bof) {
				ASSERT_TRUE(verify_equal(dst, src, n));
			} else {
				/* no BOF in effect: partial, never 0x11 */
				ASSERT_EQ(rc, DTO_ASYNC_FAILED);
				ASSERT_EQ(dto_async_status(&op), 0x03);
				ASSERT_EQ(dto_async_bytes_completed(&op), warm);
				ASSERT_TRUE(verify_equal(dst, src, warm));
			}
		}
		munmap(dst, n);
	}

	/* The same per batch operation: a BOF member into untouched pages
	 * completes on a block-on-fault queue and is otherwise stopped and
	 * redone by dto_batch_poll; the member on hot memory always completes. */
	cold = mmap(NULL, n / 2, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_TRUE(cold != MAP_FAILED);
	clear_buf(hot, n);
	ASSERT_EQ(dto_batch_add_memmove(b, cold, src, n / 2, DTO_SUBMIT_BOF), 0);
	ASSERT_EQ(dto_batch_add_memmove(b, hot, src + n / 2, n / 2, DTO_SUBMIT_BOF), 1);
	rc = dto_batch_submit(b);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		ASSERT_EQ(ASYNC_WAIT(dto_batch_poll(b)), DTO_ASYNC_DONE);
		if (wq_bof >= 0)
			ASSERT_EQ(dto_batch_status(b, 0), wq_bof ? 1 : 0x03);
		ASSERT_EQ(dto_batch_status(b, 1), 1);
		ASSERT_TRUE(verify_equal(cold, src, n / 2));
		ASSERT_TRUE(verify_equal(hot, src + n / 2, n / 2));
	}
	munmap(cold, n / 2);
	dto_batch_destroy(b);
	free(src);
	free(hot);
	return 0;
}

/* dto_async_wait must return at once for ops that were never submitted or
 * fell back: their status byte will never be written. */
static int test_async_wait_unsubmitted(void)
{
	dto_async_op op;
	uint8_t a[64], b[64];

	memset(&op, 0xAB, sizeof(op));
	dto_async_wait(&op);
	ASSERT_EQ(dto_submit_memcpy(&op, a, b, 0, 0), DTO_ASYNC_FALLBACK);
	dto_async_wait(&op);
	ASSERT_EQ(dto_wait_blocks(), 0);
	return 0;
}

static int test_async_bad_args(void)
{
	uint8_t a[64], b[64];
	struct { char pad[16]; dto_async_op op; } __attribute__((aligned(64))) s;
	dto_async_op *unaligned = (dto_async_op *)&s.pad[8];
	dto_async_op op;

	/* zero length never reaches the device */
	ASSERT_EQ(dto_submit_memcpy(&op, a, b, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_memset(&op, a, 0, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_memfill(&op, a, 0, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_crc(&op, b, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_crc32c(&op, b, 0, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_memcpy_crc(&op, a, b, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_compare(&op, a, b, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_compval(&op, a, 0, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_transl_fetch(&op, a, 0, 0), DTO_ASYNC_FALLBACK);
	/* the completion record needs 32-byte alignment */
	ASSERT_EQ(dto_submit_memcpy(unaligned, a, b, sizeof(a), 0),
		  DTO_ASYNC_FALLBACK);
	return 0;
}

/* ---- Batches ---- */

static int test_batch_limits(void)
{
	dto_batch *b = dto_batch_create(DTO_BATCH_MAX + 5);
	uint8_t *buf = aligned_alloc(4096, 3 * 4096);
	int i;

	ASSERT_TRUE(b && buf);
	clear_buf(buf, 3 * 4096);
	/* polling a batch that was never submitted reports FAILED */
	ASSERT_EQ(dto_batch_poll(b), DTO_ASYNC_FAILED);
	/* an empty batch completes at once */
	ASSERT_EQ(dto_batch_submit(b), DTO_ASYNC_SUBMITTED);
	ASSERT_EQ(dto_batch_poll(b), DTO_ASYNC_DONE);
	/* a submitted batch takes no more operations until reset */
	ASSERT_EQ(dto_batch_add_memfill(b, buf, 0, 64, 0), -1);
	dto_batch_reset(b);

	ASSERT_EQ(dto_batch_add_memmove(b, buf, buf + 64, 0, 0), -1);
	/* dualcast destinations must agree in bits 11:0 */
	ASSERT_EQ(dto_batch_add_dualcast(b, buf + 4096, buf + 2 * 4096 + 64,
					 buf, 64, 0), -1);
	/* the capacity is clamped to DTO_BATCH_MAX */
	for (i = 0; i < DTO_BATCH_MAX; i++)
		ASSERT_EQ(dto_batch_add_compval(b, buf, 0, 64, 0), i);
	ASSERT_EQ(dto_batch_add_compval(b, buf, 0, 64, 0), -1);
	ASSERT_EQ(dto_batch_count(b), DTO_BATCH_MAX);
	dto_batch_reset(b);
	ASSERT_EQ(dto_batch_count(b), 0);
	dto_batch_destroy(b);
	dto_batch_destroy(NULL);
	free(buf);
	return 0;
}

/* Every operation kind in one batch, checked against CPU results. */
static int run_batch_all_ops(int nops_limit)
{
	const size_t n = 32 * 1024 + 8;
	const uint64_t pattern = 0x1122334455667788ULL;
	uint8_t *src = aligned_alloc(4096, n + 4096);
	uint8_t *src2 = aligned_alloc(4096, n + 4096);
	uint8_t *mv = aligned_alloc(4096, n + 4096);
	uint8_t *fill = aligned_alloc(4096, n + 4096);
	uint8_t *d1 = aligned_alloc(4096, n + 4096);
	uint8_t *d2 = aligned_alloc(4096, n + 4096);
	uint8_t *cc = aligned_alloc(4096, n + 4096);
	uint8_t *pat = aligned_alloc(4096, n + 4096);
	dto_batch *b = dto_batch_create(16);
	int ops[7], count = 0, rc;

	ASSERT_TRUE(src && src2 && mv && fill && d1 && d2 && cc && pat && b);
	/* operations in one batch may run concurrently, so none of them reads
	 * what another writes */
	for (size_t i = 0; i < n; i++)
		((volatile uint8_t *)pat)[i] = (uint8_t)(pattern >> (8 * (i & 7)));
	fill_pattern(src, n);
	fill_pattern(src2, n);
	src2[777] ^= 1;
	clear_buf(mv, n);
	clear_buf(fill, n);
	clear_buf(d1, n);
	clear_buf(d2, n);
	clear_buf(cc, n);

	ops[0] = dto_batch_add_memmove(b, mv, src, n, DTO_SUBMIT_CC);
	ops[1] = dto_batch_add_memfill(b, fill, pattern, n, 0);
	ops[2] = dto_batch_add_dualcast(b, d1, d2, src, n, 0);
	ops[3] = dto_batch_add_crc(b, src, n, 0);
	ops[4] = dto_batch_add_memcpy_crc(b, cc, src, n, DTO_SUBMIT_CC);
	ops[5] = dto_batch_add_compare(b, src, src2, n, 0);
	ops[6] = dto_batch_add_compval(b, pat, pattern, n, 0);
	for (int i = 0; i < 7; i++)
		ASSERT_EQ(ops[i], i);
	count = 7;
	/* nops_limit 1 exercises the plain-descriptor path */
	if (nops_limit == 1) {
		dto_batch_reset(b);
		ASSERT_EQ(dto_batch_add_memcpy_crc(b, cc, src, n, 0), 0);
		count = 1;
	}

	rc = dto_batch_submit(b);
	CHECK_SUBMIT(rc);
	if (rc != DTO_ASYNC_SUBMITTED)
		return 0;
	dto_batch_wait(b);
	ASSERT_EQ(ASYNC_WAIT(dto_batch_poll(b)), DTO_ASYNC_DONE);
	ASSERT_EQ(dto_batch_poll(b), DTO_ASYNC_DONE);	/* stays DONE */
	for (int i = 0; i < count; i++) {
		if (expect_dsa)
			ASSERT_EQ(dto_batch_status(b, i), 1);
	}

	if (count == 1) {
		ASSERT_TRUE(verify_equal(cc, src, n));
		ASSERT_EQ(dto_batch_crc(b, 0), ref_crc_raw(src, n));
	} else {
		ASSERT_TRUE(verify_equal(mv, src, n));
		for (size_t i = 0; i < n; i++)
			ASSERT_EQ(fill[i], (uint8_t)(pattern >> (8 * (i & 7))));
		ASSERT_TRUE(verify_equal(d1, src, n));
		ASSERT_TRUE(verify_equal(d2, src, n));
		ASSERT_EQ(dto_batch_crc(b, 3), ref_crc_raw(src, n));
		ASSERT_TRUE(verify_equal(cc, src, n));
		ASSERT_EQ(dto_batch_crc(b, 4), ref_crc_raw(src, n));
		ASSERT_EQ(dto_batch_result(b, 5), 1);
		ASSERT_EQ(dto_batch_bytes_completed(b, 5), 777);
		ASSERT_EQ(dto_batch_result(b, 6), 0);
	}
	dto_batch_destroy(b);
	free(src);
	free(src2);
	free(mv);
	free(fill);
	free(d1);
	free(d2);
	free(cc);
	free(pat);
	return 0;
}

static int test_batch_all_ops(void)
{
	return run_batch_all_ops(0);
}

static int test_batch_single_op(void)
{
	return run_batch_all_ops(1);
}

/* Operations the device cannot finish are redone by dto_batch_poll. Fresh
 * anonymous pages are not mapped until first touched, so without
 * block-on-fault the device stops at them; the results must still be exactly
 * what the CPU computes, including for overlapping moves the device left
 * half done in either direction. */
static int test_batch_repair(void)
{
	const size_t page = 4096, region = 16 * page;
	const uint64_t pattern = 0xA5A55A5A0F0FF0F0ULL;
	uint8_t *m = mmap(NULL, 8 * region, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint8_t *up, *down, *cold, *fill, *cmp, *cc;
	uint8_t *exp_up, *exp_down;
	const size_t mv = 8 * page;	/* overlapping move length */
	dto_batch *b = dto_batch_create(8);
	int rc;

	ASSERT_TRUE(m != MAP_FAILED && b);
	up = m;			/* ascending move: low pages touched */
	down = m + region;	/* descending move: high pages touched */
	cold = m + 2 * region;	/* never touched: reads as zero */
	fill = m + 3 * region;
	cmp = m + 4 * region;
	cc = m + 5 * region;
	exp_up = malloc(region);
	exp_down = malloc(region);
	ASSERT_TRUE(exp_up && exp_down);

	/* up: move [4p, 12p) down to [0, 8p); the device copies upward and
	 * stops at the untouched pages from 10p, after overwriting [4p, 6p)
	 * of its own source, so redoing the whole move would be wrong. */
	for (size_t i = 0; i < 10 * page; i++)
		((volatile uint8_t *)up)[i] = (uint8_t)(i * 7);
	for (size_t i = 0; i < region; i++)
		exp_up[i] = i < 10 * page ? (uint8_t)(i * 7) : 0;
	for (size_t i = 0; i < mv; i++)	/* forward copy is memmove here */
		exp_up[i] = exp_up[4 * page + i];

	/* down: move [0, 8p) up to [4p, 12p); the device copies downward and
	 * stops at the untouched pages below 2p, after overwriting [6p, 8p)
	 * of its own source. */
	for (size_t i = 2 * page; i < region; i++)
		((volatile uint8_t *)down)[i] = (uint8_t)(i * 13);
	for (size_t i = 0; i < region; i++)
		exp_down[i] = i >= 2 * page ? (uint8_t)(i * 13) : 0;
	for (size_t i = mv; i-- > 0;)	/* backward copy is memmove here */
		exp_down[4 * page + i] = exp_down[i];

	ASSERT_TRUE(dto_batch_add_memmove(b, up, up + 4 * page, mv, 0) == 0);
	ASSERT_TRUE(dto_batch_add_memmove(b, down + 4 * page, down, mv, 0) == 1);
	ASSERT_TRUE(dto_batch_add_memfill(b, fill, pattern, region, 0) == 2);
	ASSERT_TRUE(dto_batch_add_crc(b, cold, region, 0) == 3);
	ASSERT_TRUE(dto_batch_add_compval(b, cold, 0, region, 0) == 4);
	ASSERT_TRUE(dto_batch_add_compare(b, cold, cmp, region, 0) == 5);
	ASSERT_TRUE(dto_batch_add_memcpy_crc(b, cc, cold, region, 0) == 6);

	rc = dto_batch_submit(b);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		ASSERT_EQ(ASYNC_WAIT(dto_batch_poll(b)), DTO_ASYNC_DONE);
		ASSERT_TRUE(verify_equal(up, exp_up, region));
		ASSERT_TRUE(verify_equal(down, exp_down, region));
		for (size_t i = 0; i < region; i++)
			ASSERT_EQ(fill[i], (uint8_t)(pattern >> (8 * (i & 7))));
		ASSERT_EQ(dto_batch_crc(b, 3), ref_crc_raw(cold, region));
		ASSERT_EQ(dto_batch_result(b, 4), 0);
		ASSERT_EQ(dto_batch_result(b, 5), 0);
		ASSERT_EQ(dto_batch_crc(b, 6), ref_crc_raw(cold, region));
		ASSERT_TRUE(verify_set(cc, 0, region));
	}
	dto_batch_destroy(b);
	munmap(m, 8 * region);
	free(exp_up);
	free(exp_down);
	return 0;
}

/* Two threads polling one batch that needs repair: the overlapping move is
 * not idempotent, so a repair done twice would show. */
struct poll_arg {
	dto_batch *b;
	int rc;
};

static void *poller(void *p)
{
	struct poll_arg *a = p;

	a->rc = ASYNC_WAIT(dto_batch_poll(a->b));
	return NULL;
}

static int test_batch_concurrent_poll(void)
{
	const size_t page = 4096, region = 16 * page, mv = 8 * page;
	uint8_t *m = mmap(NULL, region, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint8_t *exp = malloc(region);
	dto_batch *b = dto_batch_create(2);
	struct poll_arg args[2];
	pthread_t t[2];
	int rc;

	ASSERT_TRUE(m != MAP_FAILED && exp && b);
	for (size_t i = 2 * page; i < region; i++)
		((volatile uint8_t *)m)[i] = (uint8_t)(i * 3);
	for (size_t i = 0; i < region; i++)
		exp[i] = i >= 2 * page ? (uint8_t)(i * 3) : 0;
	for (size_t i = mv; i-- > 0;)
		exp[4 * page + i] = exp[i];

	ASSERT_EQ(dto_batch_add_memmove(b, m + 4 * page, m, mv, 0), 0);
	rc = dto_batch_submit(b);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		for (int i = 0; i < 2; i++) {
			args[i].b = b;
			pthread_create(&t[i], NULL, poller, &args[i]);
		}
		for (int i = 0; i < 2; i++) {
			pthread_join(t[i], NULL);
			ASSERT_EQ(args[i].rc, DTO_ASYNC_DONE);
		}
		ASSERT_TRUE(verify_equal(m, exp, region));
	}
	dto_batch_destroy(b);
	munmap(m, region);
	free(exp);
	return 0;
}

static int test_async_batch_copy(void)
{
	enum { COUNT = 8 };
	const size_t base = 16 * 1024;
	uint8_t *src[COUNT], *dst[COUNT];
	void *srcv[COUNT], *dstv[COUNT];
	size_t sizes[COUNT];
	dto_batch *b = dto_batch_create(COUNT);
	int rc;

	ASSERT_TRUE(b != NULL);
	for (int i = 0; i < COUNT; i++) {
		sizes[i] = base * (i + 1) + i * 64; /* varied, some unaligned */
		src[i] = malloc(sizes[i]);
		dst[i] = malloc(sizes[i]);
		ASSERT_TRUE(src[i] && dst[i]);
		fill_pattern(src[i], sizes[i]);
		clear_buf(dst[i], sizes[i]);
		srcv[i] = src[i];
		dstv[i] = dst[i];
	}

	/* more copies than the batch holds cannot be submitted */
	ASSERT_EQ(dto_submit_batch_copy(b, dstv, srcv, sizes, COUNT + 1, 0),
		  DTO_ASYNC_FALLBACK);
	rc = dto_submit_batch_copy(b, dstv, srcv, sizes, COUNT, DTO_SUBMIT_CC);
	CHECK_SUBMIT(rc);
	if (rc == DTO_ASYNC_SUBMITTED) {
		dto_batch_wait(b);
		ASSERT_EQ(ASYNC_WAIT(dto_batch_poll(b)), DTO_ASYNC_DONE);
	} else {
		for (int i = 0; i < COUNT; i++)
			memcpy(dst[i], src[i], sizes[i]);
	}
	for (int i = 0; i < COUNT; i++) {
		ASSERT_TRUE(verify_equal(dst[i], src[i], sizes[i]));
		free(src[i]);
		free(dst[i]);
	}
	dto_batch_destroy(b);
	return 0;
}

int main(void)
{
	expect_dsa = getenv("DTO_TEST_EXPECT_DSA") != NULL;

	printf("DTO Functional Tests%s\n",
	       expect_dsa ? " (DTO_TEST_EXPECT_DSA: offload required)" : "");
	printf("====================\n\n");

	struct dto_test tests[] = {
		TEST_ENTRY(test_memset_correctness),
		TEST_ENTRY(test_memcpy_correctness),
		TEST_ENTRY(test_memmove_nonoverlap),
		TEST_ENTRY(test_memmove_overlap_forward),
		TEST_ENTRY(test_memmove_overlap_backward),
		TEST_ENTRY(test_memcmp_equal),
		TEST_ENTRY(test_memcmp_differ),
		TEST_ENTRY(test_zero_length),
		TEST_ENTRY(test_memcpy_unaligned),
		TEST_ENTRY(test_multithread),
		TEST_ENTRY(test_async_memcpy),
		TEST_ENTRY(test_async_memcpy_no_cc),
		TEST_ENTRY(test_async_memset),
		TEST_ENTRY(test_async_memfill),
		TEST_ENTRY(test_async_crc),
		TEST_ENTRY(test_async_memcpy_crc),
		TEST_ENTRY(test_async_crc32c),
		TEST_ENTRY(test_async_compare),
		TEST_ENTRY(test_async_compval),
		TEST_ENTRY(test_async_transl_fetch),
		TEST_ENTRY(test_async_bof),
		TEST_ENTRY(test_async_wait_unsubmitted),
		TEST_ENTRY(test_async_bad_args),
		TEST_ENTRY(test_batch_limits),
		TEST_ENTRY(test_batch_all_ops),
		TEST_ENTRY(test_batch_single_op),
		TEST_ENTRY(test_batch_repair),
		TEST_ENTRY(test_batch_concurrent_poll),
		TEST_ENTRY(test_async_batch_copy),
		{NULL, NULL}
	};

	return run_tests(tests);
}
