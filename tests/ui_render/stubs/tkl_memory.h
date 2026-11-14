#ifndef HH_RENDER_TKL_MEMORY_H
#define HH_RENDER_TKL_MEMORY_H
#include <stddef.h>
void *tkl_system_malloc(size_t size);
void tkl_system_free(void *pointer);
void *tkl_system_realloc(void *pointer, size_t size);
void *tkl_system_psram_malloc(size_t size);
void tkl_system_psram_free(void *pointer);
void *tkl_system_psram_realloc(void *pointer, size_t size);
#endif
