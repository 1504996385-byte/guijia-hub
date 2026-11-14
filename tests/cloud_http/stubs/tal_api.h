#include "tuya_cloud_types.h"
uint64_t tal_system_get_millisecond(void);
void tal_system_sleep(uint32_t ms);
void *tal_psram_malloc(size_t size);
void tal_psram_free(void *pointer);
