#ifndef HH_FAKE_SDK_H
#define HH_FAKE_SDK_H
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "../../../firmware/core/homehub_core.h"
#include "../../../firmware/core/homehub_emotion.h"
#include "cJSON.h"
#define HOMEHUB_PLATFORM_H
#define OPRT_OK 0
#define OPRT_COM_ERROR (-1)
#define OPRT_INVALID_PARM (-2)
#define SUBSCRIBE_TYPE_NORMAL 0
#define THREAD_PRIO_1 1
#define TUYA_UART_NUM_0 0
#define ENABLE_LIBLWIP 1
#define PROJECT_VERSION "test"
#define PR_ERR(...) ((void)0)
#define PR_WARN(...) ((void)0)
#define PR_NOTICE(...) hh_fake_notice(__VA_ARGS__)
void hh_fake_notice(const char *format,...);
typedef int OPERATE_RET;
typedef char CHAR_T;
typedef unsigned UINT_T;
typedef void VOID;
typedef int BOOL_T;
typedef uint16_t USHORT_T;
typedef void *MUTEX_HANDLE;
typedef void *THREAD_HANDLE;
typedef struct {unsigned stackDepth,priority;char *thrdname;unsigned psram_mode;} THREAD_CFG_T;
typedef struct {const char *seed,*key;} tal_kv_cfg_t;
typedef struct {char wifi_ssid[33],wifi_password[65],server_url[192],device_token[129],tuya_product_id[33];bool chat_enabled;} HH_DeviceConfig;
typedef enum {EV_CHAT_REPORT,EV_CHAT_TEXT,EV_CHAT_EMOTION} HH_EventType;
typedef struct {HH_EventType type;HH_Action action;HH_ChatEvent chat_event;bool ok,chat_is_user_text;uint8_t emotion;char text[192];} HH_Event;
extern HH_DeviceConfig hh_device;
extern uint32_t hh_boot_id;
void hh_post(const HH_Event *event);
extern void *hh_events;
int tal_queue_post(void *queue, void *message, unsigned wait_ms);
int tal_mutex_create_init(MUTEX_HANDLE *m);
int tal_mutex_lock(MUTEX_HANDLE m);
int tal_mutex_unlock(MUTEX_HANDLE m);
int tal_kv_init(const tal_kv_cfg_t *cfg);
int tal_cli_init_with_uart(uint8_t uart_num);
int tal_cli_init(void);
int tuya_authorize_init(void);
int tal_sw_timer_init(void);
int tal_workq_init(void);
#define WORKQ_SYSTEM 0
int tal_workq_schedule(int service,void(*callback)(void*),void *data);
int tal_time_service_init(void);
void TUYA_LwIP_Init(void);
void tuya_tls_init(void);
void tuya_register_center_init(void);
uint64_t tal_system_get_millisecond(void);
void tal_system_sleep(unsigned ms);
int tal_thread_create_and_start(THREAD_HANDLE *t,void *a,void*b,void(*fn)(void*),void*arg,THREAD_CFG_T *cfg);
void *tkl_system_psram_malloc(size_t size);
void tkl_system_psram_free(void *memory);
typedef int (*FAKE_EVENT_CB)(void *data);
int tal_event_subscribe(const char *event,const char *name,FAKE_EVENT_CB cb,int type);
#define EVENT_AI_CLIENT_RUN "ai.run"
#define EVENT_AI_SESSION_CLOSE "ai.close"
#define EVENT_AI_PLAYER_STATE "player.state"
typedef struct {const char *uuid,*authkey;} tuya_iot_license_t;
int tuya_authorize_read(tuya_iot_license_t *license);
typedef struct {int is_activated;} tuya_iot_client_t;
typedef struct {int id;} tuya_event_msg_t;
typedef struct {const char *software_ver,*productkey,*uuid,*authkey;void(*event_handler)(tuya_iot_client_t*,tuya_event_msg_t*);bool(*network_check)(void);} tuya_iot_config_t;
#define TUYA_EVENT_MQTT_CONNECTED 1
#define TUYA_EVENT_MQTT_DISCONNECT 2
#define TUYA_EVENT_BIND_START 3
#define TUYA_EVENT_BIND_TOKEN_ON 4
int tuya_iot_init(tuya_iot_client_t*,const tuya_iot_config_t*);
int tuya_iot_start(tuya_iot_client_t*);
int tuya_iot_yield(tuya_iot_client_t*);
#define NETCONN_WIFI 1
#define NETCONN_AUTO 0
#define NETCONN_CMD_STATUS 0
#define NETCONN_CMD_SSID_PSWD 1
#define NETCONN_CMD_NETCFG 2
#define NETCONN_CMD_CLOSE 3
#define NETCFG_TUYA_BLE 2
#define NETCFG_TUYA_WIFI_AP 1
typedef enum {NETMGR_LINK_DOWN,NETMGR_LINK_UP,NETMGR_LINK_UP_SWITH} netmgr_status_e;
typedef struct {char ssid[33],pswd[65];} netconn_wifi_info_t;
typedef struct {int type;} netcfg_args_t;
int netmgr_init(int type);
int netmgr_conn_set(int type,int cmd,void *info);
int netmgr_conn_get(int type,int cmd,void *state);
typedef cJSON ty_cJSON;
ty_cJSON *hh_test_json_get(const ty_cJSON*,const char*);
#define ty_cJSON_GetObjectItem hh_test_json_get
#define ty_cJSON_ParseWithLengthOpts cJSON_ParseWithLengthOpts
#define ty_cJSON_Delete cJSON_Delete
#define ty_cJSON_IsString cJSON_IsString
#define ty_cJSON_GetArrayItem cJSON_GetArrayItem
#define ty_cJSON_GetArraySize cJSON_GetArraySize
typedef unsigned short AI_PACKET_PT;
typedef unsigned short AI_EVENT_TYPE;
typedef char *AI_EVENT_ID;
#define AI_UUID_V4_LEN 36
#define AI_BIZ_MAX_NUM 5
#define AI_PT_TEXT 1
#define AI_PT_AUDIO 2
#define AI_EVENT_START 0
#define AI_EVENT_END 2
#define AI_EVENT_CHAT_BREAK 4
#define AI_EVENT_CHAT_EXIT 0xf000
#define AI_HAS_ATTR 1
#define AUDIO_CODEC_PCM 101
#define AUDIO_CODEC_MP3 104
#define AI_STREAM_START 1
#define AI_STREAM_ING 2
#define AI_STREAM_END 3
#define AI_STREAM_ONE 4
#define AI_BIZ_DATA_TYPE_BYTE 0
#define AI_BIZ_DATA_TYPE_JSON 1
typedef struct {int codec_type;unsigned sample_rate,channels,bit_depth;} AI_AUDIO_ATTR_BASE_T;
typedef struct {int type,flag;union {struct {AI_AUDIO_ATTR_BASE_T base;struct {char *session_id_list;} option;} audio;} value;} AI_BIZ_ATTR_INFO_T;
typedef struct {int stream_flag,data_type;unsigned len,total_len;} AI_BIZ_HEAD_INFO_T;
typedef union {struct {uint64_t timestamp,pts;} audio;} AI_BIZ_HD_T;
typedef struct {char session_id[37],event_id[37];} AI_EVENT_ATTR_T;
typedef struct {AI_EVENT_TYPE type;} AI_EVENT_HEAD_T;
typedef int(*AI_BIZ_RECV_CB)(AI_BIZ_ATTR_INFO_T*,AI_BIZ_HEAD_INFO_T*,void*,void*);
typedef struct {char scode[64];struct {AI_PACKET_PT type;AI_BIZ_RECV_CB cb;} arc[AI_BIZ_MAX_NUM];int(*event_cb)(AI_EVENT_ATTR_T*,AI_EVENT_HEAD_T*,void*);} AI_AGENT_RECV_DATA_T;
typedef struct {char sid[37],eid[37];} AI_AGENT_SESSION_T;
typedef struct {char scode[64];struct {AI_AUDIO_ATTR_BASE_T audio;} attr;bool codec_enable,enable_crt_session_ext;struct {const char *format;unsigned sample_rate,bit_rate;} tts_cfg;} AI_AGENT_CFG_T;
int tuya_ai_agent_init(AI_AGENT_CFG_T*);
int tuya_ai_agent_set_recv_cb(AI_AGENT_RECV_DATA_T*);
int tuya_ai_agent_crt_session(char*,unsigned,uint64_t,void*,unsigned);
int tuya_ai_agent_del_session(char*);
AI_AGENT_SESSION_T *tuya_ai_agent_get_session(char*);
void tuya_ai_agent_server_vad_ctrl(bool);
int tuya_ai_agent_set_scode(char*);
void tuya_ai_agent_set_eid(char*);
bool tuya_ai_agent_is_ready(void);
int tuya_ai_agent_event(int,int);
int tuya_ai_agent_start(char*);
int tuya_ai_agent_upload_stream(char*,AI_PACKET_PT,AI_BIZ_HD_T*,char*,unsigned,unsigned);
void tuya_ai_input_start(bool);
void tuya_ai_input_stop(void);
bool tuya_ai_input_is_started(void);
int tuya_ai_audio_input(uint64_t,uint64_t,uint8_t*,unsigned,unsigned);
typedef void *AI_PLAYER_HANDLE;
typedef void *PLAYER_CONSUMER_HANDLE;
typedef enum {AI_PLAYER_STOPPED,AI_PLAYER_PLAYING,AI_PLAYER_PAUSED} AI_PLAYER_STATE_T;
typedef struct {AI_PLAYER_HANDLE handle;AI_PLAYER_STATE_T state;} AI_PLAYER_EVT_T;
typedef struct {int sample,datebits,channel;} AI_PLAYER_CFG_T;
typedef struct {int(*open)(PLAYER_CONSUMER_HANDLE*);int(*close)(PLAYER_CONSUMER_HANDLE);int(*start)(PLAYER_CONSUMER_HANDLE);int(*write)(PLAYER_CONSUMER_HANDLE,const void*,uint32_t);int(*stop)(PLAYER_CONSUMER_HANDLE);int(*set_volume)(PLAYER_CONSUMER_HANDLE,uint32_t);} AI_PLAYER_CONSUMER_T;
#define AI_PLAYER_MODE_FOREGROUND 0
#define AI_PLAYER_SRC_MEM 0
#define AI_AUDIO_CODEC_MP3 0
int tuya_ai_player_service_init(AI_PLAYER_CFG_T*);
int tuya_ai_player_set_consumer(AI_PLAYER_CONSUMER_T*);
int tuya_ai_player_create(int,AI_PLAYER_HANDLE*);
int tuya_ai_player_set_volume(AI_PLAYER_HANDLE,int);
int tuya_ai_player_start(AI_PLAYER_HANDLE,int,char*,int);
int tuya_ai_player_feed(AI_PLAYER_HANDLE,uint8_t*,uint32_t);
int tuya_ai_player_stop(AI_PLAYER_HANDLE);
AI_PLAYER_STATE_T tuya_ai_player_get_state(AI_PLAYER_HANDLE);
#endif
