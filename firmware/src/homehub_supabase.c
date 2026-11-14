#include "homehub_supabase.h"
#include "homehub_platform.h"
#include "homehub_chat.h"
#include "homehub_cloud_config.h"
#include "homehub_cloud_http.h"
#include "tal_api.h"
#include "tkl_memory.h"
#include "tkl_fs.h"
#include "tal_time_service.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define SUPA_JOBS 16u
#define SUPA_POLL_MS 30000u
#define SUPA_SAFE_ID_MAX UINT64_C(9007199254740991)
typedef enum {SUPA_DELIVERED,SUPA_KEY_EVENT} SupaJobType;
typedef struct {SupaJobType type;char id[17];uint8_t recipient,event;} SupaJob;
static struct {
    const HH_CloudSettings *settings;
    SupaJob *jobs;MUTEX_HANDLE lock;THREAD_HANDLE thread;
    unsigned head,count;bool attempted,started,polled,have_job;
    SupaJob current;
    uint32_t last_poll;
} supa;
extern void hh_tuya_light_tick(void);

static size_t bounded_length(const char *s,size_t maximum){size_t n=0;if(!s)return maximum;while(n<maximum&&s[n])n++;return n;}
static bool valid_settings(const HH_CloudSettings *c){
    if(!c || !c->supabase_host)return false;
    size_t host=bounded_length(c->supabase_host,128);
    if(!host || host>=128)return false;
    for(size_t i=0;i<host;i++){char ch=c->supabase_host[i];if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='-'))return false;}
    /* The explicit proxy profile has no device-side Supabase credentials.
     * Stale legacy key/CA fields are ignored and never forwarded over HTTP. */
    if(c->supabase_plain_http)return true;
    if(!c->supabase_key || !c->supabase_ca || !c->supabase_ca[0])return false;
    size_t key=bounded_length(c->supabase_key,1025);
    if(!key || key>1024)return false;
    for(size_t i=0;i<key;i++)if((unsigned char)c->supabase_key[i]<=32||(unsigned char)c->supabase_key[i]>=127)return false;
    return !strncmp(c->supabase_key,"sb_publishable_",15)||!strncmp(c->supabase_key,"eyJ",3);
}
bool hh_supabase_enabled(void){return valid_settings(hh_cloud_settings());}
static bool put_job(const SupaJob *job){
    if(!supa.started || !supa.jobs)return false;
    tal_mutex_lock(supa.lock);bool ok=supa.count<SUPA_JOBS;
    if(ok){supa.jobs[(supa.head+supa.count)%SUPA_JOBS]=*job;supa.count++;}
    tal_mutex_unlock(supa.lock);return ok;
}
static bool take_job(SupaJob *job){
    tal_mutex_lock(supa.lock);bool ok=supa.count!=0;
    if(ok){*job=supa.jobs[supa.head];supa.head=(supa.head+1)%SUPA_JOBS;supa.count--;}
    tal_mutex_unlock(supa.lock);return ok;
}
static const char *local_id_prefix(void){return supa.settings->supabase_plain_http?"px_":"sb_";}
static bool canonical_id(const char *local,char number[17]){
    size_t n=bounded_length(local,HH_ID_MAX+1);
    /* Independent backends may allocate the same numeric row ID. Keep both
     * cached WAVs and persisted core identities in separate namespaces, and
     * never acknowledge an old backend's message against the current one. */
    if(!supa.settings || n<4 || n>19 || strncmp(local,local_id_prefix(),3) || local[3]=='0')return false;
    uint64_t value=0;
    for(size_t i=3;i<n;i++){
        unsigned digit=(unsigned char)local[i]-'0';if(digit>9||value>(SUPA_SAFE_ID_MAX-digit)/10)return false;
        value=value*10+digit;
    }
    if(!value)return false;
    memcpy(number,local+3,n-3);number[n-3]=0;return true;
}
void hh_supabase_downloaded(const HH_Message *message){
    if(!message || message->source!=HH_REMOTE)return;
    SupaJob job={.type=SUPA_DELIVERED};
    if(canonical_id(message->id,job.id))(void)put_job(&job);
}
void hh_supabase_action(const HH_Action *action){
    /* The schema has no played flag and no unique key-event receipt key. */
    if(!action || action->type!=HH_ACT_KEY_EVENT || action->recipient>1 ||
       (action->trigger!=HH_ARRIVE&&action->trigger!=HH_LEAVE) || !hh_chat_network_ready())return;
    SupaJob job={.type=SUPA_KEY_EVENT,.recipient=action->recipient,.event=(uint8_t)action->trigger};
    (void)put_job(&job);
}
static bool request(const char *method,const char *path,const char *body,size_t maximum,bool api,HH_CloudResponse *response){
    char bearer[1032];HH_CloudHeader headers[3];size_t count=0;
    if(api&&!supa.settings->supabase_plain_http){
        headers[count++]=(HH_CloudHeader){"apikey",supa.settings->supabase_key};
        /* Modern publishable keys are not JWTs. Only legacy JWT keys belong
         * in Authorization; public binary downloads carry neither header. */
        if(!strncmp(supa.settings->supabase_key,"eyJ",3)){
            snprintf(bearer,sizeof(bearer),"Bearer %s",supa.settings->supabase_key);
            headers[count++]=(HH_CloudHeader){"Authorization",bearer};
        }
    }
    if(body)headers[count++]=(HH_CloudHeader){"Content-Type","application/json"};
    HH_CloudRequest q={.method=method,.host=supa.settings->supabase_host,.path=path,
        .ca_pem=supa.settings->supabase_plain_http?NULL:supa.settings->supabase_ca,
        .headers=headers,.header_count=count,.body=(const uint8_t*)body,.body_bytes=body?strlen(body):0,.max_body=maximum,.timeout_ms=8000,
        .plain_http=supa.settings->supabase_plain_http,.port=supa.settings->supabase_port};
    memset(response,0,sizeof(*response));
    bool ok=hh_cloud_http_request(&q,response)&&response->status>=200&&response->status<300;
    unsigned kind=!api?1u:!strcmp(method,"PATCH")?2u:!strcmp(method,"POST")?3u:0u;
    PR_NOTICE("HH cloud http kind=%u status=%d ok=%u bytes=%u",kind,response->status,(unsigned)ok,(unsigned)response->body_bytes);
    return ok;
}
static const char *storage_path(const char *url){
    bool plain=supa.settings->supabase_plain_http;
    size_t n=bounded_length(url,1153),host=strlen(supa.settings->supabase_host),scheme=plain?7u:8u;
    if(n>=1153 || n<=scheme+host || strncmp(url,plain?"http://":"https://",scheme)||strncmp(url+scheme,supa.settings->supabase_host,host))return NULL;
    unsigned default_port=plain?80u:443u,expected_port=supa.settings->supabase_port?supa.settings->supabase_port:default_port;
    const char *path=url+scheme+host;
    if(*path==':'){
        const char *digits=++path;unsigned port=0,count=0;
        if(*digits<'1'||*digits>'9')return NULL;
        while(*path>='0'&&*path<='9'){
            if(++count>5)return NULL;
            port=port*10u+(unsigned)(*path++-'0');
        }
        if(port>65535u||port!=expected_port||*path!='/')return NULL;
    }else if(*path!='/'||expected_port!=default_port)return NULL;
    const char *prefix=plain?"/storage/v1/object/public/messages_audio/":"/storage/v1/object/public/";
    size_t prefix_bytes=strlen(prefix);
    if(strncmp(path,prefix,prefix_bytes)||!path[prefix_bytes]||strchr(path,'#')||strstr(path,"/../")||strstr(path,"/./"))return NULL;
    for(size_t i=0;path[i];i++){
        unsigned char ch=(unsigned char)path[i];if(ch<=32||ch>=127||ch=='\\')return NULL;
        if(ch=='%'&&path[i+1]=='2'&&(path[i+2]=='e'||path[i+2]=='E'))return NULL;
        if(plain&&!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch=='/'))return NULL;
    }
    if(plain){
        /* The v2 proxy exposes only this public bucket. Its observed object
         * paths use plain ASCII directories/files; no query, encoded routing,
         * empty segments or traversal aliases are accepted. */
        if(strstr(path,"//"))return NULL;
        const char *last=strrchr(path,'/');
        if(!last||!last[1]||!strcmp(last+1,".")||!strcmp(last+1,".."))return NULL;
    }
    return path;
}
static bool number_id(cJSON *item,char id[HH_ID_MAX+1]){
    if(!supa.settings||!cJSON_IsNumber(item))return false;
    double value=item->valuedouble;
    if(!(value>=1.0&&value<=(double)SUPA_SAFE_ID_MAX))return false;
    uint64_t integer=(uint64_t)value;if((double)integer!=value)return false;
    snprintf(id,HH_ID_MAX+1,"%s%llu",local_id_prefix(),(unsigned long long)integer);return true;
}
typedef enum {CACHE_UNSAFE,CACHE_ABSENT,CACHE_VALID} CacheState;
static CacheState cached_audio(const char *path){
    /* The pinned T5/FatFs adapter returns 4/5 for a missing name/path. Other
     * stat errors, or an unreadable/invalid existing file, never authorize a
     * destructive replacement. hh_file_read owns its own filesystem lock. */
    BOOL_T exists=0;tal_mutex_lock(hh_fs_mutex);
    int rc=tkl_fs_is_exist(path,&exists);tal_mutex_unlock(hh_fs_mutex);
    if(rc==OPRT_OK&&exists){
        uint8_t *bytes=NULL;uint32_t size=0;
        bool valid=hh_file_read(path,&bytes,&size,HH_WAV_MAX)&&size>44&&
            hh_wav_validate(bytes,size,NULL);
        if(bytes)tkl_system_psram_free(bytes);
        return valid?CACHE_VALID:CACHE_UNSAFE;
    }
    return !exists&&(rc==4||rc==5)?CACHE_ABSENT:CACHE_UNSAFE;
}
static void download_message(cJSON *row,unsigned recipient){
    cJSON *to=cJSON_GetObjectItemCaseSensitive(row,"to_user"),*url=cJSON_GetObjectItemCaseSensitive(row,"audio_url");
    HH_Event event={.type=EV_REMOTE_MESSAGE};
    if(!cJSON_IsString(to)||strlen(to->valuestring)!=1||to->valuestring[0]!=(recipient?'B':'A')||
       !cJSON_IsString(url)||!number_id(cJSON_GetObjectItemCaseSensitive(row,"id"),event.message.id))return;
    const char *path=storage_path(url->valuestring);if(!path)return;
    snprintf(event.message.audio_path,sizeof(event.message.audio_path),HH_ROOT"/messages/%s.wav",event.message.id);
    CacheState cache=cached_audio(event.message.audio_path);
    if(cache==CACHE_UNSAFE)return;
    HH_CloudResponse audio={0};
    bool ok=cache==CACHE_VALID;
    if(cache==CACHE_ABSENT){
        ok=request("GET",path,NULL,HH_WAV_MAX,false,&audio)&&audio.status==200&&audio.body&&
            audio.body_bytes>44&&audio.body_bytes<=HH_WAV_MAX&&hh_wav_validate(audio.body,(uint32_t)audio.body_bytes,NULL);
        if(ok){
            /* Recheck after HTTP. A single worker owns the canonical sb_ID
             * namespace; neither the app nor local recorder creates it. */
            cache=cached_audio(event.message.audio_path);
            if(cache==CACHE_ABSENT)ok=hh_file_write_atomic(event.message.audio_path,audio.body,(uint32_t)audio.body_bytes);
            else ok=cache==CACHE_VALID;
        }
    }
    hh_cloud_http_free(&audio);
    if(ok){
        event.message.recipient=(uint8_t)recipient;event.message.trigger=HH_ANY;
        event.message.source=HH_REMOTE;event.message.state=HH_PENDING;hh_post(&event);
        PR_NOTICE("HH cloud mail queued=1 recipient=%u",recipient);
        /* Only the application knows when its metadata snapshot is durable.
         * No PATCH is performed here or after merely posting the event. */
    }
}
static void poll_recipient(unsigned recipient){
    char path[512],filter[128]={0};
    if(!supa.settings->supabase_plain_http){
        POSIX_TM_S utc={0};
        bool synced=tal_time_check_time_sync()==OPRT_OK&&tal_time_get(&utc)==OPRT_OK&&
            utc.tm_year>=124&&utc.tm_year<=200&&utc.tm_mon>=0&&utc.tm_mon<12&&utc.tm_mday>=1&&utc.tm_mday<=31&&
            utc.tm_hour>=0&&utc.tm_hour<24&&utc.tm_min>=0&&utc.tm_min<60&&utc.tm_sec>=0&&utc.tm_sec<60;
        if(synced)snprintf(filter,sizeof(filter),"&or=(play_after.is.null,play_after.lte.%04d-%02d-%02dT%02d%%3A%02d%%3A%02dZ)",
            utc.tm_year+1900,utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec);
        else snprintf(filter,sizeof(filter),"&play_after=is.null");
    }
    snprintf(path,sizeof(path),"/rest/v1/messages?select=id,to_user,from_name,kind,content,audio_url,created_at&to_user=eq.%c&delivered=eq.false%s&order=created_at.asc&limit=5",recipient?'B':'A',filter);
    HH_CloudResponse response={0};
    if(request("GET",path,NULL,32768,true,&response)&&response.status==200&&response.body&&response.body_bytes<=32768){
        cJSON *rows=cJSON_ParseWithLength((const char*)response.body,response.body_bytes);
        if(cJSON_IsArray(rows)){
            unsigned n=0;cJSON *row;cJSON_ArrayForEach(row,rows){if(n++==5)break;hh_tuya_light_tick();download_message(row,recipient);}
        }
        cJSON_Delete(rows);
    }
    hh_cloud_http_free(&response);
}
static bool send_job(const SupaJob *job){
    char path[96],body[64];HH_CloudResponse response={0};
    if(job->type==SUPA_DELIVERED){
        snprintf(path,sizeof(path),"/rest/v1/messages?id=eq.%s",job->id);
        (void)request("PATCH",path,"{\"delivered\":true}",4096,true,&response);
    }else{
        snprintf(body,sizeof(body),"{\"user_id\":\"%c\",\"event\":\"%s\"}",job->recipient?'B':'A',job->event==HH_ARRIVE?"in":"out");
        (void)request("POST","/rest/v1/home_events",body,4096,true,&response);
    }
    /* Only -1 proves no request started. Keep that job ahead of later events;
     * unknown delivery and actual HTTP failures must not blindly retry POST.
     * A failed PATCH can still recover by repoll and a durable duplicate. */
    bool consumed=response.status!=-1;
    hh_cloud_http_free(&response);
    return consumed;
}
static void supabase_step(uint32_t now){
    if(!supa.started||!hh_chat_network_ready())return;
    hh_tuya_light_tick();
    for(unsigned i=0;i<SUPA_JOBS;i++){
        if(!supa.have_job){
            if(!take_job(&supa.current))break;
            supa.have_job=true;
        }
        if(!send_job(&supa.current))return;
        supa.have_job=false;
    }
    if(!supa.polled||(uint32_t)(now-supa.last_poll)>=SUPA_POLL_MS){
        supa.polled=true;supa.last_poll=now;poll_recipient(0);poll_recipient(1);
    }
}
static void worker(void *unused){(void)unused;for(;;){supabase_step((uint32_t)tal_system_get_millisecond());tal_system_sleep(100);}}
void hh_supabase_start(void){
    if(supa.attempted||!hh_supabase_enabled())return;
    supa.attempted=true;supa.settings=hh_cloud_settings();
    if(tal_mutex_create_init(&supa.lock)!=OPRT_OK)return;
    supa.jobs=tkl_system_psram_malloc(sizeof(SupaJob)*SUPA_JOBS);if(!supa.jobs)return;
    THREAD_CFG_T cfg={.stackDepth=12288,.priority=THREAD_PRIO_1,.thrdname="hh_supabase",.psram_mode=1};
    supa.started=true;
    if(tal_thread_create_and_start(&supa.thread,NULL,NULL,worker,NULL,&cfg)!=OPRT_OK){supa.started=false;tkl_system_psram_free(supa.jobs);supa.jobs=NULL;}
    PR_NOTICE("HH cloud worker started=%u",(unsigned)supa.started);
}
