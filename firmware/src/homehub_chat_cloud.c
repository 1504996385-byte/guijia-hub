#include "homehub_chat.h"
#include "homehub_platform.h"
#include "homehub_event_queue.h"
#include "homehub_cloud_config.h"
#include "tuya_authorize.h"
#include "tal_cli.h"
#include "tuya_iot.h"
#include "tuya_ai_agent.h"
#include "tuya_ai_internal.h"
#include "svc_ai_player.h"
#include "netmgr.h"
#include "netconn_wifi.h"
#include "tuya_tls.h"
#include "tuya_register_center.h"
#include "tkl_memory.h"
#include <stdio.h>
#include <string.h>

/* Only this bounded software FIFO is touched by the microphone callback.
 * The original audio owner remains the sole codec opener and DAC writer. */
/* 380 normal slots at the microphone's 640B/20ms framing hold 7.60s,
 * covering the pinned SDK's legal 6s blocking send plus scheduling margin.
 * Four additional slots remain available for ordered control commands. */
#define CLOUD_SLOTS 384u
#define CLOUD_CONTROL_SLOTS 4u
#define CLOUD_DATA_SLOTS (CLOUD_SLOTS-CLOUD_CONTROL_SLOTS)
#define CLOUD_FRAME 1280u
#define SESSION_RETRY_MS 5000u
#define SESSION_MAX_ATTEMPTS 3u
typedef enum { C_BEGIN,C_PCM,C_SUBMIT,C_TTS_START,C_TTS_DATA,C_TTS_END,C_TEXT,C_EMOTION } CloudCommand;
typedef struct {CloudCommand kind;uint32_t turn,timestamp,bytes;bool user_text;uint8_t emotion;uint8_t data[CLOUD_FRAME];} CloudItem;
static struct {
    MUTEX_HANDLE lock;THREAD_HANDLE thread,iot_thread;tuya_iot_client_t iot;AI_PLAYER_HANDLE player;
    CloudItem *queue;unsigned head,count,callbacks;
    unsigned queue_peak;uint32_t begin_max_ms,pcm_max_ms;bool budget_reported;
    HH_ChatAvailability availability;char status[128],sid[AI_UUID_V4_LEN+1],eid[AI_UUID_V4_LEN+1];
    uint32_t turn,speaker_turn,submitted_at,completed_turn;
    uint32_t session_retry_at;unsigned session_attempts;
    bool have_completed,have_emotion;
    uint8_t emotion;
    bool started,runtime_ready,authorized,iot_ready,mqtt,agent_ready,run_pending,session_ready,wifi_disconnect_done;
    bool session_epoch_ready,session_run_seen,session_recovery_pending;
    bool capture,submitted,input_started,cancel,closing,failed,failed_reported,player_attempted,received_start;
    bool input_has_pcm,input_eof_sent;
    bool tts_eof,wrote_audio,player_stopped,session_lost;
} cloud;
static bool cached_network;
static void lock_cloud(void){tal_mutex_lock(cloud.lock);}
static void unlock_cloud(void){tal_mutex_unlock(cloud.lock);}
static void status_locked(HH_ChatAvailability a,const char *s){cloud.availability=a;snprintf(cloud.status,sizeof(cloud.status),"%s",s);}
static void report(uint32_t turn,HH_ChatEvent what,bool ok){HH_Event e={.type=EV_CHAT_REPORT,.chat_event=what,.ok=ok};e.action.chat_turn=turn;hh_post(&e);}
static void fail_locked(const char *s){
    /* Callers supply static status text only: never log cloud payloads or IDs.
     * Preserve the first failure for diagnosis instead of flooding UART. */
    if(!cloud.failed)PR_NOTICE("HH chat fail turn=%u queued=%u native=%d reason=%s",(unsigned)cloud.turn,cloud.count,tal_system_get_free_heap_size(),s);
    cloud.capture=false;cloud.cancel=true;cloud.failed=true;cloud.have_emotion=false;cloud.emotion=HH_EMOTION_NEUTRAL;snprintf(cloud.status,sizeof(cloud.status),"%s",s);
}
static void fail_turn(uint32_t turn,const char *s){lock_cloud();if(turn && cloud.turn==turn)fail_locked(s);unlock_cloud();}
static void fail_network(uint32_t turn,const char *s){lock_cloud();if(turn && cloud.turn==turn){status_locked(cached_network?HH_CHAT_CONNECTING:HH_CHAT_OFFLINE,s);fail_locked(s);}unlock_cloud();}
static bool control_command(CloudCommand kind){return kind==C_BEGIN || kind==C_SUBMIT || kind==C_TTS_START || kind==C_TTS_END;}
static unsigned data_room_locked(void){return cloud.count<CLOUD_DATA_SLOTS?CLOUD_DATA_SLOTS-cloud.count:0;}
static void reset_budget_locked(void){cloud.queue_peak=cloud.count;cloud.begin_max_ms=cloud.pcm_max_ms=0;cloud.budget_reported=false;}
static void note_sdk_elapsed(uint32_t turn,bool begin,uint32_t started){
    uint32_t elapsed=(uint32_t)tal_system_get_millisecond()-started;lock_cloud();
    if(cloud.turn==turn){uint32_t *maximum=begin?&cloud.begin_max_ms:&cloud.pcm_max_ms;if(elapsed>*maximum)*maximum=elapsed;}
    unlock_cloud();
}
static void report_budget(uint32_t turn){
    lock_cloud();bool show=cloud.turn==turn && !cloud.budget_reported;
    unsigned peak=cloud.queue_peak;uint32_t begin=cloud.begin_max_ms,pcm=cloud.pcm_max_ms;bool failed=cloud.failed;
    if(show)cloud.budget_reported=true;
    unlock_cloud();
    if(show)PR_NOTICE("HH chat budget turn=%u peak=%u begin_max_ms=%u pcm_max_ms=%u native=%d failed=%u",(unsigned)turn,peak,(unsigned)begin,(unsigned)pcm,tal_system_get_free_heap_size(),(unsigned)failed);
}
static bool put_locked(CloudCommand kind,uint32_t turn,const uint8_t *data,uint32_t bytes,uint32_t time){
    if(!cloud.queue || cloud.count>=CLOUD_SLOTS || (!control_command(kind) && !data_room_locked()))return false;
    CloudItem *i=&cloud.queue[(cloud.head+cloud.count)%CLOUD_SLOTS];
    i->kind=kind;i->turn=turn;i->bytes=bytes;i->timestamp=time;i->user_text=false;i->emotion=HH_EMOTION_NEUTRAL;if(bytes)memcpy(i->data,data,bytes);cloud.count++;if(cloud.count>cloud.queue_peak)cloud.queue_peak=cloud.count;return true;
}
static bool take(CloudItem *i){bool ok=false;lock_cloud();if(cloud.count && !cloud.cancel){*i=cloud.queue[cloud.head];cloud.head=(cloud.head+1)%CLOUD_SLOTS;cloud.count--;ok=true;}unlock_cloud();return ok;}
static bool current(uint32_t t){lock_cloud();bool ok=cloud.turn==t && !cloud.cancel;unlock_cloud();return ok;}
/* Core turn IDs advance monotonically, skip zero, and may wrap. A completed
 * generation must never reacquire an idle backend through a delayed cancel. */
