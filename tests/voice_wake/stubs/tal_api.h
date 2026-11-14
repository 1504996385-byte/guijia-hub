#ifndef HH_VOICE_FAKE_API_H
#define HH_VOICE_FAKE_API_H
#include "tuya_cloud_types.h"
typedef void *MUTEX_HANDLE;
int tal_mutex_create_init(MUTEX_HANDLE *mutex);
int tal_mutex_lock(MUTEX_HANDLE mutex);
int tal_mutex_unlock(MUTEX_HANDLE mutex);
uint64_t tal_system_get_millisecond(void);
void voice_fake_notice(const char *format,...);
#define PR_NOTICE(...) voice_fake_notice(__VA_ARGS__)
#endif
