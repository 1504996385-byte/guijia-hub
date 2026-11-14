#include "homehub_tuya_light.h"
#include "homehub_cloud_config.h"
#include "homehub_cloud_http.h"
#include "tal_api.h"
#include "tal_time_service.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define LIGHT_MAX_AGE_MS 10000u
typedef struct {
    MUTEX_HANDLE mutex;
    bool started,pending,on,have_sequence,retried;
    uint8_t mask;
    uint32_t sequence,generation,since_ms;
    /* Only the existing background worker accesses the token cache. */
    char token[256];
    uint32_t token_since_ms,token_ttl_ms;
} LightState;
static LightState light;
typedef struct {uint32_t generation,since_ms;bool on;} LightClaim;
static uint32_t light_now(void){return (uint32_t)tal_system_get_millisecond();}
static bool safe_text(const char *s,size_t max,bool path_component){
    if(!s||!*s)return false;
    size_t n=0;
    for(;s[n];n++){
        unsigned char ch=(unsigned char)s[n];
        if(n>=max||ch<33||ch>126)return false;
        if(path_component&&!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
           (ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'))return false;
    }
    return true;
}
static bool settings_valid(const HH_CloudSettings *s){
    return s&&safe_text(s->tuya_host,127,true)&&safe_text(s->tuya_client_id,64,false)&&
        safe_text(s->tuya_secret,128,false)&&safe_text(s->tuya_device_id,64,true)&&
        s->tuya_ca&&*s->tuya_ca&&s->tuya_colour_code&&
        (!strcmp(s->tuya_colour_code,"colour_data")||!strcmp(s->tuya_colour_code,"colour_data_v2"));
}
void hh_tuya_light_start(bool key0,bool key1){
    if(light.started)return;
    if(tal_mutex_create_init(&light.mutex)!=OPRT_OK)return;
    light.mask=(uint8_t)((key0?1u:0u)|(key1?2u:0u));
    light.started=true;
}
void hh_tuya_light_action(const HH_Action *a){
    if(!a||a->type!=HH_ACT_KEY_EVENT||!light.started)return;
    uint8_t mask=(uint8_t)((a->keys_present[0]?1u:0u)|(a->keys_present[1]?2u:0u));
    tal_mutex_lock(light.mutex);
    if(!light.have_sequence||(int32_t)(a->event_seq-light.sequence)>0){
        light.have_sequence=true;light.sequence=a->event_seq;
        if((mask==0)!=(light.mask==0)){
            light.pending=true;light.on=mask!=0;light.since_ms=light_now();light.generation++;light.retried=false;
        }
        light.mask=mask;
    }
    tal_mutex_unlock(light.mutex);
}
static bool claim_current(const LightClaim *c){
    uint32_t now=light_now();
    tal_mutex_lock(light.mutex);
    if(light.pending&&(uint32_t)(now-light.since_ms)>LIGHT_MAX_AGE_MS)light.pending=false;
    bool current=light.pending&&light.generation==c->generation;
    tal_mutex_unlock(light.mutex);return current;
}
static void consume(const LightClaim *c){
    tal_mutex_lock(light.mutex);
    if(light.generation==c->generation)light.pending=false;
    tal_mutex_unlock(light.mutex);
}
static bool use_retry(const LightClaim *c){
    tal_mutex_lock(light.mutex);
    bool allowed=light.pending&&light.generation==c->generation&&!light.retried;
    if(allowed)light.retried=true;
    tal_mutex_unlock(light.mutex);return allowed;
}
static void hex_digest(const uint8_t bytes[32],char out[65],bool upper){
    const char *hex=upper?"0123456789ABCDEF":"0123456789abcdef";
    for(unsigned i=0;i<32;i++){out[2*i]=hex[bytes[i]>>4];out[2*i+1]=hex[bytes[i]&15u];}
    out[64]=0;
}
static bool signed_request(const HH_CloudSettings *s,const char *method,const char *path,
                           const char *body,bool token_request,HH_CloudResponse *response){
    uint8_t digest[32];char body_hash[65],timestamp[24],sign[65],canonical[384],message[768];
    const char *token=token_request?"":light.token;
    memset(response,0,sizeof(*response));
    if(tal_time_check_time_sync()!=OPRT_OK)return false;
    uint64_t milliseconds=(uint64_t)tal_time_get_posix()*1000ULL;
    if(milliseconds<1000000000000ULL||milliseconds>9999999999999ULL)return false;
    snprintf(timestamp,sizeof(timestamp),"%llu",(unsigned long long)milliseconds);
    if(mbedtls_sha256((const uint8_t*)body,strlen(body),digest,0)!=0)return false;
    hex_digest(digest,body_hash,false);
    int n=snprintf(canonical,sizeof(canonical),"%s\n%s\n\n%s",method,body_hash,path);
    if(n<0||(size_t)n>=sizeof(canonical))return false;
    n=snprintf(message,sizeof(message),"%s%s%s%s",s->tuya_client_id,token,timestamp,canonical);
    if(n<0||(size_t)n>=sizeof(message))return false;
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!md||mbedtls_md_hmac(md,(const uint8_t*)s->tuya_secret,strlen(s->tuya_secret),
                          (const uint8_t*)message,(size_t)n,digest)!=0)return false;
    hex_digest(digest,sign,true);
    HH_CloudHeader headers[]={{"client_id",s->tuya_client_id},{"sign",sign},{"t",timestamp},
        {"sign_method","HMAC-SHA256"},{"Content-Type","application/json"},{"access_token",token}};
    HH_CloudRequest request={.method=method,.host=s->tuya_host,.path=path,.ca_pem=s->tuya_ca,
        .headers=headers,.header_count=token_request?5u:6u,.body=(const uint8_t*)body,
        .body_bytes=strlen(body),.max_body=2048,.timeout_ms=8000};
    return hh_cloud_http_request(&request,response);
}
static cJSON *response_json(const HH_CloudResponse *r){
    if(r->status<200||r->status>=300||!r->body||!r->body_bytes||r->body_bytes>2048)return NULL;
    return cJSON_ParseWithLengthOpts((const char*)r->body,r->body_bytes,NULL,false);
}
static bool get_token(const HH_CloudSettings *s){
    uint32_t now=light_now();
    if(light.token[0]&&(uint32_t)(now-light.token_since_ms)<light.token_ttl_ms)return true;
    light.token[0]=0;
    HH_CloudResponse response={0};
    bool requested=signed_request(s,"GET","/v1.0/token?grant_type=1","",true,&response);
    cJSON *root=requested?response_json(&response):NULL;
    cJSON *success=cJSON_GetObjectItemCaseSensitive(root,"success");
    cJSON *result=cJSON_GetObjectItemCaseSensitive(root,"result");
    cJSON *token=cJSON_GetObjectItemCaseSensitive(result,"access_token");
    cJSON *expiry=cJSON_GetObjectItemCaseSensitive(result,"expire_time");
    bool ok=cJSON_IsTrue(success)&&cJSON_IsString(token)&&safe_text(token->valuestring,255,false)&&
        cJSON_IsNumber(expiry)&&expiry->valuedouble>=2&&expiry->valuedouble<=86400&&
        expiry->valuedouble==(double)expiry->valueint;
    if(ok){
        uint32_t seconds=(uint32_t)expiry->valueint;
        uint32_t skew=seconds>60?60:1;
        memcpy(light.token,token->valuestring,strlen(token->valuestring)+1);
        light.token_since_ms=now;light.token_ttl_ms=(seconds-skew)*1000u;
    }
    cJSON_Delete(root);hh_cloud_http_free(&response);return ok;
}
static int send_command(const HH_CloudSettings *s,const LightClaim *claim){
    char path[128],body[256];
    int n=snprintf(path,sizeof(path),"/v1.0/devices/%s/commands",s->tuya_device_id);
    if(n<0||(size_t)n>=sizeof(path))return 0;
    if(claim->on)n=snprintf(body,sizeof(body),
        "{\"commands\":[{\"code\":\"switch_led\",\"value\":true},{\"code\":\"work_mode\",\"value\":\"colour\"},"
        "{\"code\":\"%s\",\"value\":{\"h\":35,\"s\":900,\"v\":800}}]}",s->tuya_colour_code);
    else n=snprintf(body,sizeof(body),"{\"commands\":[{\"code\":\"switch_led\",\"value\":false}]}");
    if(n<0||(size_t)n>=sizeof(body))return 0;
    if(!claim_current(claim))return 0;
    HH_CloudResponse response={0};
    bool requested=signed_request(s,"POST",path,body,false,&response);
    cJSON *root=requested?response_json(&response):NULL;
    cJSON *success=cJSON_GetObjectItemCaseSensitive(root,"success");
    cJSON *result=cJSON_GetObjectItemCaseSensitive(root,"result");
    cJSON *code=cJSON_GetObjectItemCaseSensitive(root,"code");
    bool expired=code&&((cJSON_IsNumber(code)&&code->valueint==1010)||
        (cJSON_IsString(code)&&!strcmp(code->valuestring,"1010")));
    int status=response.status==-1?-1:(cJSON_IsTrue(success)&&cJSON_IsTrue(result)?1:
        (cJSON_IsFalse(success)&&expired?1010:0));
    cJSON_Delete(root);hh_cloud_http_free(&response);return status;
}
void hh_tuya_light_tick(void){
    if(!light.started)return;
    LightClaim claim;
    tal_mutex_lock(light.mutex);
    bool pending=light.pending;claim.generation=light.generation;claim.since_ms=light.since_ms;claim.on=light.on;
    tal_mutex_unlock(light.mutex);
    if(!pending||!claim_current(&claim))return;
    const HH_CloudSettings *settings=hh_cloud_settings();
    if(!settings_valid(settings)||tal_time_check_time_sync()!=OPRT_OK)return;
    if(!get_token(settings)||!claim_current(&claim))return;
    int result=send_command(settings,&claim);
    if(result==1010){
        light.token[0]=0;
        if(claim_current(&claim)&&use_retry(&claim)&&get_token(settings)&&claim_current(&claim))result=send_command(settings,&claim);
        else result=0;
    }
    if(result!=-1){
        PR_NOTICE("HH cloud light on=%u result=%d",(unsigned)claim.on,result);
        consume(&claim);
    }
}