static bool fresh_turn_locked(uint32_t t){return t && (!cloud.have_completed || (int32_t)(t-cloud.completed_turn)>0);}
HH_ChatAvailability hh_chat_cloud_availability(void){if(!cloud.lock)return HH_CHAT_OFFLINE;lock_cloud();HH_ChatAvailability a=cloud.availability;unlock_cloud();return a;}
void hh_chat_cloud_status(char *s,size_t n){if(!s || !n)return;if(!cloud.lock){snprintf(s,n,"聊天尚未初始化");return;}lock_cloud();snprintf(s,n,"%s",cloud.status);unlock_cloud();}
bool hh_chat_network_ready(void){if(!cloud.lock)return false;lock_cloud();bool ok=cached_network;unlock_cloud();return ok;}
bool hh_chat_cloud_begin(uint32_t t){
    if(!t || !cloud.lock)return false;
    lock_cloud();bool ok=fresh_turn_locked(t) && cloud.availability==HH_CHAT_READY && cached_network && cloud.mqtt && !cloud.turn && cloud.session_ready;
    if(ok){reset_budget_locked();cloud.have_emotion=false;cloud.emotion=HH_EMOTION_NEUTRAL;cloud.turn=t;cloud.capture=true;cloud.cancel=cloud.closing=cloud.failed=cloud.failed_reported=false;cloud.submitted=cloud.input_started=cloud.player_attempted=false;cloud.input_has_pcm=cloud.input_eof_sent=false;
        cloud.received_start=cloud.tts_eof=cloud.wrote_audio=false;cloud.player_stopped=true;ok=put_locked(C_BEGIN,t,NULL,0,0);if(!ok)fail_locked("聊天命令队列已满");}
    unlock_cloud();return ok;
}
bool hh_chat_cloud_pcm(uint32_t t,const uint8_t *pcm,uint32_t bytes){
    if(!cloud.lock || !pcm || !bytes || (bytes&1u))return false;
    uint32_t chunks=bytes/CLOUD_FRAME+(bytes%CLOUD_FRAME!=0),time=(uint32_t)tal_system_get_millisecond();
    lock_cloud();bool ok=t && t==cloud.turn && cloud.capture && !cloud.cancel;
    if(ok && chunks>data_room_locked()){fail_locked("聊天上传队列已满");ok=false;}
    if(ok){for(uint32_t off=0;off<bytes;){uint32_t n=bytes-off;if(n>CLOUD_FRAME)n=CLOUD_FRAME;put_locked(C_PCM,t,pcm+off,n,time+off/32u);off+=n;}}
    unlock_cloud();return ok;
}
bool hh_chat_cloud_submit(uint32_t t){
    if(!cloud.lock)return false;
    lock_cloud();bool ok=t && t==cloud.turn && cloud.capture && !cloud.cancel;
    if(ok){cloud.capture=false;ok=put_locked(C_SUBMIT,t,NULL,0,0);if(!ok)fail_locked("聊天提交队列已满");}unlock_cloud();return ok;
}
void hh_chat_cloud_cancel(uint32_t t){
    if(!cloud.lock || !t)return;
    lock_cloud();
    /* A failed app/audio queue may require quiescence before BEGIN arrived. */
    if(!cloud.turn && fresh_turn_locked(t)){reset_budget_locked();cloud.turn=t;cloud.failed=cloud.failed_reported=false;}
    if(cloud.turn==t){cloud.capture=false;cloud.cancel=true;cloud.have_emotion=false;cloud.emotion=HH_EMOTION_NEUTRAL;}unlock_cloud();
}
static OPERATE_RET sink_open(PLAYER_CONSUMER_HANDLE *h){*h=NULL;return OPRT_OK;}
static OPERATE_RET sink_noop(PLAYER_CONSUMER_HANDLE h){(void)h;return OPRT_OK;}
static OPERATE_RET sink_volume(PLAYER_CONSUMER_HANDLE h,uint32_t v){(void)h;(void)v;return OPRT_OK;}
static OPERATE_RET sink_write(PLAYER_CONSUMER_HANDLE h,const void *p,uint32_t n){
    (void)h;lock_cloud();uint32_t t=cloud.speaker_turn;bool allowed=t && t==cloud.turn && !cloud.cancel && !cloud.closing;unlock_cloud();if(!allowed)return OPRT_COM_ERROR;
    /* Owner pacing and DAC calls can block. Never retain the cloud lock. */
    bool ok=hh_audio_chat_sink_write(t,p,n);lock_cloud();if(cloud.turn==t){if(ok && n)cloud.wrote_audio=true;else if(!ok)fail_locked("聊天扬声器写入失败");}unlock_cloud();return ok?OPRT_OK:OPRT_COM_ERROR;
}
/* Consumer stop deliberately does not clear the hardware FIFO at natural EOF.
 * The worker selects estimated drain or cancellation after actual STOPPED. */
