#define HH_CHAT_CLOUD_TEST 1
#include "stubs/fake_sdk.h"
#include ".build/official_kv.h"
#include <stdarg.h>
#include "../../firmware/src/homehub_chat.h"
#include "../../firmware/src/homehub_cloud_config.h"
HH_DeviceConfig hh_device;
uint32_t hh_boot_id=7;
void *hh_events=(void*)4;
static bool reject_app_post;
static unsigned app_post_calls,app_post_wait;
static int failures,lock_depth,iot_calls,net_calls,wifi_sets,upload_count,input_stops,player_stops,sink_writes,quiesces,report_failed,report_quiet;
static unsigned reply_volume_calls;static int reply_volume;static bool reply_volume_fails;
static unsigned kv_inits,timer_inits,workq_inits,time_inits,lwip_inits,tls_inits,register_inits,netcfg_sets,player_inits,psram_allocs,native_stack_bytes;
static HH_CloudSettings wifi_settings;static bool wifi_settings_present;
static unsigned wifi_closes,wifi_barriers,wifi_gets,iot_starts,wifi_notice_count;
static int wifi_close_rc,wifi_barrier_rc,wifi_status_rc,wifi_set_rc,wifi_net_rc;
static bool wifi_close_pending,wifi_close_processed,wifi_barrier_stalled,wifi_set_too_early;
static bool wifi_initial_fence_passed,wifi_close_before_fence,wifi_barrier_requires_close;
static uint32_t wifi_close_at,wifi_close_delay;
static void(*wifi_barrier_callback)(void*);static void *wifi_barrier_data;
static netconn_wifi_info_t selected_wifi;static char wifi_notice[256];
static bool psram_alloc_fails;
static bool kv_matches_official;
static char auth_notice[192];
static unsigned auth_notice_count;
static char auth_cli_notice[160];
static unsigned auth_cli_notice_count,cli_uart_calls,cli_auth_calls,full_cli_calls,license_reads;
static int kv_result,cli_uart_result,cli_auth_result;
static bool kv_ready,license_malformed;
static uint8_t cli_uart_number;
static bool licensed,link_up,ai_ready,stop_fails,sink_fails,quiesce_fails,response_on_submit;
static bool callback_on_player_stop;
static bool hidden_input_active,hidden_stop_delayed,sync_end_fails,direct_input_active;
static bool sync_start_fails,sync_upload_fails,session_create_fails;
static unsigned session_create_calls,session_failures_remaining,session_create_delay_ms;
static char session_notice[160];static unsigned session_notice_count;
static char failure_notice[256];static unsigned failure_notice_count;
static char budget_notice[256];static unsigned budget_notice_count;
static unsigned stall_start_frames,stall_upload_frames,stall_rejected,stall_cancel_at;
static uint32_t stall_turn;
static void inject_microphone_while_sdk_blocked(unsigned frames);
static unsigned hidden_input_starts;
static uint32_t callback_captured_during_stop;
static uint32_t callback_enter(void);
static void audio_chunk(int stream,const char*sid);
static uint32_t clock_ms,last_quiet;
static uint8_t uploaded[1048576];static size_t uploaded_bytes;
static uint64_t uploaded_timestamp[2048];static unsigned uploaded_length[2048];
static FAKE_EVENT_CB run_cb,player_cb;
static AI_AGENT_RECV_DATA_T receivers;
static AI_PLAYER_CONSUMER_T consumer;
static AI_AGENT_SESSION_T session;
static AI_PLAYER_STATE_T player_state;
static void (*iot_event)(tuya_iot_client_t*,tuya_event_msg_t*);
static HH_Event events[1024];static unsigned event_count;
#define CHECK(x) do {if(!(x)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);failures++;return;}}while(0)
#define RUN(x) do {int n=failures;x();printf("%s %s\n",failures==n?"PASS":"FAIL",#x);}while(0)
#define SDK_UNLOCKED() do {if(lock_depth){printf("FAIL SDK invoked under cloud lock\n");failures++;}}while(0)
void hh_post(const HH_Event *e){if(event_count<1024)events[event_count++]=*e;if(e->type==EV_CHAT_REPORT){if(e->chat_event==HH_CHAT_FAILED)report_failed++;if(e->chat_event==HH_CHAT_QUIESCED){report_quiet++;last_quiet=e->action.chat_turn;}}}
int tal_queue_post(void *queue,void *message,unsigned wait_ms){
    (void)queue;app_post_calls++;app_post_wait=wait_ms;
    if(reject_app_post)return -1;
    hh_post((const HH_Event*)message);return OPRT_OK;
}
bool hh_event_try_post(const HH_Event *message,uint32_t wait_ms){return tal_queue_post(hh_events,(void*)message,wait_ms)==OPRT_OK;}
int tal_mutex_create_init(MUTEX_HANDLE*m){*m=(void*)1;return 0;}
int tal_mutex_lock(MUTEX_HANDLE m){(void)m;lock_depth++;return 0;}
int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;lock_depth--;return 0;}
int tal_kv_init(const tal_kv_cfg_t*c){kv_inits++;kv_matches_official=!strcmp(c->seed,HH_OFFICIAL_KV_SEED)&&!strcmp(c->key,HH_OFFICIAL_KV_KEY);kv_ready=kv_result==0;return kv_result;}
int tal_cli_init_with_uart(uint8_t n){SDK_UNLOCKED();if(!kv_ready)failures++;cli_uart_calls++;cli_uart_number=n;return cli_uart_result;}
int tuya_authorize_init(void){SDK_UNLOCKED();if(!kv_ready)failures++;cli_auth_calls++;return cli_auth_result;}
int tal_cli_init(void){full_cli_calls++;return 0;}
const HH_CloudSettings *hh_cloud_settings(void){return wifi_settings_present?&wifi_settings:NULL;}
void hh_fake_notice(const char *format,...){va_list args;va_start(args,format);if(!strncmp(format,"HH wifi config ",15)){vsnprintf(wifi_notice,sizeof(wifi_notice),format,args);wifi_notice_count++;}else if(!strncmp(format,"HH chat auth_cli ",17)){vsnprintf(auth_cli_notice,sizeof(auth_cli_notice),format,args);auth_cli_notice_count++;}else if(!strncmp(format,"HH chat session ",16)){vsnprintf(session_notice,sizeof(session_notice),format,args);session_notice_count++;}else if(!strncmp(format,"HH chat fail ",13)){vsnprintf(failure_notice,sizeof(failure_notice),format,args);failure_notice_count++;}else if(!strncmp(format,"HH chat budget ",15)){vsnprintf(budget_notice,sizeof(budget_notice),format,args);budget_notice_count++;}else if(!strncmp(format,"HH chat output ",15)||!strncmp(format,"HH chat player ",15)){}else{vsnprintf(auth_notice,sizeof(auth_notice),format,args);auth_notice_count++;}va_end(args);}
int tal_sw_timer_init(void){timer_inits++;return 0;}
int tal_workq_init(void){workq_inits++;return 0;}
int tal_workq_schedule(int service,void(*callback)(void*),void *data){SDK_UNLOCKED();if(service!=WORKQ_SYSTEM)failures++;wifi_barriers++;if(wifi_barrier_rc)return wifi_barrier_rc;wifi_barrier_callback=callback;wifi_barrier_data=data;wifi_barrier_requires_close=wifi_closes!=0;return 0;}
int tal_time_service_init(void){time_inits++;return 0;}
void TUYA_LwIP_Init(void){lwip_inits++;}
void tuya_tls_init(void){tls_inits++;}
void tuya_register_center_init(void){register_inits++;}
uint64_t tal_system_get_millisecond(void){return clock_ms;}
void tal_system_sleep(unsigned ms){clock_ms+=ms;if(wifi_close_pending&&(uint32_t)(clock_ms-wifi_close_at)>=wifi_close_delay){wifi_close_pending=false;wifi_close_processed=true;link_up=false;}if(wifi_barrier_callback&&(!wifi_barrier_requires_close||(wifi_close_processed&&!wifi_barrier_stalled))){void(*cb)(void*)=wifi_barrier_callback;if(!wifi_barrier_requires_close)wifi_initial_fence_passed=true;wifi_barrier_callback=NULL;cb(wifi_barrier_data);}}
int tal_system_get_free_heap_size(void){return 20000;}
int tal_thread_create_and_start(THREAD_HANDLE*t,void*a,void*b,void(*fn)(void*),void*arg,THREAD_CFG_T*c){(void)a;(void)b;(void)fn;(void)arg;if(!c->psram_mode)native_stack_bytes+=c->stackDepth;*t=(void*)1;return 0;}
void *tkl_system_psram_malloc(size_t n){psram_allocs++;return psram_alloc_fails?NULL:malloc(n);}
void tkl_system_psram_free(void *p){free(p);}
int tal_event_subscribe(const char*event,const char*n,FAKE_EVENT_CB cb,int type){(void)n;(void)type;if(!strcmp(event,EVENT_AI_CLIENT_RUN))run_cb=cb;if(!strcmp(event,EVENT_AI_PLAYER_STATE))player_cb=cb;return 0;}
int tuya_authorize_read(tuya_iot_license_t*l){SDK_UNLOCKED();license_reads++;if(!kv_ready)failures++;if(!licensed){l->uuid=l->authkey=(const char*)(uintptr_t)1;return -1;}l->uuid=license_malformed?"bad":"00000000000000000000";l->authkey="00000000000000000000000000000000";return 0;}
int tuya_iot_init(tuya_iot_client_t*c,const tuya_iot_config_t*cfg){SDK_UNLOCKED();(void)c;iot_calls++;iot_event=cfg->event_handler;return 0;}
int tuya_iot_start(tuya_iot_client_t*c){(void)c;iot_starts++;return 0;}
int tuya_iot_yield(tuya_iot_client_t*c){(void)c;return 0;}
int netmgr_init(int type){(void)type;net_calls++;return wifi_net_rc;}
int netmgr_conn_set(int t,int cmd,void*p){SDK_UNLOCKED();(void)t;if(cmd==NETCONN_CMD_CLOSE){if(!wifi_initial_fence_passed)wifi_close_before_fence=true;wifi_closes++;if(wifi_close_rc)return wifi_close_rc;wifi_close_pending=true;wifi_close_at=clock_ms;return 0;}if(cmd==NETCONN_CMD_SSID_PSWD){wifi_sets++;selected_wifi=*(netconn_wifi_info_t*)p;if(!wifi_close_processed||wifi_barrier_callback)wifi_set_too_early=true;return wifi_set_rc;}if(cmd==NETCONN_CMD_NETCFG)netcfg_sets++;return 0;}
int netmgr_conn_get(int t,int cmd,void*p){SDK_UNLOCKED();(void)t;(void)cmd;wifi_gets++;if(wifi_status_rc)return wifi_status_rc;*(netmgr_status_e*)p=link_up?NETMGR_LINK_UP:NETMGR_LINK_DOWN;return 0;}
static void (*json_read_hook)(const char *key);
ty_cJSON *hh_test_json_get(const ty_cJSON *j,const char *key){
    if(json_read_hook)json_read_hook(key);
    return cJSON_GetObjectItem(j,key);
}
int tuya_ai_agent_init(AI_AGENT_CFG_T*c){SDK_UNLOCKED();if(c->codec_enable||c->attr.audio.sample_rate!=16000||c->attr.audio.channels!=1||c->attr.audio.bit_depth!=16||strcmp(c->tts_cfg.format,"mp3")||c->tts_cfg.sample_rate!=16000||c->tts_cfg.bit_rate!=64000){failures++;}return 0;}
int tuya_ai_agent_set_recv_cb(AI_AGENT_RECV_DATA_T*r){receivers=*r;return 0;}
int tuya_ai_agent_crt_session(char*s,unsigned b,uint64_t t,void*d,unsigned n){SDK_UNLOCKED();(void)s;(void)b;(void)t;(void)d;(void)n;session_create_calls++;clock_ms+=session_create_delay_ms;if(session_create_fails)return -1;if(session_failures_remaining){session_failures_remaining--;return -1;}strcpy(session.sid,"current-session");return 0;}
int tuya_ai_agent_del_session(char*s){SDK_UNLOCKED();(void)s;session.sid[0]=0;return 0;}
AI_AGENT_SESSION_T *tuya_ai_agent_get_session(char*s){(void)s;return &session;}
void tuya_ai_agent_server_vad_ctrl(bool b){(void)b;}
int tuya_ai_agent_set_scode(char*s){(void)s;return 0;}
void tuya_ai_agent_set_eid(char*e){strcpy(session.eid,e);}
bool tuya_ai_agent_is_ready(void){return ai_ready;}
int tuya_ai_agent_event(int e,int t){SDK_UNLOCKED();(void)t;if(e==AI_EVENT_END){input_stops++;if(sync_end_fails)return -1;direct_input_active=false;if(response_on_submit){response_on_submit=false;audio_chunk(AI_STREAM_START,session.sid);}}return 0;}
void tuya_ai_input_start(bool f){SDK_UNLOCKED();(void)f;hidden_input_active=true;hidden_input_starts++;}
void tuya_ai_input_stop(void){SDK_UNLOCKED();input_stops++;if(!hidden_stop_delayed)hidden_input_active=false;if(response_on_submit){response_on_submit=false;audio_chunk(AI_STREAM_START,session.sid);}}
bool tuya_ai_input_is_started(void){SDK_UNLOCKED();return true;}
int tuya_ai_audio_input(uint64_t t,uint64_t p,uint8_t*d,unsigned n,unsigned total){SDK_UNLOCKED();(void)p;(void)total;if(upload_count>=2048||uploaded_bytes+n>sizeof(uploaded)){failures++;return -1;}uploaded_timestamp[upload_count]=t;uploaded_length[upload_count]=n;upload_count++;memcpy(uploaded+uploaded_bytes,d,n);uploaded_bytes+=n;return 0;}
int tuya_ai_agent_start(char*s){SDK_UNLOCKED();(void)s;direct_input_active=true;unsigned frames=stall_start_frames;stall_start_frames=0;if(frames)inject_microphone_while_sdk_blocked(frames);return sync_start_fails?-1:0;}
int tuya_ai_agent_upload_stream(char*s,AI_PACKET_PT type,AI_BIZ_HD_T*h,char*d,unsigned n,unsigned total){SDK_UNLOCKED();(void)s;(void)type;if(sync_upload_fails)return -1;if(d&&n){if(!h || h->audio.pts!=h->audio.timestamp*1000u)failures++;int rc=tuya_ai_audio_input(h->audio.timestamp,h->audio.pts,(uint8_t*)d,n,total);unsigned frames=stall_upload_frames;stall_upload_frames=0;if(frames)inject_microphone_while_sdk_blocked(frames);return rc;}return 0;}
int tuya_ai_player_service_init(AI_PLAYER_CFG_T*c){SDK_UNLOCKED();(void)c;player_inits++;return 0;}
int tuya_ai_player_set_consumer(AI_PLAYER_CONSUMER_T*c){consumer=*c;return 0;}
int tuya_ai_player_create(int m,AI_PLAYER_HANDLE*h){(void)m;*h=(void*)2;return 0;}
int tuya_ai_player_set_volume(AI_PLAYER_HANDLE h,int v){SDK_UNLOCKED();if(h!=(void*)2)failures++;reply_volume_calls++;reply_volume=v;return reply_volume_fails?-1:0;}
int tuya_ai_player_start(AI_PLAYER_HANDLE h,int src,char*v,int codec){SDK_UNLOCKED();(void)h;(void)src;(void)v;(void)codec;player_state=AI_PLAYER_PLAYING;consumer.start(NULL);return 0;}
int tuya_ai_player_feed(AI_PLAYER_HANDLE h,uint8_t*d,uint32_t n){SDK_UNLOCKED();(void)h;if(d&&n)consumer.write(NULL,d,n);return 0;}
int tuya_ai_player_stop(AI_PLAYER_HANDLE h){SDK_UNLOCKED();(void)h;player_stops++;if(callback_on_player_stop){callback_on_player_stop=false;callback_captured_during_stop=callback_enter();}if(stop_fails)return -1;consumer.stop(NULL);player_state=AI_PLAYER_STOPPED;return 0;}
AI_PLAYER_STATE_T tuya_ai_player_get_state(AI_PLAYER_HANDLE h){(void)h;return player_state;}
bool hh_audio_chat_sink_begin(uint32_t t){SDK_UNLOCKED();(void)t;return true;}
bool hh_audio_chat_sink_write(uint32_t t,const uint8_t*p,uint32_t n){SDK_UNLOCKED();(void)t;(void)p;(void)n;sink_writes++;return !sink_fails;}
void hh_audio_chat_sink_stop(uint32_t t){SDK_UNLOCKED();(void)t;}
bool hh_audio_chat_quiesce(uint32_t t,bool natural){SDK_UNLOCKED();(void)t;(void)natural;quiesces++;return !quiesce_fails;}
#include "../../firmware/src/homehub_chat_cloud.c"

