#ifndef HH_EVENT_TEST_QUEUE_H
#define HH_EVENT_TEST_QUEUE_H
#include <stdint.h>
typedef void *QUEUE_HANDLE;
typedef int OPERATE_RET;
#define OPRT_OK 0
#define QUEUE_WAIT_FOREVER UINT32_MAX
int tal_queue_create_init(QUEUE_HANDLE*,int,int);
int tal_queue_post(QUEUE_HANDLE,void*,uint32_t);
int tal_queue_fetch(QUEUE_HANDLE,void*,uint32_t);
void tal_queue_free(QUEUE_HANDLE);
#endif
