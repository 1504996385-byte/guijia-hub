#ifndef HTTP_TEST_CONFIG_H
#define HTTP_TEST_CONFIG_H
#include "tal_api.h"
#include "tal_log.h"
#define HTTP_MAX_RESPONSE_HEADERS_SIZE_BYTES 4096U
#define HTTP_MAX_RESPONSE_CHUNK_SIZE_BYTES (16U*1024U)
#define HTTP_MAX_RESPONSE_CHUNK_ONCE_BYTES 2048U
#define HTTP_MALLOC tal_psram_malloc
#define HTTP_FREE tal_psram_free
#define HTTP_USER_AGENT_VALUE "HomeHub"
#endif
