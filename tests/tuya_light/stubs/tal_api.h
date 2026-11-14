#ifndef HH_LIGHT_FAKE_TAL_H
#define HH_LIGHT_FAKE_TAL_H
#include <stdbool.h>
#include <stdint.h>
typedef void *MUTEX_HANDLE;
#define OPRT_OK 0
#define PR_NOTICE(...) ((void)0)
int tal_mutex_create_init(MUTEX_HANDLE *mutex);
int tal_mutex_lock(MUTEX_HANDLE mutex);
int tal_mutex_unlock(MUTEX_HANDLE mutex);
uint64_t tal_system_get_millisecond(void);
#endif
