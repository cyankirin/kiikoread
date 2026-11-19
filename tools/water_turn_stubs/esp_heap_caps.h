#pragma once
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int water_test_alloc_fail;
// mingw 严格 C11 下不声明 aligned_alloc；Windows 用 CRT 对（_aligned_malloc/_aligned_free），
// Linux CI 仍走 C11 aligned_alloc/free，行为零差异。
// mingw strict C11 does not declare aligned_alloc; Windows pairs the CRT
// (_aligned_malloc/_aligned_free) while Linux CI keeps aligned_alloc/free unchanged.
static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    (void)caps;
    if (water_test_alloc_fail) return NULL;
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    return aligned_alloc(alignment, size);
#endif
}
static inline void heap_caps_free(void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