static void reset_wifi_fixture(void){memset(&wifi_settings,0,sizeof(wifi_settings));wifi_settings_present=false;wifi_closes=wifi_barriers=wifi_gets=iot_starts=wifi_notice_count=0;wifi_close_rc=wifi_barrier_rc=wifi_status_rc=wifi_set_rc=wifi_net_rc=0;wifi_close_pending=wifi_close_processed=wifi_barrier_stalled=wifi_set_too_early=false;wifi_initial_fence_passed=wifi_close_before_fence=wifi_barrier_requires_close=false;wifi_close_at=0;wifi_close_delay=20;wifi_barrier_callback=NULL;wifi_barrier_data=NULL;memset(&selected_wifi,0,sizeof(selected_wifi));wifi_notice[0]=0;}
static void prepare_boot(bool auth){reset_wifi_fixture();reply_volume_calls=0;reply_volume=-1;reply_volume_fails=false;auth_cli_notice_count=cli_uart_calls=cli_auth_calls=full_cli_calls=license_reads=0;kv_result=cli_uart_result=cli_auth_result=0;kv_ready=license_malformed=false;auth_cli_notice[0]=0;cli_uart_number=255;kv_matches_official=false;auth_notice_count=0;auth_notice[0]=0;memset(&hh_device,0,sizeof(hh_device));hh_device.chat_enabled=true;strcpy(hh_device.tuya_product_id,"development-pid");licensed=auth;link_up=ai_ready=false;iot_calls=net_calls=wifi_sets=0;upload_count=input_stops=player_stops=sink_writes=quiesces=report_failed=report_quiet=0;uploaded_bytes=event_count=clock_ms=last_quiet=callback_captured_during_stop=hidden_input_starts=0;stop_fails=sink_fails=quiesce_fails=response_on_submit=callback_on_player_stop=hidden_input_active=hidden_stop_delayed=sync_end_fails=direct_input_active=sync_start_fails=sync_upload_fails=session_create_fails=psram_alloc_fails=false;kv_inits=timer_inits=workq_inits=time_inits=lwip_inits=tls_inits=register_inits=netcfg_sets=player_inits=psram_allocs=native_stack_bytes=0;player_state=AI_PLAYER_STOPPED;memset(&session,0,sizeof(session));session_create_calls=session_failures_remaining=session_create_delay_ms=session_notice_count=0;session_notice[0]=0;stall_start_frames=stall_upload_frames=stall_rejected=stall_cancel_at=budget_notice_count=0;stall_turn=0;budget_notice[0]=0;cloud_test_reset();}
/* Existing audio/session tests start their clock after startup has completed;
 * dedicated Wi-Fi tests retain elapsed startup time and exercise the wait. */
