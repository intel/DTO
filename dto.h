/*******************************************************************************
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 * dto.h - Public asynchronous offload API for libdto.
 *
 * In addition to transparently accelerating memcpy/memmove/memset/memcmp via
 * LD_PRELOAD, libdto exposes an explicit submit/poll API for applications
 * that want to overlap accelerator data movement with CPU work.
 *
 * Lifecycle:
 *
 *   dto_async_op op;
 *   if (dto_submit_memcpy(&op, dst, src, n, 1) == DTO_ASYNC_SUBMITTED) {
 *       ... other CPU work ...
 *       while (dto_async_poll(&op) == DTO_ASYNC_PENDING)
 *           ; // pause/yield
 *       if (dto_async_poll(&op) != DTO_ASYNC_DONE)
 *           memcpy(dst, src, n);      // redo on the CPU
 *   } else {                          // DTO_ASYNC_FALLBACK: nothing submitted
 *       memcpy(dst, src, n);
 *   }
 *
 * The operation state is caller-allocated, so it can outlive the submitting
 * call and be polled from a different thread; nothing is stored in
 * thread-local state. dto_async_op is declared 64-byte aligned, which
 * automatic and static storage honour; heap-allocated ops must come from
 * aligned_alloc/posix_memalign (plain malloc does not guarantee it). An op
 * that is not at least 32-byte aligned is rejected with DTO_ASYNC_FALLBACK.
 *
 * Submission returns DTO_ASYNC_FALLBACK when the accelerator is unavailable
 * (not initialized, disabled via DTO_USESTDC_CALLS, no usable work queue) or
 * the request is out of range (n == 0, or larger than the work queue's
 * maximum transfer size). Nothing is copied in that case: the caller performs
 * the operation on the CPU. Unlike the transparent path, explicit submits are
 * not subject to the size heuristics (DTO_MIN_BYTES / auto-tuning): callers
 * decide what to offload.
 *
 * On DTO_ASYNC_FAILED (e.g. a page fault the device could not resolve) the
 * destination contents are unspecified; redo the whole operation on the CPU.
 *
 * @cache_control (DTO_SUBMIT_CC): directs the operation's output toward the
 * CPU cache (IDXD_OP_FLAG_CC) when the device supports it, for destinations
 * that will be read again soon.
 *
 * DTO_SUBMIT_BOF makes the device block on page faults and wait for the
 * kernel to resolve them, instead of aborting the operation (which surfaces
 * as DTO_ASYNC_FAILED from dto_async_poll). Use it when the buffers may not
 * be fully faulted in. The work queue must be configured with
 * block-on-fault enabled ("accel-config config-wq --block-on-fault=1"),
 * otherwise the device rejects the descriptor and the operation fails.
 ******************************************************************************/

#ifndef DTO_H
#define DTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-allocated operation state (opaque). */
typedef struct dto_async_op {
	unsigned char opaque[192] __attribute__((aligned(64)));
} dto_async_op;

/* Submission flags */
#define DTO_SUBMIT_CC  (1u << 0)	/* see @cache_control above */
#define DTO_SUBMIT_BOF (1u << 1)	/* block on page faults (see below) */

/* Submit results */
#define DTO_ASYNC_SUBMITTED 0
#define DTO_ASYNC_FALLBACK (-1)

/* Poll results */
#define DTO_ASYNC_PENDING 0
#define DTO_ASYNC_DONE 1
#define DTO_ASYNC_FAILED (-1)

/* Copy n bytes from src to dest. */
int dto_submit_memcpy(dto_async_op *op, void *dest, const void *src,
		      size_t n, unsigned int flags);

/* Fill n bytes of dest with the byte value c (like memset). */
int dto_submit_memset(dto_async_op *op, void *dest, int c, size_t n,
		      unsigned int flags);

/* Copy n bytes from src to dest and compute the CRC32C of the data.
 * The CRC value (read with dto_async_crc_val after DTO_ASYNC_DONE) is the
 * standard CRC32C (Castagnoli, seed 0xFFFFFFFF, final inversion), i.e. it
 * matches common software crc32c implementations. */
int dto_submit_memcpy_crc(dto_async_op *op, void *dest, const void *src,
			  size_t n, unsigned int flags);

/* Compute the CRC32C of n bytes at src without copying. */
int dto_submit_crc(dto_async_op *op, const void *src, size_t n,
		   unsigned int flags);

/* Poll a submitted operation. Returns DTO_ASYNC_PENDING, DTO_ASYNC_DONE or
 * DTO_ASYNC_FAILED. May be called repeatedly, from any thread; once an op
 * has left PENDING its result is stable. */
int dto_async_poll(dto_async_op *op);

/* CRC32C result of a completed dto_submit_memcpy_crc/dto_submit_crc.
 * Valid only after dto_async_poll returned DTO_ASYNC_DONE. */
uint32_t dto_async_crc_val(const dto_async_op *op);

/* ---- Asynchronous batch copy ----
 *
 * Submits up to DTO_BATCH_MAX copies as one DSA batch descriptor. The caller
 * owns the op (dto_batch_op_new / dto_batch_op_free) and may reuse it for
 * consecutive batches. A DSA batch requires at least two descriptors, so
 * count < 2 returns DTO_ASYNC_FALLBACK, as does count > DTO_BATCH_MAX or
 * count > the work queue's configured max_batch_size.
 *
 * dto_submit_batch_copy returns DTO_ASYNC_SUBMITTED or DTO_ASYNC_FALLBACK
 * (nothing submitted, nothing copied). dto_batch_poll returns
 * DTO_ASYNC_PENDING or DTO_ASYNC_DONE; copies the accelerator failed are
 * redone on the CPU before DONE is returned, so DONE means every copy is
 * complete. Once DONE, further polls return DONE without touching the
 * buffers again. */
#define DTO_BATCH_MAX 64

typedef struct dto_batch_op dto_batch_op;

dto_batch_op *dto_batch_op_new(void);
void dto_batch_op_free(dto_batch_op *op);
int dto_submit_batch_copy(dto_batch_op *op, void **dst, void **src,
			  size_t *sizes, int count, unsigned int flags);
int dto_batch_poll(dto_batch_op *op);

#ifdef __cplusplus
}
#endif

#endif /* DTO_H */
