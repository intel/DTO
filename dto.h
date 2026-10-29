/*******************************************************************************
 * Copyright (C) 2026 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 * dto.h - Public asynchronous offload API for libdto.
 *
 * In addition to transparently accelerating memcpy/memmove/memset/memcmp via
 * LD_PRELOAD, libdto exposes an explicit submit/poll API for applications
 * that want to overlap accelerator work with CPU work. libdto_explicit is
 * the same library without the libc interposers, for programs that only call
 * this API.
 *
 * Lifecycle:
 *
 *   dto_async_op op;
 *   if (dto_submit_memcpy(&op, dst, src, n, DTO_SUBMIT_CC) == DTO_ASYNC_SUBMITTED) {
 *       ... other CPU work ...
 *       dto_async_wait(&op);              // optional; or poll in your own loop
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
 * maximum transfer size). Nothing is done in that case: the caller performs
 * the operation on the CPU. Unlike the transparent path, explicit submits are
 * not subject to the size heuristics (DTO_MIN_BYTES / auto-tuning): callers
 * decide what to offload.
 *
 * On DTO_ASYNC_FAILED the device did not complete the operation;
 * dto_async_status() gives its status code. Redo the operation on the CPU.
 * After a page fault (status 0x03, see DTO_SUBMIT_BOF) the device has done
 * dto_async_bytes_completed() bytes; for a move whose buffers overlap those
 * bytes have already overwritten part of the source, so finish the move from
 * where the device stopped rather than redoing it: the first bytes were done
 * when bit 0 of dto_async_result() is 0 (ascending copy), the last when it is
 * 1 (descending).
 *
 * Flags (every submit takes a combination of these):
 *
 * DTO_SUBMIT_CC directs the operation's output toward the CPU cache
 * (IDXD_OP_FLAG_CC) when the device supports it, for destinations that will
 * be read again soon. Ignored for operations without a destination (CRC
 * generation, compare, translation fetch).
 *
 * DTO_SUBMIT_BOF makes the device block on a page fault until the kernel has
 * resolved it, so the operation completes even if its buffers are not
 * faulted in. The device only accepts it on a work queue configured with
 * block-on-fault ("accel-config config-wq --block-on-fault=1"); on a queue
 * without it the flag is dropped, so the submission is still accepted but a
 * page fault stops the operation partway, as if the flag had not been set:
 *
 *   WQ block-on-fault  DTO_SUBMIT_BOF  page fault ->
 *   enabled            set             the device waits; the op completes
 *   disabled           set             DTO_ASYNC_FAILED, status 0x03: partial
 *   either             not set         completion, see above
 *
 * The flag is thus "block on faults when the work queue allows it", and the
 * caller must handle a partial completion either way. The same applies to
 * each operation of a batch, which dto_batch_poll then redoes on the CPU.
 *
 * CRC values: dto_async_crc_val() and dto_batch_crc() return the raw CRC32C
 * of the data -- the SSE4.2 crc32 instruction applied from 0, with no final
 * inversion. Standard CRC32C (init 0xFFFFFFFF, final inversion) and chained
 * checksums are the seeded "crc32c" calls below, in the iSCSI presentation:
 * seed 0 gives the standard CRC32C, and passing one call's result as the next
 * call's seed continues the checksum across buffers.
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
#define DTO_SUBMIT_CC  (1u << 0)	/* see above */
#define DTO_SUBMIT_BOF (1u << 1)	/* block on page faults (see above) */

/* Submit results */
#define DTO_ASYNC_SUBMITTED 0
#define DTO_ASYNC_FALLBACK (-1)

/* Poll results */
#define DTO_ASYNC_PENDING 0
#define DTO_ASYNC_DONE 1
#define DTO_ASYNC_FAILED (-1)

/* ---- Data movement ---- */

/* Copy n bytes from src to dest. Overlapping buffers are handled like
 * memmove. */
int dto_submit_memcpy(dto_async_op *op, void *dest, const void *src,
		      size_t n, unsigned int flags);

