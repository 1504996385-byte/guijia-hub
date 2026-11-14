#ifndef HH_NET_START_FAKE_H
#define HH_NET_START_FAKE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../firmware/core/homehub_core.h"
#define HOMEHUB_PLATFORM_H
#define HH_ROOT "/sdcard/homehub"
#define HH_WAV_MAX (960000u+44u)
#define OPRT_OK 0
#define THREAD_PRIO_1 1
#define PR_ERR(...) ((void)0)
#define PR_NOTICE(...) ((void)0)
#define HTTP_CLIENT_SUCCESS 0
#define HTTP_FREE free
typedef void *QUEUE_HANDLE;
typedef void *MUTEX_HANDLE;
typedef void *THREAD_HANDLE;
typedef void *TUYA_DIR;
typedef void *TUYA_FILEINFO;
typedef struct {unsigned stackDepth,priority,psram_mode;const char *thrdname;} THREAD_CFG_T;
typedef struct {
    char wifi_ssid[33],wifi_password[65],server_url[192],device_token[129],tuya_product_id[33];
    bool chat_enabled,pir_enabled;uint8_t volume;
} HH_DeviceConfig;
typedef enum {EV_REMOTE_MESSAGE,EV_NETWORK_INFO} HH_EventType;
typedef struct {HH_EventType type;HH_Action action;HH_Message message;bool ok;char text[192];} HH_Event;
extern HH_DeviceConfig hh_device;
static MUTEX_HANDLE hh_fs_mutex;
static uint32_t hh_boot_id;
static unsigned net_test_queue_creates,net_test_thread_creates;
static unsigned net_test_http_requests;
static char net_test_http_path[256];
static int tal_queue_create_init(QUEUE_HANDLE *q,size_t n,unsigned count){(void)n;(void)count;net_test_queue_creates++;*q=(void*)1;return 0;}
static int tal_queue_post(QUEUE_HANDLE q,void *p,unsigned wait){(void)q;(void)p;(void)wait;return 0;}
static int tal_queue_fetch(QUEUE_HANDLE q,void *p,unsigned wait){(void)q;(void)p;(void)wait;return -1;}
static int tal_thread_create_and_start(THREAD_HANDLE *t,void *a,void *b,void(*fn)(void*),void *arg,THREAD_CFG_T *cfg){(void)a;(void)b;(void)fn;(void)arg;(void)cfg;net_test_thread_creates++;*t=(void*)2;return 0;}
static int tal_mutex_lock(MUTEX_HANDLE m){(void)m;return 0;}
static int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;return 0;}
static uint32_t tal_system_get_millisecond(void){return 0;}
static void tal_system_sleep(unsigned n){(void)n;}
static int tkl_dir_open(const char *p,TUYA_DIR *d){(void)p;(void)d;return -1;}
static int tkl_dir_read(TUYA_DIR d,TUYA_FILEINFO *i){(void)d;(void)i;return -1;}
static int tkl_dir_name(TUYA_FILEINFO i,const char **n){(void)i;(void)n;return -1;}
static int tkl_dir_close(TUYA_DIR d){(void)d;return 0;}
static int tkl_fs_remove(const char *p){(void)p;return 0;}
static void tkl_system_psram_free(void *p){free(p);}
static bool hh_file_read(const char *p,uint8_t **d,uint32_t *n,uint32_t max){(void)p;(void)d;(void)n;(void)max;return false;}
static bool hh_file_write_atomic(const char *p,const uint8_t *d,uint32_t n){(void)p;(void)d;(void)n;return false;}
static bool hh_valid_id(const char *p){return p&&p[0];}
static bool hh_wav_validate(const uint8_t *p,uint32_t n,uint32_t *pcm){(void)p;(void)n;(void)pcm;return false;}
static void hh_post(const HH_Event *e){(void)e;}
bool hh_chat_network_ready(void){return false;}
typedef struct {const char *key,*value;} http_client_header_t;
typedef struct {
    const char *host,*path,*method;uint16_t port;const uint8_t *cacert,*body;
    size_t cacert_len,headers_count,body_length;bool tls_no_verify;
    http_client_header_t *headers;unsigned timeout_ms;
} http_client_request_t;
typedef struct {void *buffer;const uint8_t *body;size_t body_length;int status_code;} http_client_response_t;
static int http_client_request(http_client_request_t *q,http_client_response_t *r){
    (void)r;net_test_http_requests++;snprintf(net_test_http_path,sizeof(net_test_http_path),"%s",q->path);return -1;
}
typedef struct cJSON {char *valuestring;int valueint;double valuedouble;struct cJSON *child,*next;} cJSON;
static cJSON *cJSON_CreateObject(void){return NULL;}
static cJSON *cJSON_Parse(const char *s){(void)s;return NULL;}
static cJSON *cJSON_ParseWithLength(const char *s,size_t n){(void)s;(void)n;return NULL;}
static cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *j,const char *key){(void)j;(void)key;return NULL;}
static int cJSON_IsString(const cJSON *j){(void)j;return 0;}
static int cJSON_IsNumber(const cJSON *j){(void)j;return 0;}
static int cJSON_IsTrue(const cJSON *j){(void)j;return 0;}
static int cJSON_IsArray(const cJSON *j){(void)j;return 0;}
static void cJSON_Delete(cJSON *j){(void)j;}
static void cJSON_free(void *p){free(p);}
static char *cJSON_PrintUnformatted(cJSON *j){(void)j;return NULL;}
static void cJSON_AddStringToObject(cJSON *j,const char *k,const char *s){(void)j;(void)k;(void)s;}
static void cJSON_AddNumberToObject(cJSON *j,const char *k,int n){(void)j;(void)k;(void)n;}
static void cJSON_AddItemToObject(cJSON *j,const char *k,cJSON *v){(void)j;(void)k;(void)v;}
#define cJSON_ArrayForEach(item,array) for((item)=(array)?(array)->child:NULL;(item);(item)=(item)->next)
static int mbedtls_sha256(const uint8_t *p,size_t n,uint8_t out[32],int mode){(void)p;(void)n;(void)mode;memset(out,0,32);return 0;}
#endif
