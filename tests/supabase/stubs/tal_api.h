#ifndef HH_SUPABASE_TEST_API_H
#define HH_SUPABASE_TEST_API_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int OPERATE_RET;
typedef void *MUTEX_HANDLE;
typedef void *THREAD_HANDLE;
typedef struct{unsigned stackDepth,priority;const char *thrdname;unsigned psram_mode;}THREAD_CFG_T;
#define OPRT_OK 0
#define THREAD_PRIO_1 1
#define PR_NOTICE(...) ((void)0)
#define PR_ERR(...) ((void)0)
int tal_mutex_create_init(MUTEX_HANDLE*);
int tal_mutex_lock(MUTEX_HANDLE);
int tal_mutex_unlock(MUTEX_HANDLE);
int tal_thread_create_and_start(THREAD_HANDLE*,void*,void*,void(*)(void*),void*,THREAD_CFG_T*);
uint64_t tal_system_get_millisecond(void);
void tal_system_sleep(unsigned);
#endif