/* Fill n bytes of dest with the byte value c (like memset). */
int dto_submit_memset(dto_async_op *op, void *dest, int c, size_t n,
		      unsigned int flags);

/* Fill n bytes of dest with the 8-byte pattern, repeated (the low byte of
 * pattern lands at dest[0]). */
int dto_submit_memfill(dto_async_op *op, void *dest, uint64_t pattern,
		       size_t n, unsigned int flags);

/* ---- CRC32C ---- */

/* Copy n bytes from src to dest and compute the raw CRC32C of the data
 * (read it with dto_async_crc_val). */
int dto_submit_memcpy_crc(dto_async_op *op, void *dest, const void *src,
			  size_t n, unsigned int flags);

/* Raw CRC32C of n bytes at src, without copying (dto_async_crc_val). */
int dto_submit_crc(dto_async_op *op, const void *src, size_t n,
		   unsigned int flags);

/* Seeded CRC32C in the iSCSI presentation (see above), with and without a
 * copy. Read the value with dto_async_crc32c_val. */
int dto_submit_memcpy_crc32c(dto_async_op *op, void *dest, const void *src,
			     size_t n, uint32_t seed, unsigned int flags);
int dto_submit_crc32c(dto_async_op *op, const void *src, size_t n,
		      uint32_t seed, unsigned int flags);

/* Synchronous seeded CRC32C, same presentation as dto_submit_crc32c. Always
 * returns the correct value: buffers smaller than DTO_CRC_MIN_BYTES (by
 * default DTO_MIN_BYTES) are checksummed on the CPU, as is anything the
 * device cannot do. */
uint32_t dto_crc32c_with_seed(uint32_t seed, const void *src, size_t n);

/* ---- Compare and translation fetch ---- */

/* Compare n bytes at src1 with src2. Once DONE, dto_async_result() is 0
 * when they are equal and 1 when they differ, and dto_async_bytes_completed()
 * is then the offset of the first difference. */
int dto_submit_compare(dto_async_op *op, const void *src1, const void *src2,
		       size_t n, unsigned int flags);

/* Compare n bytes at src with the 8-byte pattern, repeated; results as for
 * dto_submit_compare. With the buffer's own first 8 bytes as the pattern it
 * tells whether the buffer is constant-filled (all zero when they are 0). */
int dto_submit_compval(dto_async_op *op, const void *src, uint64_t pattern,
		       size_t n, unsigned int flags);

/* Translation fetch (DSA 2.0): warm the device's address translations for
 * the n bytes at addr without moving data. Devices without the operation
 * complete with DTO_ASYNC_FAILED and dto_async_status() == 0x10. */
int dto_submit_transl_fetch(dto_async_op *op, const void *addr, size_t n,
			    unsigned int flags);

/* ---- Completion ---- */

/* Poll a submitted operation. Returns DTO_ASYNC_PENDING, DTO_ASYNC_DONE or
 * DTO_ASYNC_FAILED. May be called repeatedly, from any thread; once an op
 * has left PENDING its result is stable. */
int dto_async_poll(dto_async_op *op);

/* Wait until the device has finished with a submitted op, using the wait
 * method DTO_WAIT_METHOD selects. It only waits: call dto_async_poll
 * afterwards for the result. A no-op for an op that was never submitted or
 * whose submit returned DTO_ASYNC_FALLBACK, so it is always safe to call
 * before a poll loop. There is deliberately no timeout: until the device
 * finishes it still owns the buffers, so the only safe thing to do after a
 * timeout would be to wait again. */
void dto_async_wait(dto_async_op *op);

/* Nonzero when the configured wait method gives the core back to the
 * scheduler while waiting, rather than spinning or pausing on it. For callers
 * with their own wait loop deciding whether to delegate to dto_async_wait.
 * None of the current wait methods does, so this returns 0. */
int dto_wait_blocks(void);

/* Results of a completed (DTO_ASYNC_DONE) operation. */
uint32_t dto_async_crc_val(const dto_async_op *op);	/* raw CRC32C */
uint32_t dto_async_crc32c_val(const dto_async_op *op);	/* seeded (iSCSI) */
int dto_async_result(const dto_async_op *op);		/* compare: 0 equal, 1 differ */
uint32_t dto_async_bytes_completed(const dto_async_op *op);
/* The device's status code (1 = success, 0 = still pending). */
int dto_async_status(const dto_async_op *op);

