#ifndef HH_REMINDER_TEST_API_H
#define HH_REMINDER_TEST_API_H
#include <stdint.h>
typedef void *MUTEX_HANDLE;
typedef void *QUEUE_HANDLE;
#define OPRT_OK 0
#define PR_NOTICE(...) ((void)0)
#define PR_ERR(...) ((void)0)
int tal_mutex_lock(MUTEX_HANDLE);
int tal_mutex_unlock(MUTEX_HANDLE);
#endif
