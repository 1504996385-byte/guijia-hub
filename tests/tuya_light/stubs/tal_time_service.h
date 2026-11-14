#ifndef HH_LIGHT_FAKE_TIME_H
#define HH_LIGHT_FAKE_TIME_H
#include <stdint.h>
typedef uint32_t TIME_T;
TIME_T tal_time_get_posix(void);
int tal_time_check_time_sync(void);
#endif