/* ---- Batches ----
 *
 * Up to `capacity` (at most DTO_BATCH_MAX) operations sent to the device as
 * one batch descriptor: one submission and one completion to poll instead of
 * one per operation. Add operations, submit, then poll; dto_batch_reset makes
 * the batch reusable. A batch with a single operation is sent as a plain
 * descriptor; an empty one completes immediately. The operations of a batch
 * may run in any order and concurrently, so none may depend on another.
 *
 * dto_batch_add_* return the operation's index, or -1 when the batch is full,
 * already submitted, or n is 0 (dualcast also requires bits 11:0 of the two
 * destinations to be equal, a device rule). dto_batch_submit returns
 * DTO_ASYNC_SUBMITTED, or DTO_ASYNC_FALLBACK with nothing submitted (the
 * caller does the operations on the CPU), including when the batch is larger
 * than the work queue's max_batch_size or an operation exceeds its maximum
 * transfer size.
 *
 * dto_batch_poll returns DTO_ASYNC_PENDING, then DTO_ASYNC_DONE once every
 * operation is complete: operations the device did not finish are redone on
 * the CPU before DONE is returned, results included, so after DONE every
 * result and CRC is valid. dto_batch_status(b, i) is the device's status for
 * operation i (1 when the device did it). A poll of a batch that was never
 * submitted, or whose submit fell back, returns DTO_ASYNC_FAILED.
 */
#define DTO_BATCH_MAX 1024

typedef struct dto_batch dto_batch;

/* capacity is clamped to DTO_BATCH_MAX. NULL when out of memory. */
dto_batch *dto_batch_create(int capacity);
/* Both wait for a batch that is still in flight. */
void dto_batch_destroy(dto_batch *b);
void dto_batch_reset(dto_batch *b);
int dto_batch_count(const dto_batch *b);

int dto_batch_add_memmove(dto_batch *b, void *dst, const void *src, size_t n,
			  unsigned int flags);
int dto_batch_add_memfill(dto_batch *b, void *dst, uint64_t pattern, size_t n,
			  unsigned int flags);
/* Copy src to both dst1 and dst2. */
int dto_batch_add_dualcast(dto_batch *b, void *dst1, void *dst2,
			   const void *src, size_t n, unsigned int flags);
/* Raw CRC32C, read with dto_batch_crc. */
int dto_batch_add_crc(dto_batch *b, const void *src, size_t n,
		      unsigned int flags);
int dto_batch_add_memcpy_crc(dto_batch *b, void *dst, const void *src,
			     size_t n, unsigned int flags);
/* Results as for dto_submit_compare / dto_submit_compval. */
int dto_batch_add_compare(dto_batch *b, const void *src1, const void *src2,
			  size_t n, unsigned int flags);
int dto_batch_add_compval(dto_batch *b, const void *src, uint64_t pattern,
			  size_t n, unsigned int flags);

int dto_batch_submit(dto_batch *b);
int dto_batch_poll(dto_batch *b);
/* Same contract as dto_async_wait: waits only, poll afterwards. */
void dto_batch_wait(dto_batch *b);

/* Results of operation i, valid after dto_batch_poll returned DONE. */
int dto_batch_status(const dto_batch *b, int i);
int dto_batch_result(const dto_batch *b, int i);
uint32_t dto_batch_bytes_completed(const dto_batch *b, int i);
uint32_t dto_batch_crc(const dto_batch *b, int i);

/* Convenience: reset b and submit count copies (dst[i] <- src[i], sizes[i]
 * bytes) as one batch. Returns as dto_batch_submit; DTO_ASYNC_FALLBACK also
 * when count is not between 1 and the batch's capacity or a size is 0. */
int dto_submit_batch_copy(dto_batch *b, void **dst, void **src,
			  size_t *sizes, int count, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif /* DTO_H */
