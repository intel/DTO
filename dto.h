
#ifndef DTO_H
#define DTO_H

#define DTO_API_AUTO_ADJUST_KNOBS 1
#define DTO_API_NO_AUTO_ADJUST_KNOBS 2
#define DTO_API_WAIT_BUSYPOLL 4
#define DTO_API_WAIT_UMWAIT 8
#define DTO_API_WAIT_TPAUSE 16
#define DTO_API_WAIT_YIELD 32
#define DTO_API_CACHE_CONTROL 64
#define DTO_API_NO_CACHE_CONTROL 128
#define DTO_API_NUMA_AWARE_BUFFER_CENTRIC 256
#define DTO_API_NUMA_AWARE_CPU_CENTRIC 512
#define DTO_API_NUMA_AWARE_DISABLED 1024
#define DTO_API_OVERLAPPING_MEMMOVE_ACTION_DSA 2048
#define DTO_API_OVERLAPPING_MEMMOVE_ACTION_CPU 4096

#define DTO_DSA_COMPLETE_OFFLOAD 8192
#define DTO_AUTO_SPLIT 16384

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void(*callback_t)(void*);

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

struct dto_call_cfg {
		unsigned char auto_adjust;
		unsigned char cache_control;
		enum wait_options wait_method;
		enum numa_aware numa_mode;
		enum overlapping_memmove_actions overlapping_action;
};

/**
 * dto_memcpy_default - Copy memory using DTO's default configuration.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to copy.
 */
void dto_memcpy_default(void *dest, const void *src, size_t n);

/**
 * dto_memcpy_cfg - Copy memory using a caller provided configuration.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to copy.
 * @cfg:  DTO configuration to use for this call.
 * @cb:   Optional callback invoked after completion.
 * @args: Argument passed to the callback.
 */
void dto_memcpy_cfg(void *dest, const void *src, size_t n,
                struct dto_call_cfg *cfg, callback_t cb, void* args);

/**
 * dto_memcpy - Copy memory using a configuration derived from @flags.
 * @dest:  Destination buffer.
 * @src:   Source buffer.
 * @n:     Number of bytes to copy.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 * @cb:    Optional callback invoked after completion.
 * @args:  Argument passed to the callback.
 */
void dto_memcpy(void *dest, const void *src, size_t n,
                int flags, callback_t cb, void* args);


/**
 * dto_memcpy_crc_default - Copy memory and compute CRC using DTO defaults.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to copy.
 *
 * Return: CRC32C value of the copied data.
 */
uint32_t dto_memcpy_crc_default(void *dest, const void *src, size_t n);

/**
 * dto_memcpy_crc_cfg - Copy memory and compute CRC using caller configuration.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to copy.
 * @cfg:  DTO configuration to use for this call.
 * @cb:   Optional callback invoked after submission.
 * @args: Argument passed to the callback.
 *
 * Return: CRC32C value of the copied data.
 */
uint32_t dto_memcpy_crc_cfg(void *dest, const void *src, size_t n,
                  struct dto_call_cfg *cfg, callback_t cb, void* args);

/**
 * dto_memcpy_crc - Copy memory and compute CRC using @flags derived
 *                   configuration.
 * @dest:  Destination buffer.
 * @src:   Source buffer.
 * @n:     Number of bytes to copy.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 * @cb:    Optional callback invoked after submission.
 * @args:  Argument passed to the callback.
 *
 * Return: CRC32C value of the copied data.
 */
uint32_t dto_memcpy_crc(void *dest, const void *src, size_t n,
                   int flags, callback_t cb, void* args);

/**
 * dto_memmove_default - Move memory using DTO's default configuration.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to move.
 */
void dto_memmove_default(void *dest, const void *src, size_t n);

/**
 * dto_memmove_cfg - Move memory using a caller provided configuration.
 * @dest: Destination buffer.
 * @src:  Source buffer.
 * @n:    Number of bytes to move.
 * @cfg:  DTO configuration to use for this call.
 * @cb:   Optional callback invoked after completion.
 * @args: Argument passed to the callback.
 */