static AI_PLAYER_CONSUMER_T chat_consumer={sink_open,sink_noop,sink_noop,sink_write,sink_noop,sink_volume};
static OPERATE_RET player_event(void *p){AI_PLAYER_EVT_T *e=p;if(e){lock_cloud();if(e->handle==cloud.player)cloud.player_stopped=e->state==AI_PLAYER_STOPPED;unlock_cloud();}return OPRT_OK;}
static OPERATE_RET client_run(void *p){(void)p;lock_cloud();cloud.run_pending=true;unlock_cloud();return OPRT_OK;}
static OPERATE_RET session_closed(void *p){lock_cloud();if(p && cloud.sid[0] && !strcmp(p,cloud.sid)){cloud.session_lost=true;cloud.session_ready=false;cloud.session_recovery_pending=true;cloud.session_retry_at=(uint32_t)tal_system_get_millisecond();status_locked(HH_CHAT_CONNECTING,"云端会话已断开");if(cloud.turn)fail_locked("云端会话已断开");}unlock_cloud();return OPRT_OK;}
static uint32_t callback_enter(void){lock_cloud();uint32_t t=0;if(cloud.turn && cloud.submitted && !cloud.cancel && !cloud.closing){t=cloud.turn;cloud.callbacks++;}unlock_cloud();return t;}
static void callback_leave(void){lock_cloud();cloud.callbacks--;unlock_cloud();}
static bool session_matches(const char *s){return !s || !s[0] || (cloud.sid[0] && strstr(s,cloud.sid)!=NULL);}
static OPERATE_RET receive_audio(AI_BIZ_ATTR_INFO_T *a,AI_BIZ_HEAD_INFO_T *h,void *p,void *user){
    (void)user;if(!h)return OPRT_INVALID_PARM;uint32_t t=callback_enter();if(!t)return OPRT_OK;
    lock_cloud();bool ok=cloud.turn==t && !cloud.cancel && !cloud.closing &&
        session_matches(a?a->value.audio.option.session_id_list:NULL);
    bool start=h->stream_flag==AI_STREAM_START || h->stream_flag==AI_STREAM_ONE,end=h->stream_flag==AI_STREAM_END || h->stream_flag==AI_STREAM_ONE;
    if(ok && start){
        if(!a || !(a->flag&AI_HAS_ATTR) || a->type!=AI_PT_AUDIO || a->value.audio.base.codec_type!=AUDIO_CODEC_MP3 || a->value.audio.base.sample_rate!=16000 || a->value.audio.base.channels!=1){fail_locked("云端音频格式不支持");ok=false;}
        else if(cloud.received_start){fail_locked("云端音频流重复开始");ok=false;}
        else {cloud.received_start=true;if(!put_locked(C_TTS_START,t,NULL,0,0)){fail_locked("云端音频队列已满");ok=false;}}
    }
    if(ok && cloud.received_start && !cloud.cancel){
        uint32_t bytes=h->len;unsigned slots=bytes/CLOUD_FRAME+(bytes%CLOUD_FRAME!=0);
        if((bytes && !p) || slots>data_room_locked() || (end && cloud.count+slots>=CLOUD_SLOTS))fail_locked("云端音频队列已满");
        else {for(uint32_t off=0;off<bytes;){uint32_t n=bytes-off;if(n>CLOUD_FRAME)n=CLOUD_FRAME;put_locked(C_TTS_DATA,t,(uint8_t*)p+off,n,0);off+=n;}if(end)put_locked(C_TTS_END,t,NULL,0,0);}
    }
    unlock_cloud();callback_leave();return OPRT_OK;
}
static OPERATE_RET receive_event(AI_EVENT_ATTR_T *e,AI_EVENT_HEAD_T *h,void *p){
    (void)p;if(!e || !h)return OPRT_INVALID_PARM;lock_cloud();
    bool ok=cloud.turn && !cloud.cancel && !cloud.closing && cloud.eid[0] && !strcmp(cloud.eid,e->event_id) && cloud.sid[0] && !strcmp(cloud.sid,e->session_id);
    if(ok && (h->type==AI_EVENT_CHAT_BREAK || h->type==AI_EVENT_CHAT_EXIT))fail_locked("云端已中止聊天");
    unlock_cloud();return OPRT_OK;
}
static const char *json_string(const ty_cJSON *item){return ty_cJSON_IsString(item)?item->valuestring:NULL;}
static const char *first_array_string(const ty_cJSON *array){
    return cJSON_IsArray(array)?json_string(ty_cJSON_GetArrayItem(array,0)):NULL;
}
static bool reply_emotion(ty_cJSON *body,bool nlg,bool skill,HH_Emotion *emotion){
    /* SDK ai_skill.c consumes only NLG tags[0], a Unicode code label. */
    if(nlg){const char *tag=first_array_string(ty_cJSON_GetObjectItem(body,"tags"));
        return tag && !strncmp(tag,"U+",2) && hh_emotion_parse(tag,emotion);}
    if(skill){
        const char *code=json_string(ty_cJSON_GetObjectItem(body,"code"));
        if(!code || (strcmp(code,"emo") && strcmp(code,"llm_emo")))return false;
        ty_cJSON *content=ty_cJSON_GetObjectItem(body,"skillContent");
        const char *name=first_array_string(ty_cJSON_GetObjectItem(content,"emotion"));
        const char *tag=first_array_string(ty_cJSON_GetObjectItem(content,"text"));
        /* skill_emotion.c pairs the two arrays. Require a real pair and
         * prefer the documented code; names support that same SDK form. */
        return name && tag && (hh_emotion_parse(tag,emotion) || hh_emotion_parse(name,emotion));
    }
    return false;
}
static OPERATE_RET receive_text(AI_BIZ_ATTR_INFO_T *a,AI_BIZ_HEAD_INFO_T *h,void *p,void *user){
    (void)a;(void)user;if(!h || !p)return OPRT_OK;uint32_t t=callback_enter();if(!t)return OPRT_OK;
    ty_cJSON *root=h->data_type==AI_BIZ_DATA_TYPE_JSON?(ty_cJSON*)p:ty_cJSON_ParseWithLengthOpts(p,h->len,NULL,0);
    if(root){ty_cJSON *kind=ty_cJSON_GetObjectItem(root,"bizType"),*body=ty_cJSON_GetObjectItem(root,"data");
        bool asr=kind && kind->valuestring && !strcmp(kind->valuestring,"ASR"),nlg=kind && kind->valuestring && !strcmp(kind->valuestring,"NLG"),skill=kind && kind->valuestring && !strcmp(kind->valuestring,"SKILL");
        ty_cJSON *text=ty_cJSON_GetObjectItem(body,asr?"text":"content"),*request=ty_cJSON_GetObjectItem(root,"requestId");
        HH_Emotion emotion=HH_EMOTION_NEUTRAL;bool have_emotion=reply_emotion(body,nlg,skill,&emotion);
        lock_cloud();bool matching=!request || !request->valuestring || !strcmp(request->valuestring,cloud.eid);
        if(matching && (asr || nlg) && text && text->valuestring && text->valuestring[0] && cloud.turn==t && !cloud.cancel && !cloud.closing){size_t n=strlen(text->valuestring);if(n>191)n=191;while(n && ((unsigned char)text->valuestring[n]&0xc0)==0x80)n--;
            if(put_locked(C_TEXT,t,(uint8_t*)text->valuestring,(uint32_t)n,0))cloud.queue[(cloud.head+cloud.count-1)%CLOUD_SLOTS].user_text=asr;else fail_locked("聊天文字队列已满");}
        /* Emotion admission is stricter than the historical text path. A
         * callback can race cancellation while parsing, so recheck the turn
         * and exact request ID under the lock immediately before copying. */
        const char *request_id=json_string(request);
        if(have_emotion && request_id && request_id[0] && cloud.eid[0] && !strcmp(request_id,cloud.eid) &&
           cloud.turn==t && !cloud.cancel && !cloud.closing && (!cloud.have_emotion || cloud.emotion!=(uint8_t)emotion)){
            if(put_locked(C_EMOTION,t,NULL,0,0)){
                cloud.queue[(cloud.head+cloud.count-1)%CLOUD_SLOTS].emotion=(uint8_t)emotion;
                cloud.have_emotion=true;cloud.emotion=(uint8_t)emotion;
            }
            /* A cosmetic update never interrupts audio when the FIFO is full. */
        }
        unlock_cloud();if(h->data_type!=AI_BIZ_DATA_TYPE_JSON)ty_cJSON_Delete(root);}
    callback_leave();return OPRT_OK;
}
static bool network_check(void){return hh_chat_network_ready();}
static void cloud_iot_event(tuya_iot_client_t *c,tuya_event_msg_t *e){
    (void)c;if(!e)return;lock_cloud();
    if(e->id==TUYA_EVENT_MQTT_CONNECTED){cloud.mqtt=true;status_locked(HH_CHAT_CONNECTING,"正在连接语音服务");}
    else if(e->id==TUYA_EVENT_MQTT_DISCONNECT){cloud.mqtt=false;cloud.session_epoch_ready=false;status_locked(HH_CHAT_OFFLINE,"涂鸦云连接已断开");if(cloud.turn)fail_locked("涂鸦云连接已断开");}
    else if(e->id==TUYA_EVENT_BIND_START)status_locked(HH_CHAT_CONNECTING,"请在智能生活 App 中配网绑定");
    else if(e->id==TUYA_EVENT_BIND_TOKEN_ON)status_locked(HH_CHAT_CONNECTING,"正在完成云端绑定");
    unlock_cloud();
}
static void iot_worker(void *p){(void)p;for(;;){tuya_iot_yield(&cloud.iot);tal_system_sleep(5);}}
static size_t wifi_text_length(const char *text,size_t limit){size_t n=0;if(!text)return limit;while(n<limit&&text[n])n++;return n;}
static bool select_startup_wifi(netconn_wifi_info_t *wifi,const char **source){
    const char *ssid=NULL,*password=NULL;*source=NULL;
    if(hh_device.wifi_ssid[0]){ssid=hh_device.wifi_ssid;password=hh_device.wifi_password;*source="tf";}
    else{
        const HH_CloudSettings *settings=hh_cloud_settings();
        if(settings&&settings->fallback_ssid&&settings->fallback_ssid[0]){
            ssid=settings->fallback_ssid;password=settings->fallback_password;*source="private";
        }
    }
    if(!*source)return false;
    size_t s=wifi_text_length(ssid,sizeof(wifi->ssid)),p=wifi_text_length(password,sizeof(wifi->pswd));
    if(!s||s>=sizeof(wifi->ssid)||p>=sizeof(wifi->pswd))return false;
    memcpy(wifi->ssid,ssid,s+1);memcpy(wifi->pswd,password,p+1);return true;
}
static void wifi_disconnect_barrier(void *unused){
    (void)unused;lock_cloud();cloud.wifi_disconnect_done=true;unlock_cloud();
}
static bool apply_startup_wifi(const netconn_wifi_info_t *wifi,const char *source){
    /* Flush the SDK's initial auto-connect first: until its worker runs the
     * SDK still reports READY and CLOSE is a no-op. Then fence the actual
     * disconnect in the same normal WORKQ_SYSTEM FIFO. Initial LINK_DOWN
     * alone cannot prove either stage. No credential pointer enters the FIFO. */
    uint32_t started=(uint32_t)tal_system_get_millisecond();
    int fence_rc=-99,close_rc=-99,barrier_rc=-99,status_rc=-99,set_rc=-99;
    netmgr_status_e status=NETMGR_LINK_DOWN;
    lock_cloud();cloud.wifi_disconnect_done=false;unlock_cloud();
    fence_rc=tal_workq_schedule(WORKQ_SYSTEM,wifi_disconnect_barrier,NULL);
    if(fence_rc!=OPRT_OK)goto done;
    for(;;){
        lock_cloud();bool flushed=cloud.wifi_disconnect_done;unlock_cloud();
        if(flushed)break;
        if((uint32_t)((uint32_t)tal_system_get_millisecond()-started)>=3000u)goto done;
        tal_system_sleep(20);
    }
    lock_cloud();cloud.wifi_disconnect_done=false;unlock_cloud();
    close_rc=netmgr_conn_set(NETCONN_WIFI,NETCONN_CMD_CLOSE,NULL);
    if(close_rc!=OPRT_OK)goto done;
    barrier_rc=tal_workq_schedule(WORKQ_SYSTEM,wifi_disconnect_barrier,NULL);
    if(barrier_rc!=OPRT_OK)goto done;
    for(;;){
        lock_cloud();bool disconnected=cloud.wifi_disconnect_done;unlock_cloud();
        status_rc=netmgr_conn_get(NETCONN_WIFI,NETCONN_CMD_STATUS,&status);
        if(status_rc!=OPRT_OK)break;
        if(disconnected&&status==NETMGR_LINK_DOWN){
            set_rc=netmgr_conn_set(NETCONN_WIFI,NETCONN_CMD_SSID_PSWD,(void*)wifi);break;
        }
        if((uint32_t)((uint32_t)tal_system_get_millisecond()-started)>=3000u)break;
        tal_system_sleep(20);
    }
done:
    PR_NOTICE("HH wifi config source=%s selected=1 net_rc=0 fence_rc=%d close_rc=%d barrier_rc=%d status_rc=%d status=%u set_rc=%d elapsed_ms=%u",
        source,fence_rc,close_rc,barrier_rc,status_rc,(unsigned)status,set_rc,
        (unsigned)((uint32_t)tal_system_get_millisecond()-started));
    return set_rc==OPRT_OK;
}
static void cloud_initialize(void){
    netconn_wifi_info_t wifi={0};const char *wifi_source=NULL;
    bool wifi_valid=select_startup_wifi(&wifi,&wifi_source);
    bool network_requested=wifi_source || (hh_device.server_url[0] && hh_device.device_token[0]);
    if(!hh_device.chat_enabled && !network_requested){lock_cloud();status_locked(HH_CHAT_DISABLED,"语音聊天已关闭");unlock_cloud();return;}
    /* Reading a license requires KV, but an absent license and empty network
     * configuration do not require TLS, netmgr or their background services. */
    /* Preserve compatibility with authorization written by your_chat_bot. */
    if(tal_kv_init(&(tal_kv_cfg_t){.seed="vmlkasdh93dlvlcy",.key="dflfuap134ddlduq"})!=OPRT_OK){lock_cloud();status_locked(HH_CHAT_OFFLINE,"授权存储初始化失败");unlock_cloud();return;}
    tuya_iot_license_t license={0};OPERATE_RET auth_rc=OPRT_COM_ERROR;
    if(hh_device.chat_enabled)auth_rc=tuya_authorize_read(&license);
    bool read_ok=auth_rc==OPRT_OK;
    size_t uuid_len=read_ok && license.uuid?strlen(license.uuid):0;
    size_t key_len=read_ok && license.authkey?strlen(license.authkey):0;
    if(hh_device.chat_enabled)PR_NOTICE("HH chat auth rc=%d read=%u uuid_len=%u key_len=%u",(int)auth_rc,(unsigned)read_ok,(unsigned)uuid_len,(unsigned)key_len);
    bool license_valid=read_ok && (uuid_len==16 || uuid_len==20) && key_len==32;
    if(hh_device.chat_enabled && !license_valid){
        /* KV is available. Expose the SDK authorization shell only while no
         * valid stored license exists; omit the full system/filesystem CLI. */
        int uart_rc=tal_cli_init_with_uart(TUYA_UART_NUM_0);
        OPERATE_RET register_rc=tuya_authorize_init();
        PR_NOTICE("HH chat auth_cli uart_rc=%d register_rc=%d",uart_rc,(int)register_rc);
    }
    bool authorized=hh_device.chat_enabled && license_valid && hh_device.tuya_product_id[0];
    if(!authorized && !network_requested){lock_cloud();status_locked(HH_CHAT_UNAUTHORIZED,"未检测到有效涂鸦授权或产品配置");unlock_cloud();return;}
    tal_sw_timer_init();tal_workq_init();tal_time_service_init();
    bool iot_ok=false;
    if(authorized){tuya_iot_config_t config={.software_ver=PROJECT_VERSION,.productkey=hh_device.tuya_product_id,.uuid=license.uuid,.authkey=license.authkey,.event_handler=cloud_iot_event,.network_check=network_check};iot_ok=tuya_iot_init(&cloud.iot,&config)==OPRT_OK;}
    else {tuya_tls_init();tuya_register_center_init();}
#if defined(ENABLE_LIBLWIP) && ENABLE_LIBLWIP
    TUYA_LwIP_Init();
#endif
    int net_rc=netmgr_init(NETCONN_WIFI);bool net_ok=net_rc==OPRT_OK,wifi_ok=true;
    if(net_ok && iot_ok){netcfg_args_t args={.type=NETCFG_TUYA_BLE|NETCFG_TUYA_WIFI_AP};netmgr_conn_set(NETCONN_WIFI,NETCONN_CMD_NETCFG,&args);}
    if(wifi_source){
        if(net_ok&&wifi_valid)wifi_ok=apply_startup_wifi(&wifi,wifi_source);
        else{wifi_ok=false;PR_NOTICE("HH wifi config source=%s selected=%u net_rc=%d fence_rc=-99 close_rc=-99 barrier_rc=-99 status_rc=-99 status=0 set_rc=-99 elapsed_ms=0",wifi_source,(unsigned)wifi_valid,net_rc);}
    }
    lock_cloud();cloud.runtime_ready=net_ok&&wifi_ok;cloud.authorized=authorized;cloud.iot_ready=iot_ok&&wifi_ok;
    if(!hh_device.chat_enabled)status_locked(HH_CHAT_DISABLED,"语音聊天已关闭");else if(!authorized)status_locked(HH_CHAT_UNAUTHORIZED,"未检测到有效涂鸦授权或产品配置");
    else if(!iot_ok || !net_ok || !wifi_ok)status_locked(HH_CHAT_OFFLINE,"云端或网络初始化失败");else status_locked(HH_CHAT_CONNECTING,"等待配网与云端绑定");
    unlock_cloud();
    if(iot_ok && net_ok && wifi_ok){
        if(tuya_iot_start(&cloud.iot)!=OPRT_OK){lock_cloud();cloud.iot_ready=false;status_locked(HH_CHAT_OFFLINE,"涂鸦云启动失败");unlock_cloud();return;}
        THREAD_CFG_T cfg={.stackDepth=8192,.priority=THREAD_PRIO_1,.thrdname="hh_iot"};
        if(tal_thread_create_and_start(&cloud.iot_thread,NULL,NULL,iot_worker,NULL,&cfg)!=OPRT_OK){lock_cloud();cloud.iot_ready=false;status_locked(HH_CHAT_OFFLINE,"云网络线程启动失败");unlock_cloud();}
    }
}
static bool prepare_session(unsigned attempt){
    OPERATE_RET rc=tuya_ai_agent_crt_session("",0,0,NULL,0);
    AI_AGENT_SESSION_T *s=rc==OPRT_OK?tuya_ai_agent_get_session(""):NULL;
    unsigned sid_len=s?(unsigned)strlen(s->sid):0;
    PR_NOTICE("HH chat session rc=%d attempt=%u sid_len=%u",(int)rc,attempt,sid_len);
    if(rc!=OPRT_OK || !sid_len)return false;
    bool transport_ready=tuya_ai_agent_is_ready();
    lock_cloud();snprintf(cloud.sid,sizeof(cloud.sid),"%s",s->sid);cloud.session_ready=true;cloud.session_lost=false;cloud.session_recovery_pending=false;
    if(!cloud.turn && !cloud.cancel && !cloud.closing && cached_network && cloud.mqtt && transport_ready)status_locked(HH_CHAT_READY,"按住板载 KEY 开始聊天");
    unlock_cloud();return true;
}
static bool initialize_agent(void){
    /* The bounded FIFO is reserved once, before readiness. Microphone and
     * cloud callbacks only copy into it and never allocate. Keep it out of
     * scarce native BSS, particularly on the entirely offline local path. */
    if(!cloud.queue)cloud.queue=tkl_system_psram_malloc(sizeof(CloudItem)*CLOUD_SLOTS);
    if(!cloud.queue)return false;
    AI_PLAYER_CFG_T p={.sample=16000,.datebits=16,.channel=1};
    if(tuya_ai_player_service_init(&p)!=OPRT_OK || tuya_ai_player_set_consumer(&chat_consumer)!=OPRT_OK || tuya_ai_player_create(AI_PLAYER_MODE_FOREGROUND,&cloud.player)!=OPRT_OK)return false;
    /* Full AI playback level as requested; NULL selects global DAC volume. */
    if(tuya_ai_player_set_volume(cloud.player,100)!=OPRT_OK)return false;
    PR_NOTICE("HH chat player volume=100");
    if(tal_event_subscribe(EVENT_AI_PLAYER_STATE,"homehub.chat",player_event,SUBSCRIBE_TYPE_NORMAL)!=OPRT_OK || tal_event_subscribe(EVENT_AI_CLIENT_RUN,"homehub.chat",client_run,SUBSCRIBE_TYPE_NORMAL)!=OPRT_OK || tal_event_subscribe(EVENT_AI_SESSION_CLOSE,"homehub.chat",session_closed,SUBSCRIBE_TYPE_NORMAL)!=OPRT_OK)return false;
    AI_AGENT_CFG_T cfg={0};cfg.attr.audio.codec_type=AUDIO_CODEC_PCM;cfg.attr.audio.sample_rate=16000;cfg.attr.audio.channels=1;cfg.attr.audio.bit_depth=16;
    /* Match the pinned SDK's MP3 default; 16 kbps is its Opus default. */
    cfg.codec_enable=false;cfg.enable_crt_session_ext=true;cfg.tts_cfg.format="mp3";cfg.tts_cfg.sample_rate=16000;cfg.tts_cfg.bit_rate=64000;
    if(tuya_ai_agent_init(&cfg)!=OPRT_OK)return false;
    PR_NOTICE("HH chat output requested codec=mp3 sample=16000 bitrate=64000");
    tuya_ai_agent_server_vad_ctrl(false);
    AI_AGENT_RECV_DATA_T r={0};r.arc[0].type=AI_PT_AUDIO;r.arc[0].cb=receive_audio;r.arc[1].type=AI_PT_TEXT;r.arc[1].cb=receive_text;r.event_cb=receive_event;
    return tuya_ai_agent_set_recv_cb(&r)==OPRT_OK;
}
static bool end_input(uint32_t t){
    /* All PCM calls run synchronously in this worker. Never start or feed the
     * SDK's hidden input.c ringbuffer task: its void stop may time out or
     * return early while old PCM is still queued. agent_end also discards
     * its nested network errors, so send/check the two end operations here. */
    if(!tuya_ai_agent_is_ready()){fail_network(t,"语音上传结束尚未确认：服务未就绪");return false;}
    lock_cloud();bool pcm=cloud.input_has_pcm,eof=cloud.input_eof_sent;unlock_cloud();
    if(pcm && !eof){
        AI_BIZ_HD_T hd={0};hd.audio.timestamp=tal_system_get_millisecond();hd.audio.pts=hd.audio.timestamp*1000u;
        uint32_t started=(uint32_t)tal_system_get_millisecond();OPERATE_RET rc=tuya_ai_agent_upload_stream("",AI_PT_AUDIO,&hd,NULL,0,0);note_sdk_elapsed(t,false,started);
        if(rc!=OPRT_OK){fail_network(t,"语音流结束提交失败");return false;}
        lock_cloud();cloud.input_eof_sent=true;unlock_cloud();
    }
    if(tuya_ai_agent_event(AI_EVENT_END,0)!=OPRT_OK){fail_network(t,"语音上传结束尚未确认");return false;}
    lock_cloud();cloud.input_started=false;unlock_cloud();return true;
}
static void finish_turn(bool natural){
    /* Seal admission in the same critical section as the in-flight snapshot.
     * SDK session deletion does not join callbacks already resolved by its
     * dispatcher. Existing callbacks must leave before any owner is released. */
    lock_cloud();cloud.closing=true;uint32_t t=cloud.turn;unsigned callbacks=cloud.callbacks;bool input=cloud.input_started,player=cloud.player_attempted,session=cloud.session_ready,failed=cloud.failed;
    bool notify=failed && !cloud.failed_reported;cloud.capture=false;if(notify)cloud.failed_reported=true;unlock_cloud();
    if(!t)return;
    if(notify)report(t,HH_CHAT_FAILED,false);
    if(callbacks)return;
    if(input){
        bool remote_end=end_input(t);
        /* Returned synchronous calls leave no local upload worker behind.
         * A failed remote END is reported, then this session's local routes
         * are deleted below. QUIESCED promises local silence, not remote ACK. */
        lock_cloud();cloud.input_started=false;bool extra_failure=!remote_end && !cloud.failed_reported;
        if(extra_failure)cloud.failed_reported=true;
        unlock_cloud();
        if(extra_failure)report(t,HH_CHAT_FAILED,false);
    }
    if(!natural && session)tuya_ai_agent_event(AI_EVENT_CHAT_BREAK,0);
    if(player && (tuya_ai_player_stop(cloud.player)!=OPRT_OK || tuya_ai_player_get_state(cloud.player)!=AI_PLAYER_STOPPED)){fail_turn(t,"语音播放尚未确认停止");return;}
    lock_cloud();cloud.speaker_turn=0;unlock_cloud();if(!natural)hh_audio_chat_sink_stop(t);
    if(session){lock_cloud();cloud.session_ready=false;cloud.sid[0]=0;unlock_cloud();tuya_ai_agent_del_session("");}
    if(!hh_audio_chat_quiesce(t,natural && !failed)){fail_turn(t,"音频尚未确认停止");return;}
    report_budget(t);
    bool transport_ready=cloud.agent_ready && tuya_ai_agent_is_ready();
    lock_cloud();cloud.completed_turn=t;cloud.have_completed=true;cloud.turn=0;cloud.cancel=cloud.closing=false;cloud.count=cloud.head=0;cloud.submitted=cloud.input_started=cloud.player_attempted=cloud.received_start=false;cloud.eid[0]=0;cloud.have_emotion=false;cloud.emotion=HH_EMOTION_NEUTRAL;
    bool reconnect=transport_ready && cached_network && cloud.mqtt;if(reconnect)status_locked(HH_CHAT_CONNECTING,"正在准备下一轮会话");unlock_cloud();report(t,HH_CHAT_QUIESCED,true);
    /* Preserve the explicit post-quiescence next-session refresh. It is not
     * an automatic recovery attempt; a failed refresh enters the same bounded
     * recovery budget used by the current transport epoch. */
    if(reconnect && !prepare_session(0)){lock_cloud();cloud.session_recovery_pending=true;cloud.session_retry_at=(uint32_t)tal_system_get_millisecond()+SESSION_RETRY_MS;status_locked(HH_CHAT_CONNECTING,"云端会话准备失败");unlock_cloud();}
}
static void execute(const CloudItem *i){
    uint32_t t=i->turn;if(!current(t))return;
    switch(i->kind){
    case C_BEGIN:{char eid[AI_UUID_V4_LEN+1]={0};snprintf(eid,sizeof(eid),"hh%08x_%08x",hh_boot_id,t);lock_cloud();snprintf(cloud.eid,sizeof(cloud.eid),"%s",eid);unlock_cloud();
        if(!tuya_ai_agent_is_ready()){fail_turn(t,"语音服务尚未就绪");break;}tuya_ai_agent_set_scode("");tuya_ai_agent_set_eid(eid);lock_cloud();cloud.input_started=true;unlock_cloud();
        uint32_t started=(uint32_t)tal_system_get_millisecond();OPERATE_RET rc=tuya_ai_agent_start("");note_sdk_elapsed(t,true,started);
        if(rc!=OPRT_OK || !current(t)){fail_network(t,"语音采集启动失败");break;}report(t,HH_CHAT_STARTED,true);break;}
    case C_PCM:{
        AI_BIZ_HD_T hd={0};hd.audio.timestamp=i->timestamp;hd.audio.pts=(uint64_t)i->timestamp*1000u;
        lock_cloud();cloud.input_has_pcm=true;unlock_cloud();
        uint32_t started=(uint32_t)tal_system_get_millisecond();OPERATE_RET rc=tuya_ai_agent_upload_stream("",AI_PT_AUDIO,&hd,(char*)i->data,i->bytes,i->bytes);note_sdk_elapsed(t,false,started);
        if(rc!=OPRT_OK)fail_network(t,"语音上传失败");
        break;}
    case C_SUBMIT:
        /* The server can answer synchronously with the end marker. Open the
         * receive phase after prior PCM commands, before checked end calls. */
        lock_cloud();cloud.submitted=true;cloud.submitted_at=(uint32_t)tal_system_get_millisecond();unlock_cloud();
        end_input(t);break;
    case C_TTS_START:if(!hh_audio_chat_sink_begin(t)){fail_turn(t,"聊天扬声器忙碌");break;}lock_cloud();cloud.speaker_turn=t;cloud.player_attempted=true;cloud.player_stopped=false;unlock_cloud();
        if(tuya_ai_player_start(cloud.player,AI_PLAYER_SRC_MEM,NULL,AI_AUDIO_CODEC_MP3)!=OPRT_OK){fail_turn(t,"语音播放启动失败");break;}if(current(t))report(t,HH_CHAT_TTS_STARTED,true);break;
    case C_TTS_DATA:if(tuya_ai_player_feed(cloud.player,(uint8_t*)i->data,i->bytes)!=OPRT_OK)fail_turn(t,"语音解码队列写入失败");break;
    case C_TTS_END:if(tuya_ai_player_feed(cloud.player,NULL,0)!=OPRT_OK)fail_turn(t,"语音结束标记提交失败");lock_cloud();cloud.tts_eof=true;unlock_cloud();break;
    case C_EMOTION:{HH_Event e={.type=EV_CHAT_EMOTION,.emotion=i->emotion};e.action.chat_turn=t;
        /* A full application FIFO must never stall audio for a cosmetic tag. */
        (void)hh_event_try_post(&e,0);break;}
    case C_TEXT:{HH_Event e={.type=EV_CHAT_TEXT,.chat_is_user_text=i->user_text};e.action.chat_turn=t;memcpy(e.text,i->data,i->bytes);e.text[i->bytes]=0;hh_post(&e);break;}
    }
}
static void cloud_tick(void){
    netmgr_status_e state=NETMGR_LINK_DOWN;bool linked=cloud.runtime_ready && netmgr_conn_get(NETCONN_AUTO,NETCONN_CMD_STATUS,&state)==OPRT_OK && state!=NETMGR_LINK_DOWN;
    lock_cloud();cached_network=linked;
    if(cloud.authorized && cloud.iot_ready && !linked)status_locked(HH_CHAT_OFFLINE,"Wi-Fi 已断开");
    if(cloud.turn && (!linked || !cloud.mqtt))fail_locked("网络已断开");
    bool init=cloud.iot_ready && linked && cloud.mqtt && !cloud.agent_ready;unlock_cloud();
    if(init){bool ok=initialize_agent();lock_cloud();cloud.agent_ready=ok;if(!ok){cloud.iot_ready=false;status_locked(HH_CHAT_OFFLINE,"语音服务初始化失败");}unlock_cloud();}
    bool transport_ready=cloud.agent_ready && tuya_ai_agent_is_ready();
    lock_cloud();
    if(cloud.agent_ready && linked && cloud.mqtt && !transport_ready){status_locked(HH_CHAT_CONNECTING,"语音服务连接已断开，等待重连");if(cloud.turn)fail_locked("语音服务连接已断开");}
    /* A short Wi-Fi drop can leave the authenticated AI transport and its
     * session intact. Restore only the conjunction of all verified layers. */
    if(!cloud.turn && cloud.authorized && cloud.iot_ready && linked && cloud.mqtt && transport_ready && cloud.session_ready && !cloud.session_lost)
        status_locked(HH_CHAT_READY,"按住板载 KEY 开始聊天");
    uint32_t now=(uint32_t)tal_system_get_millisecond();
    bool ready=cloud.authorized && cloud.iot_ready && linked && cloud.mqtt && transport_ready;
    bool missing_session=!cloud.session_ready && (!cloud.sid[0] || cloud.session_lost);
    if(!ready)cloud.session_epoch_ready=false;
    else if(!cloud.session_epoch_ready){
        cloud.session_epoch_ready=true;cloud.session_attempts=0;cloud.session_retry_at=now;
        if(cloud.session_run_seen && missing_session)cloud.session_recovery_pending=true;
    }
    if(cloud.run_pending && cloud.agent_ready){
        cloud.run_pending=false;cloud.session_run_seen=true;
        if(missing_session && !cloud.session_attempts){cloud.session_recovery_pending=true;cloud.session_retry_at=now;}
    }
    bool need=ready && missing_session && cloud.session_recovery_pending && !cloud.turn && !cloud.cancel && !cloud.closing &&
        cloud.session_attempts<SESSION_MAX_ATTEMPTS && (int32_t)(now-cloud.session_retry_at)>=0;
    unsigned attempt=need?++cloud.session_attempts:0;unlock_cloud();
    /* Session creation synchronously waits for MQTT/token/session replies.
     * Never hold the cloud lock, and measure backoff from its completion. */
    if(need && !prepare_session(attempt)){
        uint32_t finished=(uint32_t)tal_system_get_millisecond();lock_cloud();cloud.session_retry_at=finished+SESSION_RETRY_MS;
        if(cached_network && cloud.mqtt){status_locked(HH_CHAT_CONNECTING,cloud.session_attempts<SESSION_MAX_ATTEMPTS?"会话准备失败，稍后重试":"会话准备失败，请检查云端智能体后重连");}
        unlock_cloud();
    }
    for(unsigned n=0;n<4;n++){CloudItem i;if(!take(&i))break;execute(&i);}
    lock_cloud();uint32_t t=cloud.turn;if(t && cloud.submitted && !cloud.received_start && (uint32_t)((uint32_t)tal_system_get_millisecond()-cloud.submitted_at)>45000u)fail_locked("等待云端回答超时");
    bool cancel=cloud.cancel,natural=t && !cancel && cloud.tts_eof && cloud.wrote_audio && !cloud.failed && !cloud.count;unlock_cloud();
    if(cancel)finish_turn(false);else if(natural && tuya_ai_player_get_state(cloud.player)==AI_PLAYER_STOPPED)finish_turn(true);
}
static void cloud_worker(void *p){(void)p;cloud_initialize();for(;;){cloud_tick();tal_system_sleep(5);}}
void hh_chat_cloud_start(void){
    if(cloud.started)return;
    if(tal_mutex_create_init(&cloud.lock)!=OPRT_OK)return;
    cloud.started=true;status_locked(HH_CHAT_CONNECTING,"正在初始化网络与聊天");
    /* A worker remains available for asynchronous pre-BEGIN cancellation;
     * its stack must not consume the native heap needed by FatFs FIL. */
    THREAD_CFG_T cfg={.stackDepth=12288,.priority=THREAD_PRIO_1,.thrdname="hh_chat_cloud",.psram_mode=1};
    if(tal_thread_create_and_start(&cloud.thread,NULL,NULL,cloud_worker,NULL,&cfg)!=OPRT_OK){lock_cloud();status_locked(HH_CHAT_OFFLINE,"聊天线程启动失败");unlock_cloud();}
}
#ifdef HH_CHAT_CLOUD_TEST
static void cloud_test_reset(void){if(cloud.queue)tkl_system_psram_free(cloud.queue);memset(&cloud,0,sizeof(cloud));cached_network=false;}
#endif
