#define HOMEHUB_PLATFORM_H
#include "homehub_core.h"
#include "tal_api.h"
#include "homehub_cloud_config.h"
#include "homehub_cloud_http.h"
#include "tal_time_service.h"
#include "tkl_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HH_ROOT "/sdcard/homehub"
#define HH_WAV_MAX (960000u+44u)
typedef struct{int type;HH_Message message;}HH_Event;
#define EV_REMOTE_MESSAGE 1
static unsigned failures,allocs,threads,lock_depth,polls,audio_gets,patches,posts,writes,events,light_ticks,frees;
static bool config_present,network_up,write_ok,wav_ok,http_cancel,cancel_binary_only;
static int patch_status,post_status,binary_status,time_rc,sync_rc;
static POSIX_TM_S utc;
static HH_CloudSettings settings;
static HH_Message received[16];
static uint32_t now_ms;
static const char *rows_a,*rows_b;
static char poll_path[1024],last_patch_path[128],last_post[100],order[256];
static unsigned order_count;
static unsigned post_sent,patch_sent;
static char sent_posts[40][64];
static uint8_t wave[60];
static uint8_t disk[60];static uint32_t disk_bytes;
static char disk_path[HH_PATH_MAX+1];
static bool legacy_disk_exists;static uint8_t legacy_disk[60];
static bool disk_exists,read_ok,replace_rename_failure;
static int stat_result;static unsigned stat_calls,read_calls;
MUTEX_HANDLE hh_fs_mutex=(void*)3;
#define CHECK(v) do{if(!(v)){printf("FAIL %s:%d %s\n",__func__,__LINE__,#v);failures++;return;}}while(0)
#define RUN(f) do{unsigned before=failures;f();printf("%s %s\n",before==failures?"PASS":"FAIL",#f);}while(0)
static void step_order(char c){if(order_count<sizeof(order)-1){order[order_count++]=c;order[order_count]=0;}}
const HH_CloudSettings *hh_cloud_settings(void){return config_present?&settings:NULL;}
int tal_mutex_create_init(MUTEX_HANDLE *m){*m=(void*)1;return 0;}
int tal_mutex_lock(MUTEX_HANDLE m){(void)m;lock_depth++;return 0;}
int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;lock_depth--;return 0;}
void *tkl_system_psram_malloc(size_t n){allocs++;return calloc(1,n);}
void tkl_system_psram_free(void *p){free(p);}
int tal_thread_create_and_start(THREAD_HANDLE*t,void*a,void*b,void(*fn)(void*),void*arg,THREAD_CFG_T*c){(void)a;(void)b;(void)fn;(void)arg;if(c->psram_mode!=1||c->stackDepth>16384)failures++;*t=(void*)2;threads++;return 0;}
uint64_t tal_system_get_millisecond(void){return now_ms;}
void tal_system_sleep(unsigned ms){now_ms+=ms;}
int tal_time_check_time_sync(void){return sync_rc;}
int tal_time_get(POSIX_TM_S *tm){*tm=utc;return time_rc;}
bool hh_chat_network_ready(void){return network_up;}
void hh_tuya_light_tick(void){light_ticks++;step_order('L');}
bool hh_wav_validate(const uint8_t*d,uint32_t n,uint32_t*pcm){(void)pcm;step_order('V');return wav_ok&&n==sizeof(wave)&&!memcmp(d,"RIFF",4);}
static bool current_path(const char *path){return disk_exists&&!strcmp(path,disk_path);}
static bool legacy_path(const char *path){return legacy_disk_exists&&!strcmp(path,HH_ROOT"/messages/sb_42.wav");}
int32_t tkl_fs_is_exist(const char *path,BOOL_T *exists){stat_calls++;if(lock_depth!=1)failures++;*exists=current_path(path)||legacy_path(path);return stat_result==-99?(*exists?0:4):stat_result;}
bool hh_file_read(const char *path,uint8_t **bytes,uint32_t *size,uint32_t limit){read_calls++;if(lock_depth)failures++;bool old=legacy_path(path);uint32_t n=old?sizeof(legacy_disk):disk_bytes;if((!old&&!current_path(path))||!read_ok||n>limit)return false;*bytes=malloc(n);memcpy(*bytes,old?legacy_disk:disk,n);*size=n;return true;}
bool hh_file_write_atomic(const char*p,const uint8_t*d,uint32_t n){if(lock_depth||strncmp(p,HH_ROOT"/messages/",sizeof(HH_ROOT"/messages/")-1))failures++;writes++;step_order('W');if(current_path(p)&&replace_rename_failure){disk_exists=false;return false;}if(!write_ok||n>sizeof(disk))return false;memcpy(disk,d,n);snprintf(disk_path,sizeof(disk_path),"%s",p);disk_bytes=n;disk_exists=true;return true;}
void hh_post(const HH_Event *e){if(e->type!=EV_REMOTE_MESSAGE)failures++;if(events<16)received[events]=e->message;events++;step_order('P');}
static void reply(HH_CloudResponse*r,const uint8_t*d,size_t n,int status){memset(r,0,sizeof(*r));r->storage=malloc(n+1);if(n)memcpy(r->storage,d,n);r->storage[n]=0;r->body=r->storage;r->body_bytes=n;r->status=status;}
bool hh_cloud_http_request(const HH_CloudRequest*q,HH_CloudResponse*r){
    if(lock_depth||strcmp(q->host,settings.supabase_host)||q->ca_pem!=(settings.supabase_plain_http?NULL:settings.supabase_ca)||q->timeout_ms!=8000||
       q->plain_http!=settings.supabase_plain_http||q->port!=settings.supabase_port)failures++;
    bool binary=!strncmp(q->path,"/storage/v1/object/public/",26),apikey=false,bearer=false;
    for(size_t i=0;i<q->header_count;i++){if(!strcmp(q->headers[i].key,"apikey"))apikey=true;if(!strcmp(q->headers[i].key,"Authorization"))bearer=true;}
    if(binary){if(q->header_count)failures++;audio_gets++;step_order('A');reply(r,wave,sizeof(wave),binary_status);return !http_cancel&&!cancel_binary_only;}
    if(settings.supabase_plain_http){if(apikey||bearer)failures++;}
    else if(!apikey||bearer!=(!strncmp(settings.supabase_key,"eyJ",3)))failures++;
    if(!strcmp(q->method,"GET")){
        polls++;step_order('G');snprintf(poll_path,sizeof(poll_path),"%s",q->path);
        const char *body=strstr(q->path,"to_user=eq.A")?rows_a:rows_b;reply(r,(const uint8_t*)body,strlen(body),200);return !http_cancel;
    }
    if(!strcmp(q->method,"PATCH")){patches++;step_order('D');snprintf(last_patch_path,sizeof(last_patch_path),"%s",q->path);if(q->body_bytes!=18||memcmp(q->body,"{\"delivered\":true}",18))failures++;if(patch_status!=-1)patch_sent++;reply(r,NULL,0,patch_status);return !http_cancel;}
    if(!strcmp(q->method,"POST")){posts++;step_order('E');snprintf(last_post,sizeof(last_post),"%.*s",(int)q->body_bytes,(const char*)q->body);if(post_status!=-1){if(post_sent<40)snprintf(sent_posts[post_sent],sizeof(sent_posts[0]),"%s",last_post);post_sent++;}reply(r,NULL,0,post_status);return !http_cancel;}
    failures++;memset(r,0,sizeof(*r));return false;
}
void hh_cloud_http_free(HH_CloudResponse*r){if(r->storage){free(r->storage);frees++;}memset(r,0,sizeof(*r));}
#include "../../firmware/src/homehub_supabase.c"
static void reset(void){
    if(supa.jobs)free(supa.jobs);memset(&supa,0,sizeof(supa));
    allocs=threads=lock_depth=polls=audio_gets=patches=posts=writes=events=light_ticks=frees=order_count=now_ms=0;
    post_sent=patch_sent=0;memset(sent_posts,0,sizeof(sent_posts));
    config_present=network_up=write_ok=wav_ok=true;http_cancel=cancel_binary_only=false;patch_status=post_status=204;binary_status=200;time_rc=sync_rc=0;
    memset(&settings,0,sizeof(settings));settings.supabase_host="public.unit.invalid";settings.supabase_key="sb_publishable_synthetic_test";settings.supabase_ca="synthetic-ca";
    utc=(POSIX_TM_S){.tm_year=126,.tm_mon=10,.tm_mday=6,.tm_hour=20,.tm_min=10};
    rows_a=rows_b="[]";order[0]=poll_path[0]=last_patch_path[0]=last_post[0]=0;memset(wave,0,sizeof(wave));memcpy(wave,"RIFF",4);
    memset(disk,0,sizeof(disk));disk_bytes=0;disk_exists=replace_rename_failure=false;read_ok=true;stat_result=-99;stat_calls=read_calls=0;
    snprintf(disk_path,sizeof(disk_path),HH_ROOT"/messages/sb_42.wav");legacy_disk_exists=false;memset(legacy_disk,0,sizeof(legacy_disk));
}
static bool start(void){if(!hh_supabase_enabled())return false;hh_supabase_start();return threads==1;}
static void run(uint32_t now){now_ms=now;supabase_step(now);}
static void one_row(void){rows_a="[{\"id\":42,\"to_user\":\"A\",\"audio_url\":\"https://public.unit.invalid/storage/v1/object/public/voices/a.wav\"}]";}
static void disabled_is_allocation_free_and_start_is_once(void){reset();config_present=false;hh_supabase_start();CHECK(!hh_supabase_enabled()&&!allocs&&!threads);config_present=true;CHECK(start());hh_supabase_start();CHECK(threads==1&&allocs==1);}
static void valid_audio_is_durable_posted_before_explicit_receipt_patch(void){reset();CHECK(start());one_row();run(0);CHECK(events==1&&writes==1&&audio_gets==1&&!patches);CHECK(strchr(order,'G')&&strchr(order,'A')&&strchr(order,'G')<strchr(order,'A')&&strstr(order,"AVWP"));CHECK(!strcmp(received[0].id,"sb_42")&&!strcmp(received[0].audio_path,HH_ROOT"/messages/sb_42.wav"));CHECK(received[0].source==HH_REMOTE&&received[0].trigger==HH_ANY&&received[0].recipient==0);HH_Message m=received[0];hh_supabase_downloaded(&m);strcpy(m.id,"sb_99");run(100);CHECK(patches==1&&!strcmp(last_patch_path,"/rest/v1/messages?id=eq.42"));}
static void rejects_fractional_negative_unsafe_and_string_identifiers(void){const char *ids[]={"0","-1","1.5","9007199254740992","\"42\"","null","{}","1e99"};for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++){reset();CHECK(start());char row[256];snprintf(row,sizeof(row),"[{\"id\":%s,\"to_user\":\"A\",\"audio_url\":\"https://public.unit.invalid/storage/v1/object/public/v/a.wav\"}]",ids[i]);rows_a=row;run(0);CHECK(!audio_gets&&!events&&!patches);}}
static void origin_and_public_storage_path_are_exact(void){const char*urls[]={"http://public.unit.invalid/storage/v1/object/public/v/a.wav","https://public.unit.invalid.evil/storage/v1/object/public/v/a.wav","https://evil@public.unit.invalid/storage/v1/object/public/v/a.wav","https://public.unit.invalid/storage/v1/object/private/v/a.wav","https://public.unit.invalid/storage/v1/object/public/v/a.wav#x"};for(unsigned i=0;i<sizeof(urls)/sizeof(urls[0]);i++){reset();CHECK(start());char row[384];snprintf(row,sizeof(row),"[{\"id\":42,\"to_user\":\"A\",\"audio_url\":\"%s\"}]",urls[i]);rows_a=row;run(0);CHECK(!audio_gets&&!events&&!patches);}}
static void invalid_wav_failed_write_and_canceled_http_never_ack(void){reset();CHECK(start());one_row();wav_ok=false;run(0);CHECK(!writes&&!events&&!patches);reset();CHECK(start());one_row();write_ok=false;run(0);CHECK(writes==1&&!events&&!patches);reset();CHECK(start());one_row();http_cancel=true;run(0);CHECK(!events&&!patches&&frees==2);}
static void actual_utc_and_unsynchronized_clock_have_separate_filters(void){reset();CHECK(start());run(0);CHECK(strstr(poll_path,"play_after.lte.2026-11-06T20%3A10%3A00Z"));reset();CHECK(start());sync_rc=-1;run(0);CHECK(strstr(poll_path,"play_after=is.null")&&!strstr(poll_path,"play_after.lte"));reset();CHECK(start());time_rc=-1;run(0);CHECK(!strstr(poll_path,"play_after.lte"));}
static void recipient_b_polling_interval_and_light_priority_are_preserved(void){reset();CHECK(start());rows_b="[{\"id\":43,\"to_user\":\"B\",\"audio_url\":\"https://public.unit.invalid/storage/v1/object/public/v/b.wav\"}]";run(0);CHECK(events==1&&received[0].recipient==1&&polls==2&&order[0]=='L');run(29999);CHECK(polls==2);run(30000);CHECK(polls==4);}
static void network_down_does_not_publish_or_poll_and_receipt_recovers(void){reset();CHECK(start());one_row();run(0);HH_Message m=received[0];network_up=false;hh_supabase_downloaded(&m);run(100);CHECK(!patches&&polls==2);network_up=true;run(200);CHECK(patches==1);}
static void key_events_are_copied_best_effort_without_post_retry(void){reset();CHECK(start());HH_Action a={.type=HH_ACT_KEY_EVENT,.recipient=1,.trigger=HH_LEAVE};hh_supabase_action(&a);a.recipient=0;post_status=503;run(0);CHECK(posts==1&&strstr(last_post,"\"user_id\":\"B\"")&&strstr(last_post,"\"event\":\"out\""));run(100);CHECK(posts==1);}
static void legacy_jwt_headers_and_unrecognized_fields_do_not_invent_messages(void){reset();settings.supabase_key="eyJsynthetic.test.value";CHECK(start());one_row();run(0);CHECK(events==1);reset();CHECK(start());rows_a="[{\"id\":42,\"to_user\":\"C\",\"audio_url\":\"https://public.unit.invalid/storage/v1/object/public/v/a.wav\"}]";run(0);CHECK(!audio_gets&&!events);}
static void bounded_receipt_queue_rejects_bad_local_id_and_preserves_real_rows(void){reset();CHECK(start());HH_Message m={.source=HH_REMOTE};for(unsigned i=1;i<=20;i++){snprintf(m.id,sizeof(m.id),"sb_%u",i);hh_supabase_downloaded(&m);}run(0);CHECK(patches==16);strcpy(m.id,"sb_01");hh_supabase_downloaded(&m);strcpy(m.id,"sb_1/evil");hh_supabase_downloaded(&m);run(100);CHECK(patches==16);}
static void canceled_binary_response_is_freed_without_writing_or_acknowledging(void){reset();CHECK(start());one_row();cancel_binary_only=true;run(0);CHECK(audio_gets==1&&!writes&&!events&&!patches&&frees==3);}
static void failed_patch_is_recovered_only_by_repoll_and_durable_callback(void){reset();CHECK(start());one_row();run(0);hh_supabase_downloaded(&received[0]);patch_status=503;run(100);CHECK(patches==1);run(200);CHECK(patches==1);run(30000);CHECK(events==2&&!strcmp(received[0].id,received[1].id));hh_supabase_downloaded(&received[1]);patch_status=204;run(30100);CHECK(patches==2);}
static void missing_or_mistyped_fields_and_nonarray_root_are_not_messages(void){const char *rows[]={"[{}]","[{\"id\":42,\"to_user\":\"A\"}]","{\"messages\":[]}","[{\"id\":42,\"to_user\":\"A\",\"audio_url\":2}]"};for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);i++){reset();CHECK(start());rows_a=rows[i];run(0);CHECK(!audio_gets&&!events&&!patches);}}
static void repoll_preserves_good_wav_when_replacement_rename_would_destroy_it(void){
    reset();CHECK(start());one_row();run(0);CHECK(events==1&&disk_exists&&writes==1);HH_Message durable_candidate=received[0];
    replace_rename_failure=true;run(30000);
    CHECK(disk_exists&&disk_bytes==sizeof(wave)&&!memcmp(disk,wave,sizeof(wave))&&writes==1&&audio_gets==1&&events==2);
    CHECK(!patches);hh_supabase_downloaded(&durable_candidate);run(30100);CHECK(patches==1&&disk_exists);
}
static void corrupt_existing_and_failed_read_are_not_overwritten_or_acknowledged(void){
    reset();CHECK(start());one_row();disk_exists=true;disk_bytes=sizeof(disk);memcpy(disk,wave,sizeof(disk));disk[0]='X';run(0);CHECK(!audio_gets&&!writes&&!events&&!patches&&disk_exists&&disk[0]=='X');
    reset();CHECK(start());one_row();disk_exists=true;disk_bytes=sizeof(disk);memcpy(disk,wave,sizeof(disk));read_ok=false;run(0);CHECK(!audio_gets&&!writes&&!events&&!patches&&disk_exists);
}
static void unknown_path_status_never_grants_creation_or_receipt(void){
    const int errors[]={-1,1,2,0};for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){reset();CHECK(start());one_row();stat_result=errors[i];run(0);CHECK(!audio_gets&&!writes&&!events&&!patches);}
}
static void key_event(unsigned recipient,HH_Trigger trigger){HH_Action a={.type=HH_ACT_KEY_EVENT,.recipient=(uint8_t)recipient,.trigger=trigger};hh_supabase_action(&a);}
static void deferred_post_is_sent_once_after_idle(void){
    reset();CHECK(start());key_event(0,HH_ARRIVE);post_status=-1;run(0);
    CHECK(posts==1&&!post_sent);run(100);CHECK(posts==2&&!post_sent);
    post_status=201;run(200);CHECK(posts==3&&post_sent==1);
    CHECK(!strcmp(sent_posts[0],"{\"user_id\":\"A\",\"event\":\"in\"}"));
    run(300);CHECK(posts==3&&post_sent==1);
}
static void deferred_patch_retries_until_started_then_completes_once(void){
    reset();CHECK(start());HH_Message m={.source=HH_REMOTE};strcpy(m.id,"sb_42");hh_supabase_downloaded(&m);
    patch_status=-1;run(0);CHECK(patches==1&&!patch_sent);
    patch_status=204;run(100);CHECK(patches==2&&patch_sent==1&&!strcmp(last_patch_path,"/rest/v1/messages?id=eq.42"));
    run(200);CHECK(patches==2&&patch_sent==1);
}
static void unknown_post_and_http_failure_are_not_retried(void){
    const int statuses[]={0,503};
    for(unsigned i=0;i<sizeof(statuses)/sizeof(statuses[0]);i++){
        reset();CHECK(start());key_event(1,HH_LEAVE);post_status=statuses[i];run(0);
        CHECK(posts==1&&post_sent==1);post_status=201;run(100);CHECK(posts==1&&post_sent==1);
    }
}
static void network_loss_preserves_a_deferred_current_job(void){
    reset();CHECK(start());key_event(1,HH_LEAVE);post_status=-1;run(0);CHECK(posts==1&&!post_sent);
    network_up=false;post_status=201;run(100);CHECK(posts==1&&!post_sent);
    network_up=true;run(200);CHECK(posts==2&&post_sent==1&&strstr(sent_posts[0],"\"user_id\":\"B\""));
}
static void deferred_current_job_keeps_new_events_in_order(void){
    reset();CHECK(start());key_event(0,HH_ARRIVE);post_status=-1;run(0);
    key_event(1,HH_LEAVE);key_event(0,HH_LEAVE);run(100);CHECK(posts==2&&!post_sent);
    post_status=201;run(200);CHECK(post_sent==3);
    CHECK(!strcmp(sent_posts[0],"{\"user_id\":\"A\",\"event\":\"in\"}"));
    CHECK(!strcmp(sent_posts[1],"{\"user_id\":\"B\",\"event\":\"out\"}"));
    CHECK(!strcmp(sent_posts[2],"{\"user_id\":\"A\",\"event\":\"out\"}"));
    run(300);CHECK(post_sent==3);
}
static void deferred_current_job_adds_only_one_bounded_slot(void){
    reset();CHECK(start());key_event(0,HH_ARRIVE);post_status=-1;run(0);
    for(unsigned i=0;i<20;i++)key_event(1,HH_LEAVE);
    CHECK(supa.count==16&&allocs==1);post_status=201;run(100);run(200);
    CHECK(post_sent==17&&allocs==1);
}
static void proxy_profile(void){settings.supabase_host="proxy.unit.invalid";settings.supabase_plain_http=true;settings.supabase_port=8000;settings.supabase_key=NULL;settings.supabase_ca=NULL;}
static void proxy_row(void){rows_a="[{\"id\":42,\"to_user\":\"A\",\"audio_url\":\"http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/test.wav\"}]";}
static void proxy_without_credentials_has_no_legacy_time_filter(void){
    reset();proxy_profile();CHECK(start());run(0);CHECK(polls==2&&!strstr(poll_path,"play_after"));
    sync_rc=time_rc=-1;run(30000);CHECK(polls==4&&!strstr(poll_path,"play_after"));
}
static void proxy_audio_keeps_durable_receipt_and_key_event_pipeline(void){
    reset();proxy_profile();CHECK(start());proxy_row();run(0);
    CHECK(events==1&&writes==1&&audio_gets==1&&!patches&&received[0].trigger==HH_ANY);
    CHECK(received[0].source==HH_REMOTE&&!strcmp(received[0].id,"px_42"));
    hh_supabase_downloaded(&received[0]);patch_status=200;run(100);CHECK(patches==1&&patch_sent==1);
    key_event(1,HH_LEAVE);post_status=200;run(200);CHECK(posts==1&&post_sent==1&&strstr(last_post,"\"event\":\"out\""));
    replace_rename_failure=true;run(30000);CHECK(events==2&&writes==1&&audio_gets==1&&disk_exists);
}
static void proxy_origin_port_bucket_and_path_are_strict(void){
    const char*urls[]={
        "https://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:80/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:443/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:0/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:65536/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:08000/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:8000x/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:8000.evil/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid.evil:8000/storage/v1/object/public/messages_audio/A/a.wav",
        "http://other.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav",
        "http://user@proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/voices/A/a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/private/messages_audio/A/a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/../a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/./a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A//a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/%2e%2e/a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/%252e%252e/a.wav",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav?download=1",
        "http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav#x"};
    for(unsigned i=0;i<sizeof(urls)/sizeof(urls[0]);i++){
        reset();proxy_profile();CHECK(start());char row[512];snprintf(row,sizeof(row),"[{\"id\":42,\"to_user\":\"A\",\"audio_url\":\"%s\"}]",urls[i]);rows_a=row;run(0);
        CHECK(!audio_gets&&!events&&!writes&&!patches);
    }
}
static void proxy_ignores_stale_credentials_and_refuses_binary_redirects(void){
    reset();proxy_profile();settings.supabase_key="eyJsynthetic.stale.value";settings.supabase_ca="unused-ca";
    CHECK(start());proxy_row();binary_status=302;run(0);CHECK(audio_gets==1&&!events&&!writes&&!patches);
}
static void proxy_default_http_origin_and_explicit_default_port_match(void){
    reset();proxy_profile();settings.supabase_port=0;CHECK(start());
    CHECK(storage_path("http://proxy.unit.invalid/storage/v1/object/public/messages_audio/A/a.wav")!=NULL);
    CHECK(storage_path("http://proxy.unit.invalid:80/storage/v1/object/public/messages_audio/A/a.wav")!=NULL);
    CHECK(storage_path("http://proxy.unit.invalid:8000/storage/v1/object/public/messages_audio/A/a.wav")==NULL);
}
static void proxy_numeric_id_does_not_reuse_legacy_cache_or_snapshot_identity(void){
    reset();proxy_profile();CHECK(start());proxy_row();
    memcpy(legacy_disk,wave,sizeof(legacy_disk));legacy_disk[4]=0x7a;legacy_disk_exists=true;
    HH_Core original,restored;HH_Snapshot snapshot;HH_Config cfg=hh_default_config();
    hh_init(&original,&cfg,NULL,NULL,0);HH_Message prior={.recipient=0,.trigger=HH_ANY,.source=HH_REMOTE,.state=HH_PENDING};
    strcpy(prior.id,"sb_42");strcpy(prior.audio_path,HH_ROOT"/messages/sb_42.wav");
    CHECK(hh_add_message(&original,&prior)==HH_OK);hh_export(&original,&snapshot);
    hh_init(&restored,&cfg,NULL,NULL,0);CHECK(hh_restore(&restored,&snapshot)==HH_OK);
    run(0);CHECK(events==1&&audio_gets==1&&writes==1);
    CHECK(!strcmp(received[0].id,"px_42")&&!strcmp(received[0].audio_path,HH_ROOT"/messages/px_42.wav"));
    CHECK(legacy_disk_exists&&legacy_disk[4]==0x7a&&disk_exists&&disk[4]==wave[4]);
    CHECK(hh_add_message(&restored,&received[0])==HH_OK&&hh_pending_count(&restored,0)==2);
}
static void proxy_ack_rejects_legacy_namespace_and_decodes_only_proxy_id(void){
    reset();proxy_profile();CHECK(start());HH_Message m={.source=HH_REMOTE};
    strcpy(m.id,"sb_42");hh_supabase_downloaded(&m);CHECK(supa.count==0);
    strcpy(m.id,"px_01");hh_supabase_downloaded(&m);CHECK(supa.count==0);
    strcpy(m.id,"px_42");hh_supabase_downloaded(&m);CHECK(supa.count==1);
    run(0);CHECK(patches==1&&!strcmp(last_patch_path,"/rest/v1/messages?id=eq.42"));
}
static void legacy_ack_rejects_proxy_namespace_and_keeps_original_numeric_id(void){
    reset();CHECK(start());HH_Message m={.source=HH_REMOTE};
    strcpy(m.id,"px_42");hh_supabase_downloaded(&m);CHECK(supa.count==0);
    strcpy(m.id,"sb_42");hh_supabase_downloaded(&m);CHECK(supa.count==1);
    run(0);CHECK(patches==1&&!strcmp(last_patch_path,"/rest/v1/messages?id=eq.42"));
}
int main(void){RUN(proxy_numeric_id_does_not_reuse_legacy_cache_or_snapshot_identity);RUN(proxy_ack_rejects_legacy_namespace_and_decodes_only_proxy_id);RUN(legacy_ack_rejects_proxy_namespace_and_keeps_original_numeric_id);RUN(proxy_without_credentials_has_no_legacy_time_filter);RUN(proxy_audio_keeps_durable_receipt_and_key_event_pipeline);RUN(proxy_origin_port_bucket_and_path_are_strict);RUN(proxy_ignores_stale_credentials_and_refuses_binary_redirects);RUN(proxy_default_http_origin_and_explicit_default_port_match);RUN(deferred_post_is_sent_once_after_idle);RUN(deferred_patch_retries_until_started_then_completes_once);RUN(unknown_post_and_http_failure_are_not_retried);RUN(network_loss_preserves_a_deferred_current_job);RUN(deferred_current_job_keeps_new_events_in_order);RUN(deferred_current_job_adds_only_one_bounded_slot);RUN(repoll_preserves_good_wav_when_replacement_rename_would_destroy_it);RUN(corrupt_existing_and_failed_read_are_not_overwritten_or_acknowledged);RUN(unknown_path_status_never_grants_creation_or_receipt);RUN(disabled_is_allocation_free_and_start_is_once);RUN(valid_audio_is_durable_posted_before_explicit_receipt_patch);RUN(rejects_fractional_negative_unsafe_and_string_identifiers);RUN(origin_and_public_storage_path_are_exact);RUN(invalid_wav_failed_write_and_canceled_http_never_ack);RUN(actual_utc_and_unsynchronized_clock_have_separate_filters);RUN(recipient_b_polling_interval_and_light_priority_are_preserved);RUN(network_down_does_not_publish_or_poll_and_receipt_recovers);RUN(key_events_are_copied_best_effort_without_post_retry);RUN(legacy_jwt_headers_and_unrecognized_fields_do_not_invent_messages);RUN(bounded_receipt_queue_rejects_bad_local_id_and_preserves_real_rows);RUN(canceled_binary_response_is_freed_without_writing_or_acknowledging);RUN(failed_patch_is_recovered_only_by_repoll_and_durable_callback);RUN(missing_or_mistyped_fields_and_nonarray_root_are_not_messages);printf("%u failure(s)\n",failures);return failures?1:0;}
