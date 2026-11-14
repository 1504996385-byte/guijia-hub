#ifndef HH_RENDER_TAL_API_H
#define HH_RENDER_TAL_API_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef void *QUEUE_HANDLE;
typedef void *MUTEX_HANDLE;
typedef int OPERATE_RET;
#define OPRT_OK 0
#define OPRT_COM_ERROR (-1)
#define PR_NOTICE(...) do { printf(__VA_ARGS__); putchar('\n'); } while(0)
uint32_t tal_system_get_millisecond(void);
OPERATE_RET tal_queue_post(QUEUE_HANDLE queue, void *message, uint32_t timeout_ms);
#endif