void dto_memmove_cfg(void *dest, const void *src, size_t n,
                struct dto_call_cfg *cfg, callback_t cb, void* args);

/**
 * dto_memmove - Move memory using a configuration derived from @flags.
 * @dest:  Destination buffer.
 * @src:   Source buffer.
 * @n:     Number of bytes to move.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 * @cb:    Optional callback invoked after completion.
 * @args:  Argument passed to the callback.
 */
void dto_memmove(void *dest, const void *src, size_t n,
                int flags, callback_t cb, void* args);

/**
 * dto_memset_default - Set memory using DTO's default configuration.
 * @s: Destination buffer to fill.
 * @c: Byte value to set.
 * @n: Number of bytes to set.
 */
void dto_memset_default(void *s, int c, size_t n);

/**
 * dto_memset_cfg - Set memory using a caller provided configuration.
 * @s:   Destination buffer to fill.
 * @c:   Byte value to set.
 * @n:   Number of bytes to set.
 * @cfg: DTO configuration to use for this call.
 */
void dto_memset_cfg(void *s, int c, size_t n, struct dto_call_cfg *cfg);

/**
 * dto_memset - Set memory using a configuration derived from @flags.
 * @s:     Destination buffer to fill.
 * @c:     Byte value to set.
 * @n:     Number of bytes to set.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 */
void dto_memset(void *s, int c, size_t n, int flags);

/**
 * dto_memcmp_default - Compare memory using DTO's default configuration.
 * @s1: First buffer.
 * @s2: Second buffer.
 * @n:  Number of bytes to compare.
 *
 * Return: < 0, 0 or > 0 if s1 is found to be less than, equal to or greater
 * than s2 respectively.
 */
int dto_memcmp_default(const void *s1, const void *s2, size_t n);

/**
 * dto_memcmp_cfg - Compare memory using a caller provided configuration.
 * @s1:  First buffer.
 * @s2:  Second buffer.
 * @n:   Number of bytes to compare.
 * @cfg: DTO configuration to use for this call.
 *
 * Return: Comparison result as in memcmp().
 */
int dto_memcmp_cfg(const void *s1, const void *s2, size_t n,
                struct dto_call_cfg *cfg);

/**
 * dto_memcmp - Compare memory using a configuration derived from @flags.
 * @s1:    First buffer.
 * @s2:    Second buffer.
 * @n:     Number of bytes to compare.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 *
 * Return: Comparison result as in memcmp().
 */
int dto_memcmp(const void *s1, const void *s2, size_t n, int flags);

/**
 * dto_crc_default - Compute CRC using DTO's default configuration.
 * @src: Buffer to checksum.
 * @n:   Number of bytes to process.
 *
 * Return: CRC32C value of the buffer.
 */
uint32_t dto_crc_default(const void *src, size_t n);

/**
 * dto_crc_cfg - Compute CRC using a caller provided configuration.
 * @src:  Buffer to checksum.
 * @n:    Number of bytes to process.
 * @cfg:  DTO configuration to use for this call.
 * @cb:   Optional callback invoked after submission.
 * @args: Argument passed to the callback.
 *
 * Return: CRC32C value of the buffer.
 */
uint32_t dto_crc_cfg(const void *src, size_t n, struct dto_call_cfg *cfg,
                callback_t cb, void* args);

/**
 * dto_crc - Compute CRC using a configuration derived from @flags.
 * @src:   Buffer to checksum.
 * @n:     Number of bytes to process.
 * @flags: Bitwise OR of DTO_API_* values that override defaults.
 * @cb:    Optional callback invoked after submission.
 * @args:  Argument passed to the callback.
 *
 * Return: CRC32C value of the buffer.
 */
uint32_t dto_crc(const void *src, size_t n, int flags, callback_t cb,
                void* args);

#ifdef __cplusplus
}
#endif

#endif