static void boot(bool auth){prepare_boot(auth);strcpy(hh_device.wifi_ssid,"test");hh_chat_cloud_start();cloud_initialize();clock_ms=0;}
static void inject_microphone_while_sdk_blocked(unsigned frames){
    uint8_t pcm[640];
    for(unsigned i=0;i<frames;i++){
        clock_ms+=20;memset(pcm,(uint8_t)(i+1),sizeof(pcm));
        if(stall_cancel_at && i+1==stall_cancel_at)hh_chat_cloud_cancel(stall_turn);
        if(!hh_chat_cloud_pcm(stall_turn,pcm,sizeof(pcm)))stall_rejected++;
    }
}
static void tick(void){cloud_tick();}
static void ready(void){tuya_event_msg_t e={TUYA_EVENT_MQTT_CONNECTED};link_up=true;if(!iot_event)return;iot_event(NULL,&e);tick();ai_ready=true;if(run_cb)run_cb(NULL);tick();}
static void drain(void){for(unsigned i=0;i<CLOUD_SLOTS+16;i++)tick();}
static void unauthorized_never_initializes_iot_but_keeps_wifi(void){boot(false);CHECK(iot_calls==0&&net_calls==1&&wifi_sets==1);CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED);CHECK(!hh_chat_cloud_begin(1));}
static void kv_parameters_match_installed_official_example(void){boot(false);CHECK(kv_inits==1&&kv_matches_official);}
static void successful_authorization_notice_contains_only_result_and_lengths(void){boot(true);CHECK(auth_notice_count==1);CHECK(!strcmp(auth_notice,"HH chat auth rc=0 read=1 uuid_len=20 key_len=32"));}
static void failed_authorization_notice_does_not_inspect_returned_fields(void){boot(false);CHECK(auth_notice_count==1);CHECK(!strcmp(auth_notice,"HH chat auth rc=-1 read=0 uuid_len=0 key_len=0"));CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED&&iot_calls==0);}
static void missing_license_opens_only_uart0_authorization_commands(void){prepare_boot(false);hh_chat_cloud_start();cloud_initialize();CHECK(kv_ready&&license_reads==1);CHECK(cli_uart_calls==1&&cli_uart_number==TUYA_UART_NUM_0&&cli_auth_calls==1&&full_cli_calls==0);CHECK(auth_cli_notice_count==1&&!strcmp(auth_cli_notice,"HH chat auth_cli uart_rc=0 register_rc=0"));CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED&&net_calls==0);}
static void valid_license_never_opens_overwrite_channel_even_without_pid(void){boot(true);CHECK(cli_uart_calls==0&&cli_auth_calls==0&&full_cli_calls==0);prepare_boot(true);hh_device.tuya_product_id[0]=0;hh_chat_cloud_start();cloud_initialize();CHECK(license_reads==1&&cli_uart_calls==0&&cli_auth_calls==0&&auth_cli_notice_count==0);CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED);}
static void invalid_license_format_allows_authorization_channel(void){prepare_boot(true);license_malformed=true;hh_chat_cloud_start();cloud_initialize();CHECK(cli_uart_calls==1&&cli_auth_calls==1&&full_cli_calls==0);CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED&&net_calls==0);}
static void kv_failure_never_reads_license_or_opens_uart(void){prepare_boot(false);kv_result=-55;hh_chat_cloud_start();cloud_initialize();CHECK(kv_inits==1&&!kv_ready&&license_reads==0&&cli_uart_calls==0&&cli_auth_calls==0);CHECK(hh_chat_cloud_availability()==HH_CHAT_OFFLINE&&net_calls==0);}
static void disabled_chat_never_opens_authorization_channel(void){prepare_boot(false);hh_device.chat_enabled=false;strcpy(hh_device.server_url,"http://example.test");strcpy(hh_device.device_token,"test-token");hh_chat_cloud_start();cloud_initialize();CHECK(kv_ready&&license_reads==0&&cli_uart_calls==0&&cli_auth_calls==0&&auth_cli_notice_count==0);CHECK(hh_chat_cloud_availability()==HH_CHAT_DISABLED);}
static void cli_errors_do_not_start_network_or_block_local_quiescence(void){prepare_boot(false);cli_uart_result=-51;cli_auth_result=-52;hh_chat_cloud_start();cloud_initialize();CHECK(cli_uart_calls==1&&cli_auth_calls==1&&full_cli_calls==0);CHECK(!strcmp(auth_cli_notice,"HH chat auth_cli uart_rc=-51 register_rc=-52"));CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED&&net_calls==0&&player_inits==0);hh_chat_cloud_cancel(4);drain();CHECK(report_quiet==1&&last_quiet==4);}
static void unauthorized_empty_config_skips_network_and_native_stacks(void){prepare_boot(false);hh_chat_cloud_start();cloud_initialize();CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED);CHECK(kv_inits==1);CHECK(net_calls==0&&iot_calls==0&&timer_inits==0&&workq_inits==0&&time_inits==0);CHECK(lwip_inits==0&&tls_inits==0&&register_inits==0&&player_inits==0&&psram_allocs==0);CHECK(native_stack_bytes==0);CHECK(sizeof(cloud)<4096);hh_chat_cloud_cancel(2);drain();CHECK(report_quiet==1&&last_quiet==2);CHECK(!hh_chat_network_ready());}
static void disabled_empty_config_skips_authorization_runtime(void){prepare_boot(false);hh_device.chat_enabled=false;hh_chat_cloud_start();cloud_initialize();CHECK(hh_chat_cloud_availability()==HH_CHAT_DISABLED);CHECK(kv_inits==0&&net_calls==0&&timer_inits==0&&tls_inits==0&&register_inits==0);CHECK(native_stack_bytes==0&&psram_allocs==0&&auth_notice_count==0);}
static void authorized_empty_wifi_still_supports_ble_ap(void){prepare_boot(true);hh_chat_cloud_start();cloud_initialize();CHECK(iot_calls==1&&net_calls==1&&netcfg_sets==1&&wifi_sets==0);CHECK(timer_inits==1&&workq_inits==1&&time_inits==1&&lwip_inits==1);CHECK(player_inits==0&&psram_allocs==0);ready();CHECK(hh_chat_cloud_availability()==HH_CHAT_READY&&player_inits==1&&psram_allocs==1);}
static void unauthorized_http_config_keeps_runtime_without_ssid(void){prepare_boot(false);strcpy(hh_device.server_url,"http://example.test");strcpy(hh_device.device_token,"test-token");hh_chat_cloud_start();cloud_initialize();CHECK(hh_chat_cloud_availability()==HH_CHAT_UNAUTHORIZED);CHECK(net_calls==1&&iot_calls==0&&wifi_sets==0&&netcfg_sets==0);CHECK(timer_inits==1&&tls_inits==1&&register_inits==1&&lwip_inits==1);CHECK(player_inits==0&&psram_allocs==0);link_up=true;tick();CHECK(hh_chat_network_ready());}
static void audio_fifo_allocation_failure_cannot_claim_ready(void){boot(true);psram_alloc_fails=true;ready();CHECK(hh_chat_cloud_availability()==HH_CHAT_OFFLINE);CHECK(!hh_chat_cloud_begin(3));CHECK(player_inits==0&&psram_allocs==1);}
static void wifi_and_mqtt_are_not_chat_ready(void){boot(true);link_up=true;tick();CHECK(hh_chat_network_ready());CHECK(hh_chat_cloud_availability()!=HH_CHAT_READY);ready();CHECK(hh_chat_cloud_availability()==HH_CHAT_READY);CHECK(net_calls==1&&wifi_sets==1);}
static void lost_wifi_revokes_ready_before_capture(void){boot(true);ready();link_up=false;tick();CHECK(hh_chat_cloud_availability()==HH_CHAT_OFFLINE);CHECK(!hh_chat_cloud_begin(9));}
static void ai_transport_loss_revokes_ready_even_with_wifi_and_mqtt(void){boot(true);ready();ai_ready=false;tick();CHECK(hh_chat_cloud_availability()==HH_CHAT_CONNECTING);CHECK(!hh_chat_cloud_begin(8));}
static void pcm_is_copied_split_and_precedes_submit(void){uint8_t pcm[2800];boot(true);ready();memset(pcm,0x45,sizeof(pcm));CHECK(hh_chat_cloud_begin(10));CHECK(hh_chat_cloud_pcm(10,pcm,sizeof(pcm)));CHECK(hh_chat_cloud_submit(10));memset(pcm,0,sizeof(pcm));CHECK(upload_count==0&&input_stops==0);drain();CHECK(uploaded_bytes==2800&&uploaded[0]==0x45&&uploaded[2799]==0x45);CHECK(input_stops==1);}
static void overflow_latches_cancel_and_late_pcm_is_rejected(void){uint8_t pcm[1280]={0};boot(true);ready();CHECK(hh_chat_cloud_begin(20));for(unsigned i=0;i<CLOUD_SLOTS+8;i++)hh_chat_cloud_pcm(20,pcm,sizeof(pcm));CHECK(!hh_chat_cloud_pcm(20,pcm,sizeof(pcm)));hh_chat_cloud_cancel(20);drain();CHECK(report_failed>=1&&report_quiet==1&&last_quiet==20);CHECK(!hh_chat_cloud_pcm(20,pcm,sizeof(pcm)));}
static void overflow_exposes_first_failure_without_session_or_credentials(void){uint8_t pcm[1280]={0};boot(true);ready();failure_notice_count=0;failure_notice[0]=0;CHECK(hh_chat_cloud_begin(777));for(unsigned i=0;i<CLOUD_SLOTS+8;i++)hh_chat_cloud_pcm(777,pcm,sizeof(pcm));CHECK(failure_notice_count==1);CHECK(strstr(failure_notice,"turn=777")&&strstr(failure_notice,"queued=380")&&strstr(failure_notice,"native=20000"));CHECK(strstr(failure_notice,"聊天上传队列已满"));CHECK(!strstr(failure_notice,"current-session")&&!strstr(failure_notice,"00000000000000000000"));drain();CHECK(failure_notice_count==1);}
static void cancel_without_begin_still_quiesces(void){boot(false);hh_chat_cloud_cancel(30);drain();CHECK(report_quiet==1&&last_quiet==30);}
static AI_BIZ_RECV_CB audio_receiver(void){for(unsigned i=0;i<AI_BIZ_MAX_NUM;i++)if(receivers.arc[i].type==AI_PT_AUDIO)return receivers.arc[i].cb;return NULL;}
static void audio_chunk(int stream,const char*sid){uint8_t b[20]={0};AI_BIZ_ATTR_INFO_T a={0};AI_BIZ_HEAD_INFO_T h={0};a.type=AI_PT_AUDIO;a.flag=AI_HAS_ATTR;a.value.audio.base.codec_type=AUDIO_CODEC_MP3;a.value.audio.base.sample_rate=16000;a.value.audio.base.channels=1;a.value.audio.base.bit_depth=16;a.value.audio.option.session_id_list=(char*)sid;h.stream_flag=stream;h.len=sizeof(b);if(audio_receiver())audio_receiver()(&a,&h,b,NULL);}
static void start_response(uint32_t t){CHECK(hh_chat_cloud_begin(t));CHECK(hh_chat_cloud_submit(t));drain();audio_chunk(AI_STREAM_START,session.sid);tick();}
static void fast_response_during_submit_stop_is_not_dropped(void){boot(true);ready();CHECK(hh_chat_cloud_begin(33));response_on_submit=true;CHECK(hh_chat_cloud_submit(33));drain();CHECK(sink_writes>0);}
static void stopped_without_eof_cannot_succeed(void){boot(true);ready();start_response(40);player_state=AI_PLAYER_STOPPED;tick();CHECK(report_quiet==0);audio_chunk(AI_STREAM_END,session.sid);drain();CHECK(report_quiet==1&&report_failed==0);}
static void playing_response_is_not_cancelled_by_wait_timer(void){boot(true);ready();start_response(190);clock_ms=45001;tick();CHECK(report_failed==0&&report_quiet==0&&player_state==AI_PLAYER_PLAYING);}
static void stale_idle_cancel_cannot_claim_next_generation(void){boot(true);ready();start_response(200);hh_chat_cloud_cancel(200);drain();CHECK(report_quiet==1&&last_quiet==200);hh_chat_cloud_cancel(200);CHECK(hh_chat_cloud_begin(201));hh_chat_cloud_cancel(200);hh_chat_cloud_cancel(201);drain();CHECK(report_quiet==2&&last_quiet==201);}
static void fresh_unbegun_cancel_still_cleans_after_completed_generation(void){boot(true);ready();hh_chat_cloud_cancel(210);drain();CHECK(report_quiet==1&&last_quiet==210);hh_chat_cloud_cancel(210);hh_chat_cloud_cancel(211);drain();CHECK(report_quiet==2&&last_quiet==211);}
static void transient_wifi_loss_restores_only_verified_ready_state(void){boot(true);ready();link_up=false;tick();CHECK(hh_chat_cloud_availability()==HH_CHAT_OFFLINE);link_up=true;tick();CHECK(hh_chat_cloud_availability()==HH_CHAT_READY);link_up=false;tick();ai_ready=false;link_up=true;tick();CHECK(hh_chat_cloud_availability()!=HH_CHAT_READY);ai_ready=true;cloud.session_ready=false;tick();CHECK(hh_chat_cloud_availability()!=HH_CHAT_READY);}
static void natural_cleanup_seals_new_callbacks_before_player_stop(void){boot(true);ready();start_response(400);audio_chunk(AI_STREAM_END,session.sid);player_state=AI_PLAYER_STOPPED;callback_on_player_stop=true;drain();CHECK(callback_captured_during_stop==0);CHECK(cloud.callbacks==0&&report_quiet==1&&last_quiet==400);}
static void natural_cleanup_waits_for_entered_callback_before_quiescence(void){boot(true);ready();start_response(410);CHECK(callback_enter()==410);audio_chunk(AI_STREAM_END,session.sid);player_state=AI_PLAYER_STOPPED;drain();CHECK(report_quiet==0&&cloud.turn==410);CHECK(callback_enter()==0);audio_chunk(AI_STREAM_START,session.sid);CHECK(cloud.count==0);callback_leave();drain();CHECK(report_quiet==1&&last_quiet==410&&cloud.callbacks==0);}
static void timed_out_hidden_sdk_stop_cannot_leave_uploading_after_quiescence(void){uint8_t p[640]={0};boot(true);ready();hidden_stop_delayed=true;CHECK(hh_chat_cloud_begin(420));CHECK(hh_chat_cloud_pcm(420,p,sizeof(p)));CHECK(hh_chat_cloud_submit(420));drain();hh_chat_cloud_cancel(420);drain();CHECK(report_quiet==1);CHECK(hidden_input_starts==0&&!hidden_input_active&&!direct_input_active);}
static void synchronous_end_error_releases_local_owner_without_claiming_remote_end(void){boot(true);ready();CHECK(hh_chat_cloud_begin(430));sync_end_fails=session_create_fails=true;CHECK(hh_chat_cloud_submit(430));drain();CHECK(report_failed>=1&&report_quiet==1&&cloud.turn==0);CHECK(direct_input_active&&hidden_input_starts==0&&!hidden_input_active);CHECK(hh_chat_cloud_availability()!=HH_CHAT_READY&&!hh_chat_cloud_begin(431));}
static void synchronous_start_error_releases_local_owner_and_blocks_new_chat(void){boot(true);ready();sync_start_fails=session_create_fails=true;CHECK(hh_chat_cloud_begin(440));drain();CHECK(report_failed>=1&&report_quiet==1&&cloud.turn==0);CHECK(hidden_input_starts==0&&!hidden_input_active&&hh_chat_cloud_availability()!=HH_CHAT_READY);}
static void synchronous_upload_error_releases_local_owner_and_blocks_new_chat(void){uint8_t p[640]={0};boot(true);ready();sync_upload_fails=session_create_fails=true;CHECK(hh_chat_cloud_begin(450));CHECK(hh_chat_cloud_pcm(450,p,sizeof(p)));drain();CHECK(report_failed>=1&&report_quiet==1&&cloud.turn==0);CHECK(hidden_input_starts==0&&!hidden_input_active&&hh_chat_cloud_availability()!=HH_CHAT_READY);}
static void sink_error_reports_failure_and_stop_failure_keeps_owner(void){boot(true);ready();sink_fails=true;stop_fails=true;start_response(50);drain();CHECK(report_failed>=1&&report_quiet==0);stop_fails=false;drain();CHECK(report_quiet==1);}
static void failed_session_recovers_after_completion_relative_backoff(void){
    boot(true);session_failures_remaining=1;session_create_delay_ms=6000;ready();
    CHECK(session_create_calls==1&&clock_ms==6000&&hh_chat_cloud_availability()!=HH_CHAT_READY);
    clock_ms+=4999;tick();CHECK(session_create_calls==1);
    clock_ms++;tick();CHECK(session_create_calls==2&&hh_chat_cloud_availability()==HH_CHAT_READY);
    CHECK(auth_notice_count==1&&session_notice_count==2);
    CHECK(!strcmp(session_notice,"HH chat session rc=0 attempt=2 sid_len=15"));
}
static void repeated_run_respects_three_attempt_budget_and_backoff(void){
    boot(true);session_create_fails=true;ready();CHECK(session_create_calls==1);
    for(unsigned i=0;i<8;i++){run_cb(NULL);tick();}CHECK(session_create_calls==1);
    clock_ms=4999;run_cb(NULL);tick();CHECK(session_create_calls==1);
    clock_ms=5000;tick();CHECK(session_create_calls==2);
    clock_ms=10000;tick();CHECK(session_create_calls==3);
    for(unsigned i=0;i<8;i++){clock_ms+=5000;run_cb(NULL);tick();}
    CHECK(session_create_calls==3&&hh_chat_cloud_availability()!=HH_CHAT_READY&&!hh_chat_cloud_begin(500));
}
static void failed_session_waits_for_all_network_layers(void){
    boot(true);session_create_fails=true;ready();CHECK(session_create_calls==1);clock_ms=5000;
    link_up=false;tick();CHECK(session_create_calls==1);
    tuya_event_msg_t down={TUYA_EVENT_MQTT_DISCONNECT};iot_event(NULL,&down);link_up=true;tick();CHECK(session_create_calls==1);
    ai_ready=false;tuya_event_msg_t up={TUYA_EVENT_MQTT_CONNECTED};iot_event(NULL,&up);run_cb(NULL);tick();CHECK(session_create_calls==1);
    session_create_fails=false;ai_ready=true;tick();CHECK(session_create_calls==2&&hh_chat_cloud_availability()==HH_CHAT_READY);
}
static void actual_transport_reconnect_resets_exhausted_budget(void){
    boot(true);session_create_fails=true;ready();clock_ms=5000;tick();clock_ms=10000;tick();
    CHECK(session_create_calls==3);clock_ms=15000;tick();CHECK(session_create_calls==3);
    ai_ready=false;tick();CHECK(session_create_calls==3);
    session_create_fails=false;ai_ready=true;tick();
    CHECK(session_create_calls==4&&hh_chat_cloud_availability()==HH_CHAT_READY);
}
static void established_session_ignores_duplicate_run_without_recreation(void){
    boot(true);ready();CHECK(session_create_calls==1);
    for(unsigned i=0;i<8;i++){run_cb(NULL);tick();}
    CHECK(session_create_calls==1&&hh_chat_cloud_availability()==HH_CHAT_READY);
}
static void first_session_requires_client_run_event(void){
    boot(true);link_up=true;tuya_event_msg_t up={TUYA_EVENT_MQTT_CONNECTED};iot_event(NULL,&up);tick();
    ai_ready=true;tick();CHECK(session_create_calls==0&&hh_chat_cloud_availability()!=HH_CHAT_READY);
    run_cb(NULL);tick();CHECK(session_create_calls==1&&hh_chat_cloud_availability()==HH_CHAT_READY);
}
static void mqtt_disconnect_between_ticks_resets_exhausted_budget(void){
    boot(true);session_create_fails=true;ready();clock_ms=5000;tick();clock_ms=10000;tick();CHECK(session_create_calls==3);
    tuya_event_msg_t down={TUYA_EVENT_MQTT_DISCONNECT},up={TUYA_EVENT_MQTT_CONNECTED};iot_event(NULL,&down);iot_event(NULL,&up);
    session_create_fails=false;tick();CHECK(session_create_calls==4&&hh_chat_cloud_availability()==HH_CHAT_READY);
}
static void pending_cancellation_blocks_session_recovery(void){
    boot(true);session_create_fails=true;ready();CHECK(session_create_calls==1);
    quiesce_fails=true;hh_chat_cloud_cancel(510);clock_ms=5000;tick();
    CHECK(session_create_calls==1&&cloud.turn==510&&cloud.cancel);
}
static void active_turn_never_creates_session_on_duplicate_run(void){
    boot(true);ready();CHECK(hh_chat_cloud_begin(520));run_cb(NULL);clock_ms=5000;tick();
    CHECK(session_create_calls==1&&cloud.turn==520);
}
static void old_session_and_eid_cannot_end_current_turn(void){boot(true);ready();start_response(60);hh_chat_cloud_cancel(60);drain();CHECK(report_quiet==1);start_response(61);unsigned before=sink_writes;audio_chunk(AI_STREAM_ONE,"old-session");AI_EVENT_ATTR_T attr={0};AI_EVENT_HEAD_T head={AI_EVENT_CHAT_BREAK};strcpy(attr.event_id,"old-eid");strcpy(attr.session_id,"old-session");if(receivers.event_cb)receivers.event_cb(&attr,&head,NULL);tick();CHECK(sink_writes==before&&report_quiet==1);hh_chat_cloud_cancel(60);tick();CHECK(report_quiet==1);}

