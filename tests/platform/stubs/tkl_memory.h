#ifndef HH_HOST_TKL_MEMORY_H
#define HH_HOST_TKL_MEMORY_H
#include <stddef.h>
#include <stdint.h>
void *tkl_system_psram_malloc(size_t size);
void tkl_system_psram_free(void *pointer);
int tkl_system_get_free_heap_size(void);
int32_t tkl_system_psram_get_free_heap_size(void);
#endif
