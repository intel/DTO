
#ifndef DTO_H
#define DTO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void(*callback_t)(void*);

void dto_memcpy_async(void *dest, const void *src, size_t n, callback_t cb, void* args);

#ifdef __cplusplus
}
#endif

#endif

