#ifndef HH_SUPABASE_TEST_TIME_H
#define HH_SUPABASE_TEST_TIME_H
typedef struct{int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday;}POSIX_TM_S;
int tal_time_check_time_sync(void);
int tal_time_get(POSIX_TM_S*);
#endif
