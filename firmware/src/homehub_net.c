#include "homehub_platform.h"
#include "homehub_chat.h"
#include "http_client_interface.h"
#include "core_http_config.h"
#include "tkl_memory.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

volatile bool hh_network_ready;
static QUEUE_HANDLE net_jobs;
static char host[128],base_path[128],authorization[144];
static uint16_t port;
static uint8_t *ca;
static uint32_t ca_size;
static bool configured,tls;
typedef struct { HH_Action action; bool downloaded; } NetJob;
static void refresh_greeting(const HH_Action *action);
static void hh_http_free(http_client_response_t *response)
{
    /* coreHTTP allocates responses with HTTP_MALLOC (PSRAM with EXT_RAM).
       Match that allocator instead of the wrapper's unconditional tal_free. */
    if(response->buffer)HTTP_FREE(response->buffer);
    if(response->body)HTTP_FREE((void*)response->body);
    memset(response,0,sizeof(*response));
}
static bool parse_url(void)
{
    const char *p=hh_device.server_url;tls=!strncmp(p,"https://",8);
    if(tls)p+=8;else if(!strncmp(p,"http://",7))p+=7;else return false;
    const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);
    if(!n||n>=sizeof(host)||memchr(p,'@',n)||memchr(p,'\r',n)||memchr(p,'\n',n))return false;
    memcpy(host,p,n);host[n]=0;port=tls?443:80;
    char *colon=strchr(host,':');
    if(colon){char *end=NULL;long v=strtol(colon+1,&end,10);if(!end||*end||v<1||v>65535)return false;port=(uint16_t)v;*colon=0;}
    if(slash){if(strlen(slash)>=sizeof(base_path)||strchr(slash,'?')||strchr(slash,'#'))return false;snprintf(base_path,sizeof(base_path),"%s",slash);}
    n=strlen(base_path);while(n&&base_path[n-1]=='/')base_path[--n]=0;
    if(tls&&!hh_file_read(HH_ROOT "/server_ca.pem",&ca,&ca_size,16384))return false;
    snprintf(authorization,sizeof(authorization),"Bearer %s",hh_device.device_token);
    return true;
}
static bool request(const char *method,const char *path,const char *body,http_client_response_t *response)
{
    char full_path[256];snprintf(full_path,sizeof(full_path),"%s%s",base_path,path);
    http_client_header_t headers[]={{"Authorization",authorization},{"Content-Type","application/json"}};
    http_client_request_t req={.host=host,.port=port,.path=full_path,.cacert=ca,
        .cacert_len=ca?ca_size+1:0,.tls_no_verify=false,.method=method,.headers=headers,.headers_count=2,
        .body=(const uint8_t*)body,.body_length=body?strlen(body):0,.timeout_ms=8000};
    memset(response,0,sizeof(*response));
    return http_client_request(&req,response)==HTTP_CLIENT_SUCCESS&&response->status_code>=200&&response->status_code<300;
}
static void outbox_write(const NetJob *job)
{
    const HH_Action *a=&job->action;
    if(a->type==HH_ACT_PLAYED_RECEIPT&&a->source!=HH_REMOTE)return;
    cJSON *wrapper=cJSON_CreateObject(),*body=cJSON_CreateObject();char event[64],path[192];
    if(a->type==HH_ACT_KEY_EVENT){
        snprintf(event,sizeof(event),"%08x-key-%08x",hh_boot_id,a->event_seq);
    }else{
        char identity[80];uint8_t digest[32];
        snprintf(identity,sizeof(identity),"%s:%s",job->downloaded?"downloaded":"played",a->id);
        if(mbedtls_sha256((const uint8_t*)identity,strlen(identity),digest,0)!=0){cJSON_Delete(body);cJSON_Delete(wrapper);return;}
        memcpy(event,"r_",2);for(unsigned i=0;i<20;i++)snprintf(event+2+2*i,3,"%02x",digest[i]);
    }
    cJSON_AddStringToObject(body,"event_id",event);
    if(a->type==HH_ACT_KEY_EVENT){
        cJSON_AddStringToObject(wrapper,"endpoint","/api/device/events");
        cJSON_AddNumberToObject(body,"recipient",a->recipient);
        cJSON_AddStringToObject(body,"event",a->trigger==HH_ARRIVE?"arrive":"leave");
    }else{
        if(!hh_valid_id(a->id)){cJSON_Delete(body);cJSON_Delete(wrapper);return;}
        cJSON_AddStringToObject(wrapper,"endpoint","/api/device/receipt");
        cJSON_AddStringToObject(body,"id",a->id);cJSON_AddStringToObject(body,"status",job->downloaded?"downloaded":"played");
    }
    cJSON_AddItemToObject(wrapper,"body",body);char *text=cJSON_PrintUnformatted(wrapper);
    snprintf(path,sizeof(path),HH_ROOT "/outbox/%s.json",event);
    if(text){if(!hh_file_write_atomic(path,(uint8_t*)text,strlen(text)))PR_ERR("Outbox write failed");cJSON_free(text);}
    cJSON_Delete(wrapper);
}
static void flush_outbox(void)
{
    /* Copy a bounded list under the FS lock; HTTP never holds the filesystem
       mutex, so microphone saving remains independent of network timeouts. */
    char paths[8][192];unsigned count=0;TUYA_DIR dir;TUYA_FILEINFO info;
    tal_mutex_lock(hh_fs_mutex);
    if(tkl_dir_open(HH_ROOT "/outbox",&dir)==0){
        while(count<8&&tkl_dir_read(dir,&info)==0){const char *name=NULL;
            if(tkl_dir_name(info,&name)==0&&name&&strstr(name,".json")&&strlen(name)<100){
                snprintf(paths[count++],sizeof(paths[0]),HH_ROOT "/outbox/%s",name);
            }
        }tkl_dir_close(dir);
    }tal_mutex_unlock(hh_fs_mutex);
    for(unsigned i=0;i<count;i++){
        uint8_t *data=NULL;uint32_t size=0;if(!hh_file_read(paths[i],&data,&size,4096))continue;
        cJSON *j=cJSON_Parse((char*)data);tkl_system_psram_free(data);if(!j)continue;
        cJSON *endpoint=cJSON_GetObjectItemCaseSensitive(j,"endpoint"),*body=cJSON_GetObjectItemCaseSensitive(j,"body");
        if(cJSON_IsString(endpoint)&&body&&(!strcmp(endpoint->valuestring,"/api/device/events")||!strcmp(endpoint->valuestring,"/api/device/receipt"))){
            char *text=cJSON_PrintUnformatted(body);http_client_response_t response={0};
            bool ok=text&&request("POST",endpoint->valuestring,text,&response);
            hh_http_free(&response);if(text)cJSON_free(text);
            if(ok){tal_mutex_lock(hh_fs_mutex);tkl_fs_remove(paths[i]);tal_mutex_unlock(hh_fs_mutex);}
        }cJSON_Delete(j);
    }
}
static bool matches(const uint8_t *data,uint32_t bytes,const char *expected)
{
    if(!expected||strlen(expected)!=64)return false;
    uint8_t digest[32];char hex[65];if(mbedtls_sha256(data,bytes,digest,0)!=0)return false;
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    return !strcmp(hex,expected);
}
static void remote_message(cJSON *j)
{
    cJSON *id=cJSON_GetObjectItemCaseSensitive(j,"id"),*recipient=cJSON_GetObjectItemCaseSensitive(j,"recipient"),
        *trigger=cJSON_GetObjectItemCaseSensitive(j,"trigger"),*hash=cJSON_GetObjectItemCaseSensitive(j,"sha256"),
        *bytes=cJSON_GetObjectItemCaseSensitive(j,"bytes"),*url=cJSON_GetObjectItemCaseSensitive(j,"audio_url");
    if(!cJSON_IsString(id)||!hh_valid_id(id->valuestring)||!cJSON_IsNumber(recipient)||
       recipient->valueint<0||recipient->valueint>1||!cJSON_IsString(trigger)||
       (strcmp(trigger->valuestring,"arrive")&&strcmp(trigger->valuestring,"leave"))||
       !cJSON_IsString(hash)||!cJSON_IsNumber(bytes)||bytes->valueint<=44||bytes->valueint>(int)HH_WAV_MAX||!cJSON_IsString(url))return;
    char endpoint[128],path[192];snprintf(endpoint,sizeof(endpoint),"/api/device/audio/%s.wav",id->valuestring);
    if(strcmp(url->valuestring,endpoint))return;
    snprintf(path,sizeof(path),HH_ROOT "/messages/%s.wav",id->valuestring);
    uint8_t *existing=NULL;uint32_t existing_size=0;
    bool valid=hh_file_read(path,&existing,&existing_size,HH_WAV_MAX)&&existing_size==(uint32_t)bytes->valueint&&
        hh_wav_validate(existing,existing_size,NULL)&&matches(existing,existing_size,hash->valuestring);
    if(existing)tkl_system_psram_free(existing);
    if(!valid){
        http_client_response_t response={0};bool ok=request("GET",endpoint,NULL,&response);
        valid=ok&&response.body_length==(size_t)bytes->valueint&&hh_wav_validate(response.body,response.body_length,NULL)&&
            matches(response.body,response.body_length,hash->valuestring)&&hh_file_write_atomic(path,response.body,response.body_length);
        hh_http_free(&response);
    }
    if(valid){HH_Event event={.type=EV_REMOTE_MESSAGE};snprintf(event.message.id,sizeof(event.message.id),"%s",id->valuestring);
        snprintf(event.message.audio_path,sizeof(event.message.audio_path),"%s",path);event.message.recipient=recipient->valueint;
        event.message.trigger=!strcmp(trigger->valuestring,"arrive")?HH_ARRIVE:HH_LEAVE;
        event.message.source=HH_REMOTE;event.message.state=HH_PENDING;hh_post(&event);
    }
}
static void poll(void)
{
    http_client_response_t response={0};
    if(request("GET","/api/device/poll?limit=8",NULL,&response)&&response.body_length<32768){
        cJSON *json=cJSON_ParseWithLength((const char*)response.body,response.body_length);
        cJSON *messages=cJSON_GetObjectItemCaseSensitive(json,"messages");
        if(cJSON_IsArray(messages)){cJSON *m;unsigned n=0;cJSON_ArrayForEach(m,messages){if(n++>=8)break;remote_message(m);}}
        cJSON *weather=cJSON_GetObjectItemCaseSensitive(json,"weather");
        cJSON *temperature=cJSON_GetObjectItemCaseSensitive(weather,"temperature"),*condition=cJSON_GetObjectItemCaseSensitive(weather,"condition");
        if(cJSON_IsNumber(temperature)&&cJSON_IsString(condition)){
            HH_Event event={.type=EV_NETWORK_INFO};int deci=(int)(temperature->valuedouble*10);
            bool stale=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather,"stale"));
            snprintf(event.text,sizeof(event.text),"%s%s%d.%d C  %.120s",stale?"缓存 ":"",deci<0?"-":"",abs(deci)/10,abs(deci)%10,condition->valuestring);
            hh_post(&event);
        }
        cJSON_Delete(json);
    }hh_http_free(&response);
}
static void refresh_greeting(const HH_Action *action)
{
    /* Leave events remain in the outbox, but never request an extra voice. */
    if(action->trigger!=HH_ARRIVE)return;
    char endpoint[128];snprintf(endpoint,sizeof(endpoint),"/api/device/greeting?recipient=%u&event=%s",action->recipient,action->trigger==HH_ARRIVE?"arrive":"leave");
    http_client_response_t response={0};
    if(!request("GET",endpoint,NULL,&response)||response.body_length>32768){hh_http_free(&response);return;}
    cJSON *json=cJSON_ParseWithLength((const char*)response.body,response.body_length);hh_http_free(&response);
    cJSON *source=cJSON_GetObjectItemCaseSensitive(json,"source"),*text=cJSON_GetObjectItemCaseSensitive(json,"text"),
        *url=cJSON_GetObjectItemCaseSensitive(json,"audio_url"),*bytes=cJSON_GetObjectItemCaseSensitive(json,"bytes"),
        *hash=cJSON_GetObjectItemCaseSensitive(json,"sha256");
    HH_Event event={.type=EV_NETWORK_INFO,.action=*action};
    if(cJSON_IsString(source)&&strcmp(source->valuestring,"unconfigured")){
        if(cJSON_IsString(text)){
            size_t length=strlen(text->valuestring);if(length>=sizeof(event.text))length=sizeof(event.text)-1;
            while(length&&((unsigned char)text->valuestring[length]&0xc0)==0x80)length--;
            memcpy(event.text,text->valuestring,length);event.text[length]=0;
        }
        if(cJSON_IsString(url)&&!strncmp(url->valuestring,"/api/device/audio/",18)&&cJSON_IsNumber(bytes)&&
           bytes->valueint>44&&bytes->valueint<=(int)HH_WAV_MAX&&cJSON_IsString(hash)){
            const char *name=url->valuestring+18;size_t length=strlen(name);char id[HH_ID_MAX+1]={0};
            if(length>4&&length-4<=HH_ID_MAX&&!strcmp(name+length-4,".wav")){
                memcpy(id,name,length-4);
                if(hh_valid_id(id)){
                    http_client_response_t audio={0};
                    snprintf(event.action.audio_path,sizeof(event.action.audio_path),HH_ROOT "/voices/net%u_%u.wav",action->recipient,action->trigger);
                    event.ok=request("GET",url->valuestring,NULL,&audio)&&audio.body_length==(size_t)bytes->valueint&&
                        hh_wav_validate(audio.body,audio.body_length,NULL)&&matches(audio.body,audio.body_length,hash->valuestring)&&
                        hh_file_write_atomic(event.action.audio_path,audio.body,audio.body_length);
                    hh_http_free(&audio);
                }
            }
        }
        if(event.text[0]||event.ok)hh_post(&event);
    }cJSON_Delete(json);
}
static void network_worker(void *unused)
{
    (void)unused;NetJob job;uint32_t last_poll=0;
    /* The chat/network worker owns runtime and netmgr even without a Tuya
     * license. HTTP configuration does not decide whether Wi-Fi may run. */
    while(1){
        hh_network_ready=hh_chat_network_ready();
        while(tal_queue_fetch(net_jobs,&job,0)==OPRT_OK){
            outbox_write(&job);
            if(configured&&hh_network_ready&&job.action.type==HH_ACT_KEY_EVENT)refresh_greeting(&job.action);
        }
        if(configured){
            uint32_t now=(uint32_t)tal_system_get_millisecond();
            if(hh_network_ready){flush_outbox();if(!last_poll||now-last_poll>=30000){last_poll=now;poll();}}
        }tal_system_sleep(1000);
    }
}
void hh_net_action(const HH_Action *action)
{
    NetJob job={.action=*action};if(net_jobs&&tal_queue_post(net_jobs,&job,0)!=OPRT_OK)PR_ERR("Network outbox queue full");
}
void hh_net_downloaded(const HH_Message *message)
{
    NetJob job={.downloaded=true};job.action.type=HH_ACT_PLAYED_RECEIPT;job.action.source=HH_REMOTE;
    snprintf(job.action.id,sizeof(job.action.id),"%s",message->id);
    if(net_jobs)tal_queue_post(net_jobs,&job,0);
}
void hh_net_start(void)
{
    configured=hh_device.server_url[0]&&hh_device.device_token[0]&&parse_url();
    /* Local-only devices have no HTTP work or remote outbox to service. Keep
       the native queue and thread stack available for recording/file saves. */
    if(!configured)return;
    if(tal_queue_create_init(&net_jobs,sizeof(NetJob),32)!=OPRT_OK){configured=false;PR_ERR("Network queue creation failed");return;}
    THREAD_HANDLE thread=NULL;THREAD_CFG_T cfg={.stackDepth=12288,.priority=THREAD_PRIO_1,.thrdname="hh_network"};
    tal_thread_create_and_start(&thread,NULL,NULL,network_worker,NULL,&cfg);
}