static unsigned count_event(HH_EventType type){unsigned n=0;for(unsigned i=0;i<event_count;i++)if(events[i].type==type)n++;return n;}
static const HH_Event *last_event(HH_EventType type){for(unsigned i=event_count;i;i--)if(events[i-1].type==type)return &events[i-1];return NULL;}
static void submit_text_turn(uint32_t turn){boot(true);ready();CHECK(hh_chat_cloud_begin(turn));CHECK(hh_chat_cloud_submit(turn));drain();}
static void text_message(const char *kind,const char *body,const char *request,bool native_json){
    char json[1024];snprintf(json,sizeof(json),"{\"bizType\":\"%s\",\"data\":%s%s%s%s}",kind,body,request?",\"requestId\":\"":"",request?request:"",request?"\"":"");
    AI_BIZ_HEAD_INFO_T h={0};h.len=(unsigned)strlen(json);h.data_type=native_json?AI_BIZ_DATA_TYPE_JSON:AI_BIZ_DATA_TYPE_BYTE;
    cJSON *root=native_json?cJSON_Parse(json):NULL;
    receive_text(NULL,&h,native_json?(void*)root:(void*)json,NULL);
    if(root)cJSON_Delete(root);
}
static void nlg_tags_are_queued_as_compact_reply_emotions(void){
    submit_text_turn(600);unsigned allocations=psram_allocs;
    text_message("NLG","{\"content\":\"hello\",\"tags\":[\"U+1F642\"]}",session.eid,true);
    CHECK(count_event(EV_CHAT_EMOTION)==0);drain();
    const HH_Event *e=last_event(EV_CHAT_EMOTION);CHECK(e&&e->action.chat_turn==600&&e->emotion==HH_EMOTION_HAPPY);
    CHECK(e->text[0]==0&&psram_allocs==allocations);CHECK(count_event(EV_CHAT_TEXT)==1);
}
static void changed_emotions_emit_once_and_neutral_resets_a_reply(void){
    submit_text_turn(610);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,false);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1);
    text_message("NLG","{\"tags\":[\"U+1F620\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==2&&last_event(EV_CHAT_EMOTION)->emotion==HH_EMOTION_ANGRY);
    text_message("NLG","{\"tags\":[\"U+1F636\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==3&&last_event(EV_CHAT_EMOTION)->emotion==HH_EMOTION_NEUTRAL);
}
static void emotion_requires_exact_nonempty_request_id(void){
    submit_text_turn(620);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",NULL,true);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}","",true);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}","old-eid",true);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    AI_BIZ_HEAD_INFO_T h={.data_type=AI_BIZ_DATA_TYPE_JSON};cJSON *j=cJSON_Parse("{\"bizType\":\"NLG\",\"requestId\":2,\"data\":{\"tags\":[\"U+1F642\"]}}");receive_text(NULL,&h,j,NULL);cJSON_Delete(j);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1);
}
static void malformed_unknown_or_asr_tags_do_not_invent_reply_emotion(void){
    submit_text_turn(630);
    const char *invalid[]={"{}","{\"tags\":[]}","{\"tags\":\"U+1F642\"}","{\"tags\":{\"0\":\"U+1F642\"}}","{\"tags\":[null]}","{\"tags\":[2]}","{\"tags\":[\"HAPPY\"]}","{\"tags\":[\"U+1F642junk\"]}","{\"tags\":[\"U+0000\",\"U+1F642\"]}"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)text_message("NLG",invalid[i],session.eid,true);
    text_message("ASR","{\"text\":\"happy\",\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    text_message("NLG","{\"content\":\"I am happy\"}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==0&&count_event(EV_CHAT_TEXT)==2);
}
static void skill_emotion_supports_official_code_and_parallel_arrays(void){
    submit_text_turn(640);
    text_message("SKILL","{\"code\":\"emo\",\"skillContent\":{\"emotion\":[\"SAD\"],\"text\":[\"U+1F614\"]}}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1&&last_event(EV_CHAT_EMOTION)->emotion==HH_EMOTION_SAD);
    text_message("SKILL","{\"code\":\"llm_emo\",\"skillContent\":{\"emotion\":[\"LAUGHING\"],\"text\":[\"U+1F606\"]}}",session.eid,false);drain();CHECK(count_event(EV_CHAT_EMOTION)==2&&last_event(EV_CHAT_EMOTION)->emotion==HH_EMOTION_JOY);
    text_message("SKILL","{\"code\":\"music\",\"skillContent\":{\"emotion\":[\"ANGRY\"],\"text\":[\"U+1F620\"]}}",session.eid,true);
    text_message("SKILL","{\"code\":\"emo\",\"skillContent\":{\"emotion\":[\"ANGRY\"],\"text\":[]}}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==2&&count_event(EV_CHAT_TEXT)==0);
}
static void cancel_on_tags_read(const char *key){if(!strcmp(key,"tags")){json_read_hook=NULL;hh_chat_cloud_cancel(650);}}
static void cancellation_during_parse_and_queued_old_turns_are_discarded(void){
    submit_text_turn(650);json_read_hook=cancel_on_tags_read;
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);json_read_hook=NULL;drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    CHECK(hh_chat_cloud_begin(651));CHECK(hh_chat_cloud_submit(651));drain();
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);hh_chat_cloud_cancel(651);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    CHECK(hh_chat_cloud_begin(652));CHECK(hh_chat_cloud_submit(652));drain();
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1&&last_event(EV_CHAT_EMOTION)->action.chat_turn==652);
}
static void disconnect_clears_queued_emotions_and_next_generation_dedupe(void){
    submit_text_turn(660);text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);
    tuya_event_msg_t e={TUYA_EVENT_MQTT_DISCONNECT};iot_event(NULL,&e);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    e.id=TUYA_EVENT_MQTT_CONNECTED;iot_event(NULL,&e);ready();CHECK(hh_chat_cloud_begin(661));CHECK(hh_chat_cloud_submit(661));drain();
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1&&last_event(EV_CHAT_EMOTION)->action.chat_turn==661);
}
static void existing_asr_and_nlg_text_compatibility_is_preserved(void){
    submit_text_turn(670);text_message("ASR","{\"text\":\"heard\"}",NULL,false);text_message("NLG","{\"content\":\"reply\"}",NULL,true);text_message("NLG","{\"content\":\"old\"}","stale",true);drain();
    CHECK(count_event(EV_CHAT_TEXT)==2&&count_event(EV_CHAT_EMOTION)==0);const HH_Event *e=last_event(EV_CHAT_TEXT);CHECK(e&&!e->chat_is_user_text&&!strcmp(e->text,"reply"));
}

