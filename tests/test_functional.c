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
 * These use byte-by-byte loops so the compiler cannot optimize them
 * into memset/memcmp calls (which would be intercepted by DTO, defeating
 * the purpose of independent verification).
 */
static int __attribute__((optimize("no-tree-loop-distribute-patterns")))
verify_set(const uint8_t *buf, uint8_t val, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (buf[i] != val) {
			fprintf(stderr, "    byte[%zu] = 0x%02x, expected 0x%02x\n",
				i, buf[i], val);
			return 0;
		}
	}
	return 1;
}

static int __attribute__((optimize("no-tree-loop-distribute-patterns")))
verify_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i]) {
			fprintf(stderr, "    byte[%zu]: got 0x%02x, expected 0x%02x\n",
				i, a[i], b[i]);
			return 0;
		}
	}
	return 1;
}

static void __attribute__((optimize("no-tree-loop-distribute-patterns")))
fill_pattern(uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = (uint8_t)(i & 0xFF);
}

static void __attribute__((optimize("no-tree-loop-distribute-patterns")))
clear_buf(uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = 0;
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
		for (size_t i = 0; i < n; i++)
			ref[i] = ref[offset + i];

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
	for (int i = 0; i < 16; i++)
		save_a[i] = a[i];

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

/* ---- Explicit asynchronous API (dto.h) ---- */

/* Standard CRC32C (Castagnoli): init 0xFFFFFFFF, final inversion, the
 * convention dto.h documents for dto_async_crc_val. */
__attribute__((target("sse4.2")))
static uint32_t ref_crc32c(const void *buf, size_t n)
{
	const uint8_t *p = buf;
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++)
		crc = _mm_crc32_u8(crc, p[i]);
	return ~crc;
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
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = ASYNC_WAIT(dto_async_poll(&op));
		ASSERT_TRUE(rc == DTO_ASYNC_DONE || rc == DTO_ASYNC_FAILED);
		if (rc != DTO_ASYNC_DONE)
			memcpy(dst, src, n);	/* documented caller fallback */
	} else {
		ASSERT_EQ(rc, DTO_ASYNC_FALLBACK);
		memcpy(dst, src, n);
	}
	ASSERT_TRUE(verify_equal(dst, src, n));
	free(src);
	free(dst);
	return 0;
}

static int test_async_memcpy(void)
{
	return check_async_memcpy(DTO_SUBMIT_CC | DTO_SUBMIT_BOF);
}

static int test_async_memcpy_no_bof(void)
{
	/* Without DTO_SUBMIT_BOF a page fault aborts the operation and poll
	 * reports DTO_ASYNC_FAILED; the caller redoes the copy on the CPU.
	 * Either outcome is valid here, the data must be right afterwards. */
	return check_async_memcpy(0);
}

static int test_async_memset(void)
{
	const size_t n = 1 << 20;
	uint8_t *dst = malloc(n);
	dto_async_op op;
	int rc;

	ASSERT_TRUE(dst != NULL);
	clear_buf(dst, n);

	rc = dto_submit_memset(&op, dst, 0x5A, n, DTO_SUBMIT_BOF);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = ASYNC_WAIT(dto_async_poll(&op));
		ASSERT_TRUE(rc == DTO_ASYNC_DONE || rc == DTO_ASYNC_FAILED);
		if (rc != DTO_ASYNC_DONE)
			memset(dst, 0x5A, n);
	} else {
		ASSERT_EQ(rc, DTO_ASYNC_FALLBACK);
		memset(dst, 0x5A, n);
	}
	ASSERT_TRUE(verify_set(dst, 0x5A, n));
	free(dst);
	return 0;
}

/* BOF is rejected by the device on a WQ configured without block-on-fault
 * (dto.h), so like the other tests the CRC tests accept DTO_ASYNC_FAILED;
 * the CRC value is only checked when the device completed the op. */
static int test_async_crc(void)
{
	const size_t n = 256 * 1024;
	uint8_t *src = malloc(n);
	dto_async_op op;
	uint32_t expect;
	int rc;

	ASSERT_TRUE(src != NULL);
	fill_pattern(src, n);
	expect = ref_crc32c(src, n);

	rc = dto_submit_crc(&op, src, n, DTO_SUBMIT_BOF);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = ASYNC_WAIT(dto_async_poll(&op));
		ASSERT_TRUE(rc == DTO_ASYNC_DONE || rc == DTO_ASYNC_FAILED);
		if (rc == DTO_ASYNC_DONE)
			ASSERT_EQ(dto_async_crc_val(&op), expect);
	} else {
		ASSERT_EQ(rc, DTO_ASYNC_FALLBACK);
	}
	free(src);
	return 0;
}

static int test_async_memcpy_crc(void)
{
	const size_t n = 256 * 1024;
	uint8_t *src = malloc(n), *dst = malloc(n);
	dto_async_op op;
	uint32_t expect;
	int rc;

	ASSERT_TRUE(src && dst);
	fill_pattern(src, n);
	clear_buf(dst, n);
	expect = ref_crc32c(src, n);

	rc = dto_submit_memcpy_crc(&op, dst, src, n, DTO_SUBMIT_CC | DTO_SUBMIT_BOF);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = ASYNC_WAIT(dto_async_poll(&op));
		ASSERT_TRUE(rc == DTO_ASYNC_DONE || rc == DTO_ASYNC_FAILED);
		if (rc == DTO_ASYNC_DONE)
			ASSERT_EQ(dto_async_crc_val(&op), expect);
		else
			memcpy(dst, src, n);
	} else {
		ASSERT_EQ(rc, DTO_ASYNC_FALLBACK);
		memcpy(dst, src, n);
	}
	ASSERT_TRUE(verify_equal(dst, src, n));
	free(src);
	free(dst);
	return 0;
}

static int test_async_bad_args(void)
{
	uint8_t a[64], b[64];
	void *dstv[1] = { a }, *srcv[1] = { b };
	size_t sizes[1] = { 64 };
	dto_async_op op;
	dto_batch_op *bop;

	/* zero length never reaches the device */
	ASSERT_EQ(dto_submit_memcpy(&op, a, b, 0, 0), DTO_ASYNC_FALLBACK);
	ASSERT_EQ(dto_submit_crc(&op, b, 0, 0), DTO_ASYNC_FALLBACK);

	/* a DSA batch needs at least two descriptors */
	bop = dto_batch_op_new();
	ASSERT_TRUE(bop != NULL);
	ASSERT_EQ(dto_submit_batch_copy(bop, dstv, srcv, sizes, 1, 0),
		  DTO_ASYNC_FALLBACK);
	dto_batch_op_free(bop);
	return 0;
}

static int test_async_batch_copy(void)
{
	enum { COUNT = 8 };
	const size_t base = 16 * 1024;
	uint8_t *src[COUNT], *dst[COUNT];
	void *srcv[COUNT], *dstv[COUNT];
	size_t sizes[COUNT];
	dto_batch_op *bop;
	int rc;

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

	bop = dto_batch_op_new();
	ASSERT_TRUE(bop != NULL);
	rc = dto_submit_batch_copy(bop, dstv, srcv, sizes, COUNT, DTO_SUBMIT_BOF);
	if (rc == DTO_ASYNC_SUBMITTED) {
		rc = ASYNC_WAIT(dto_batch_poll(bop));
		ASSERT_EQ(rc, DTO_ASYNC_DONE);
		/* a completed batch stays DONE */
		ASSERT_EQ(dto_batch_poll(bop), DTO_ASYNC_DONE);
	} else {
		ASSERT_EQ(rc, DTO_ASYNC_FALLBACK);
		for (int i = 0; i < COUNT; i++)
			memcpy(dst[i], src[i], sizes[i]);
	}
	for (int i = 0; i < COUNT; i++) {
		ASSERT_TRUE(verify_equal(dst[i], src[i], sizes[i]));
		free(src[i]);
		free(dst[i]);
	}
	dto_batch_op_free(bop);
	return 0;
}

int main(void)
{
	printf("DTO Functional Tests\n");
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
		TEST_ENTRY(test_async_memset),
		TEST_ENTRY(test_async_crc),
		TEST_ENTRY(test_async_memcpy_crc),
		TEST_ENTRY(test_async_memcpy_no_bof),
		TEST_ENTRY(test_async_bad_args),
		TEST_ENTRY(test_async_batch_copy),
		{NULL, NULL}
	};

	return run_tests(tests);
}