static void full_fifo_drops_only_cosmetic_update_and_allows_retry(void){
    submit_text_turn(680);
    for(unsigned i=0;i<CLOUD_DATA_SLOTS;i++)text_message("NLG","{\"content\":\"stream\"}",session.eid,true);
    CHECK(cloud.count==CLOUD_DATA_SLOTS);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);
    CHECK(!cloud.cancel&&!cloud.failed);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1);
}
static void prior_request_and_taken_old_command_cannot_repaint_new_generation(void){
    submit_text_turn(690);char old_id[AI_UUID_V4_LEN+1];strcpy(old_id,session.eid);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);CloudItem old;CHECK(take(&old));
    hh_chat_cloud_cancel(690);drain();CHECK(hh_chat_cloud_begin(691));CHECK(hh_chat_cloud_submit(691));drain();
    execute(&old);text_message("NLG","{\"tags\":[\"U+1F642\"]}",old_id,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==0);
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();CHECK(count_event(EV_CHAT_EMOTION)==1&&last_event(EV_CHAT_EMOTION)->action.chat_turn==691);
}
static void full_app_queue_drops_emotion_without_blocking_audio(void){
    submit_text_turn(700);app_post_calls=app_post_wait=0;reject_app_post=true;
    text_message("NLG","{\"tags\":[\"U+1F642\"]}",session.eid,true);drain();
    bool valid=app_post_calls==1&&app_post_wait==0&&count_event(EV_CHAT_EMOTION)==0&&
               !cloud.failed&&!cloud.cancel&&cloud.turn==700;
    reject_app_post=false;CHECK(valid);
    hh_chat_cloud_cancel(700);drain();CHECK(last_quiet==700);
}
static void legal_six_second_upload_stall_keeps_every_pcm_byte_and_timestamp(void){
    uint8_t first[640];memset(first,0x5a,sizeof(first));boot(true);ready();
    CHECK(hh_chat_cloud_begin(800));CHECK(hh_chat_cloud_pcm(800,first,sizeof(first)));
    stall_turn=800;stall_upload_frames=300;tick();
    CHECK(clock_ms==6000&&stall_rejected==0&&!cloud.cancel&&!cloud.failed);
    CHECK(hh_chat_cloud_submit(800));drain();
    CHECK(upload_count==301&&uploaded_bytes==301u*640u&&input_stops==1&&report_failed==0);
    for(unsigned i=0;i<301;i++){
        CHECK(uploaded_length[i]==640&&uploaded_timestamp[i]==(i?20u*i:0));
        for(unsigned b=0;b<640;b++)CHECK(uploaded[i*640u+b]==(i?(uint8_t)i:0x5a));
    }
    CHECK(psram_allocs==1);hh_chat_cloud_cancel(800);drain();
    CHECK(report_quiet==1&&budget_notice_count==1);
    CHECK(strstr(budget_notice,"pcm_max_ms=6000")&&strstr(budget_notice,"peak=300"));
}
static void legal_six_second_begin_stall_keeps_microphone_backlog(void){
    boot(true);ready();CHECK(hh_chat_cloud_begin(801));stall_turn=801;stall_start_frames=300;tick();
    CHECK(stall_rejected==0&&!cloud.cancel);CHECK(hh_chat_cloud_submit(801));drain();
    CHECK(upload_count==300&&uploaded_bytes==300u*640u&&report_failed==0);
    for(unsigned i=0;i<300;i++)CHECK(uploaded_timestamp[i]==20u*(i+1)&&uploaded[i*640u]==(uint8_t)(i+1));
    hh_chat_cloud_cancel(801);drain();CHECK(budget_notice_count==1&&strstr(budget_notice,"begin_max_ms=6000"));
}
static void saturated_pcm_storage_keeps_a_submit_control_slot(void){
    uint8_t pcm[640]={0};boot(true);ready();CHECK(hh_chat_cloud_begin(802));
    lock_cloud();while(put_locked(C_PCM,802,pcm,sizeof(pcm),0)){}unlock_cloud();
    CHECK(hh_chat_cloud_submit(802));CHECK(!cloud.cancel&&!cloud.failed);
    hh_chat_cloud_cancel(802);drain();CHECK(report_quiet==1);
}
static void cancel_during_blocked_upload_rejects_backlog_without_reacquiring_owner(void){
    uint8_t pcm[640]={0};boot(true);ready();CHECK(hh_chat_cloud_begin(803));CHECK(hh_chat_cloud_pcm(803,pcm,sizeof(pcm)));
    stall_turn=803;stall_upload_frames=300;stall_cancel_at=150;tick();drain();
    CHECK(stall_rejected==151&&upload_count==1&&report_quiet==1&&last_quiet==803&&cloud.turn==0);
    CHECK(!hh_chat_cloud_pcm(803,pcm,sizeof(pcm))&&!hidden_input_active&&!direct_input_active);
}
static void ai_reply_full_volume_uses_only_player_handle_before_ready(void){boot(true);ready();CHECK(reply_volume_calls==1&&reply_volume==100);CHECK(hh_chat_cloud_availability()==HH_CHAT_READY);}
static void failed_reply_gain_setup_never_claims_ready(void){boot(true);reply_volume_fails=true;ready();CHECK(reply_volume_calls==1);CHECK(hh_chat_cloud_availability()!=HH_CHAT_READY);}
static void private_wifi_fixture(void){wifi_settings_present=true;wifi_settings.fallback_ssid="private-test-network";wifi_settings.fallback_password="private-test-password";}
static void no_explicit_wifi_does_not_close_an_existing_connection(void){prepare_boot(true);link_up=true;hh_chat_cloud_start();cloud_initialize();CHECK(!wifi_closes&&!wifi_barriers&&!wifi_sets&&iot_starts==1);CHECK(license_reads==1&&!cli_uart_calls&&!full_cli_calls);}
static void private_wifi_waits_for_queued_disconnect_even_when_link_initially_down(void){prepare_boot(true);private_wifi_fixture();wifi_close_delay=80;hh_chat_cloud_start();cloud_initialize();CHECK(wifi_closes==1&&wifi_barriers==2&&wifi_sets==1&&!wifi_set_too_early);CHECK(clock_ms>=80&&!strcmp(selected_wifi.ssid,wifi_settings.fallback_ssid)&&!strcmp(selected_wifi.pswd,wifi_settings.fallback_password));CHECK(wifi_notice_count==1&&strstr(wifi_notice,"source=private selected=1")&&strstr(wifi_notice,"set_rc=0"));CHECK(!strstr(wifi_notice,selected_wifi.ssid)&&!strstr(wifi_notice,selected_wifi.pswd));CHECK(auth_notice_count==1&&license_reads==1&&!cli_uart_calls&&iot_starts==1);}
static void tf_wifi_has_priority_and_waits_for_up_connection_to_go_down(void){prepare_boot(true);private_wifi_fixture();strcpy(hh_device.wifi_ssid,"tf-test-network");strcpy(hh_device.wifi_password,"tf-test-password");link_up=true;hh_chat_cloud_start();cloud_initialize();CHECK(wifi_closes==1&&wifi_sets==1&&!wifi_set_too_early);CHECK(!strcmp(selected_wifi.ssid,hh_device.wifi_ssid)&&!strcmp(selected_wifi.pswd,hh_device.wifi_password));CHECK(strstr(wifi_notice,"source=tf selected=1")&&strstr(wifi_notice,"set_rc=0"));}
static void wifi_close_barrier_status_and_set_failures_do_not_claim_runtime_ready(void){for(unsigned i=0;i<4;i++){prepare_boot(true);private_wifi_fixture();link_up=true;if(i==0)wifi_close_rc=-31;if(i==1)wifi_barrier_rc=-32;if(i==2)wifi_status_rc=-33;if(i==3)wifi_set_rc=-34;hh_chat_cloud_start();cloud_initialize();CHECK(wifi_closes==(i==1?0:1)&&wifi_sets==(i==3?1:0));CHECK(!cloud.runtime_ready&&!iot_starts&&hh_chat_cloud_availability()==HH_CHAT_OFFLINE);CHECK(wifi_notice_count==1&&!strstr(wifi_notice,wifi_settings.fallback_ssid)&&!strstr(wifi_notice,wifi_settings.fallback_password));CHECK(license_reads==1&&!cli_uart_calls&&!full_cli_calls);}}
static void wifi_workqueue_timeout_is_bounded_and_late_barrier_has_no_credentials(void){prepare_boot(true);private_wifi_fixture();wifi_barrier_stalled=true;hh_chat_cloud_start();cloud_initialize();CHECK(wifi_closes==1&&wifi_barriers==2&&!wifi_sets&&!cloud.runtime_ready&&!iot_starts);CHECK(clock_ms>=3000&&clock_ms<=3020);CHECK(wifi_barrier_callback!=NULL&&wifi_barrier_data==NULL);wifi_barrier_callback(NULL);CHECK(!wifi_sets&&!cloud.runtime_ready);CHECK(strstr(wifi_notice,"set_rc=-99")&&!strstr(wifi_notice,"private-test-network"));}
static void invalid_private_wifi_is_rejected_without_disconnect_or_set(void){prepare_boot(true);private_wifi_fixture();char long_ssid[34];memset(long_ssid,'x',33);long_ssid[33]=0;wifi_settings.fallback_ssid=long_ssid;hh_chat_cloud_start();cloud_initialize();CHECK(!wifi_closes&&!wifi_barriers&&!wifi_sets&&!cloud.runtime_ready&&!iot_starts);CHECK(wifi_notice_count==1&&strstr(wifi_notice,"source=private selected=0"));}
static void private_wifi_is_applied_each_boot_without_license_overwrite_or_cli(void){for(unsigned i=0;i<2;i++){prepare_boot(true);private_wifi_fixture();hh_chat_cloud_start();cloud_initialize();CHECK(wifi_sets==1&&wifi_closes==1&&license_reads==1&&!cli_uart_calls&&!cli_auth_calls&&!full_cli_calls);CHECK(iot_starts==1&&cloud.authorized);}}
static void wifi_network_init_failure_skips_disconnect_and_never_claims_ready(void){prepare_boot(true);private_wifi_fixture();wifi_net_rc=-35;hh_chat_cloud_start();cloud_initialize();CHECK(net_calls==1&&!wifi_closes&&!wifi_barriers&&!wifi_sets&&!iot_starts&&!cloud.runtime_ready);CHECK(hh_chat_cloud_availability()==HH_CHAT_OFFLINE&&strstr(wifi_notice,"net_rc=-35"));CHECK(license_reads==1&&!cli_uart_calls&&!full_cli_calls);}
static void initial_ready_state_auto_connect_is_fenced_before_close(void){prepare_boot(true);private_wifi_fixture();hh_chat_cloud_start();cloud_initialize();CHECK(wifi_barriers==2&&wifi_closes==1&&!wifi_close_before_fence&&wifi_sets==1&&!wifi_set_too_early);}
int main(void){RUN(initial_ready_state_auto_connect_is_fenced_before_close);RUN(wifi_network_init_failure_skips_disconnect_and_never_claims_ready);RUN(no_explicit_wifi_does_not_close_an_existing_connection);RUN(private_wifi_waits_for_queued_disconnect_even_when_link_initially_down);RUN(tf_wifi_has_priority_and_waits_for_up_connection_to_go_down);RUN(wifi_close_barrier_status_and_set_failures_do_not_claim_runtime_ready);RUN(wifi_workqueue_timeout_is_bounded_and_late_barrier_has_no_credentials);RUN(invalid_private_wifi_is_rejected_without_disconnect_or_set);RUN(private_wifi_is_applied_each_boot_without_license_overwrite_or_cli);RUN(ai_reply_full_volume_uses_only_player_handle_before_ready);RUN(failed_reply_gain_setup_never_claims_ready);RUN(legal_six_second_upload_stall_keeps_every_pcm_byte_and_timestamp);RUN(legal_six_second_begin_stall_keeps_microphone_backlog);RUN(saturated_pcm_storage_keeps_a_submit_control_slot);RUN(cancel_during_blocked_upload_rejects_backlog_without_reacquiring_owner);RUN(full_app_queue_drops_emotion_without_blocking_audio);RUN(full_fifo_drops_only_cosmetic_update_and_allows_retry);RUN(prior_request_and_taken_old_command_cannot_repaint_new_generation);RUN(nlg_tags_are_queued_as_compact_reply_emotions);RUN(changed_emotions_emit_once_and_neutral_resets_a_reply);RUN(emotion_requires_exact_nonempty_request_id);RUN(malformed_unknown_or_asr_tags_do_not_invent_reply_emotion);RUN(skill_emotion_supports_official_code_and_parallel_arrays);RUN(cancellation_during_parse_and_queued_old_turns_are_discarded);RUN(disconnect_clears_queued_emotions_and_next_generation_dedupe);RUN(existing_asr_and_nlg_text_compatibility_is_preserved);RUN(first_session_requires_client_run_event);RUN(mqtt_disconnect_between_ticks_resets_exhausted_budget);RUN(failed_session_recovers_after_completion_relative_backoff);RUN(repeated_run_respects_three_attempt_budget_and_backoff);RUN(failed_session_waits_for_all_network_layers);RUN(actual_transport_reconnect_resets_exhausted_budget);RUN(established_session_ignores_duplicate_run_without_recreation);RUN(pending_cancellation_blocks_session_recovery);RUN(active_turn_never_creates_session_on_duplicate_run);RUN(missing_license_opens_only_uart0_authorization_commands);RUN(valid_license_never_opens_overwrite_channel_even_without_pid);RUN(invalid_license_format_allows_authorization_channel);RUN(kv_failure_never_reads_license_or_opens_uart);RUN(disabled_chat_never_opens_authorization_channel);RUN(cli_errors_do_not_start_network_or_block_local_quiescence);RUN(kv_parameters_match_installed_official_example);RUN(successful_authorization_notice_contains_only_result_and_lengths);RUN(failed_authorization_notice_does_not_inspect_returned_fields);RUN(unauthorized_never_initializes_iot_but_keeps_wifi);RUN(unauthorized_empty_config_skips_network_and_native_stacks);RUN(disabled_empty_config_skips_authorization_runtime);RUN(authorized_empty_wifi_still_supports_ble_ap);RUN(unauthorized_http_config_keeps_runtime_without_ssid);RUN(audio_fifo_allocation_failure_cannot_claim_ready);RUN(wifi_and_mqtt_are_not_chat_ready);RUN(lost_wifi_revokes_ready_before_capture);RUN(ai_transport_loss_revokes_ready_even_with_wifi_and_mqtt);RUN(pcm_is_copied_split_and_precedes_submit);RUN(overflow_latches_cancel_and_late_pcm_is_rejected);RUN(overflow_exposes_first_failure_without_session_or_credentials);RUN(cancel_without_begin_still_quiesces);RUN(fast_response_during_submit_stop_is_not_dropped);RUN(stopped_without_eof_cannot_succeed);RUN(playing_response_is_not_cancelled_by_wait_timer);RUN(stale_idle_cancel_cannot_claim_next_generation);RUN(fresh_unbegun_cancel_still_cleans_after_completed_generation);RUN(transient_wifi_loss_restores_only_verified_ready_state);RUN(natural_cleanup_seals_new_callbacks_before_player_stop);RUN(natural_cleanup_waits_for_entered_callback_before_quiescence);RUN(timed_out_hidden_sdk_stop_cannot_leave_uploading_after_quiescence);RUN(synchronous_end_error_releases_local_owner_without_claiming_remote_end);RUN(synchronous_start_error_releases_local_owner_and_blocks_new_chat);RUN(synchronous_upload_error_releases_local_owner_and_blocks_new_chat);RUN(sink_error_reports_failure_and_stop_failure_keeps_owner);RUN(old_session_and_eid_cannot_end_current_turn);printf("%d failure(s)\n",failures);return failures?1:0;}
