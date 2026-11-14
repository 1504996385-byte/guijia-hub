#include "homehub_tuya_light.h"
#include "homehub_cloud_config.h"
#include "homehub_cloud_http.h"
#include "tal_api.h"
#include "tal_time_service.h"
#include "cJSON.h"
#include "vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures,depth,calls,commands,token_calls,http_errors;
static uint32_t now_ms,utc_sec,sequence;
static bool synced,have_settings,transport_ok;
static int reply_status,expire_replies,expire_value,defer_command;
static bool command_transport_fail;
static const char *token_reply,*command_reply;
static void (*request_hook)(void);
static HH_CloudSettings settings;
typedef struct {char sign[65],timestamp[24],body[256],token[256];} RequestCopy;
static RequestCopy requests[20];
#define CHECK(x) do {if(!(x)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);failures++;return;}}while(0)
const HH_CloudSettings *hh_cloud_settings(void){return have_settings?&settings:NULL;}
int tal_mutex_create_init(MUTEX_HANDLE *m){*m=(void*)1;return 0;}
int tal_mutex_lock(MUTEX_HANDLE m){(void)m;depth++;return 0;}
int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;depth--;return 0;}
uint64_t tal_system_get_millisecond(void){return now_ms;}
TIME_T tal_time_get_posix(void){return utc_sec;}
int tal_time_check_time_sync(void){return synced?0:-1;}
void hh_cloud_http_background_idle(bool idle){(void)idle;}
bool hh_cloud_http_request(const HH_CloudRequest *r,HH_CloudResponse *out){
    if(depth||!r->ca_pem||!*r->ca_pem||r->timeout_ms>8000||!r->max_body)http_errors++;
    RequestCopy *copy=&requests[calls<20?calls:19];memset(copy,0,sizeof(*copy));calls++;
    for(size_t i=0;i<r->header_count;i++){
        if(!strcmp(r->headers[i].key,"sign"))snprintf(copy->sign,sizeof(copy->sign),"%s",r->headers[i].value);
        if(!strcmp(r->headers[i].key,"t"))snprintf(copy->timestamp,sizeof(copy->timestamp),"%s",r->headers[i].value);
        if(!strcmp(r->headers[i].key,"access_token"))snprintf(copy->token,sizeof(copy->token),"%s",r->headers[i].value);
    }
    if(r->body_bytes)snprintf(copy->body,sizeof(copy->body),"%.*s",(int)r->body_bytes,r->body);
    bool token=!strcmp(r->path,"/v1.0/token?grant_type=1");
    if(token)token_calls++;else commands++;
    if(request_hook){void(*hook)(void)=request_hook;request_hook=NULL;hook();}
    if(!token&&commands==defer_command){memset(out,0,sizeof(*out));out->status=-1;return false;}
    const char *body=token?token_reply:command_reply;
    if(!token&&expire_replies>0){expire_replies--;body=expire_value==1010?"{\"success\":false,\"code\":1010}":"{\"success\":false,\"code\":1011}";}
    memset(out,0,sizeof(*out));out->status=reply_status;out->body=(const uint8_t*)body;out->body_bytes=strlen(body);
    return transport_ok&&!(command_transport_fail&&!token);
}
void hh_cloud_http_free(HH_CloudResponse *r){memset(r,0,sizeof(*r));}
#ifdef HH_LIGHT_RED
void hh_tuya_light_start(bool a,bool b){(void)a;(void)b;}
void hh_tuya_light_action(const HH_Action *a){(void)a;}
void hh_tuya_light_tick(void){}
#else
#include "../../firmware/src/homehub_tuya_light.c"
#endif
static void reset(unsigned mask){
#ifndef HH_LIGHT_RED
    memset(&light,0,sizeof(light));
#endif
    depth=calls=commands=token_calls=http_errors=0;now_ms=100;utc_sec=1793923200u;sequence=0;
    synced=have_settings=transport_ok=true;reply_status=200;expire_replies=0;expire_value=1010;request_hook=NULL;
    defer_command=0;command_transport_fail=false;
    token_reply="{\"success\":true,\"result\":{\"access_token\":\"test-access-token\",\"expire_time\":7200}}";
    command_reply="{\"success\":true,\"result\":true}";
    memset(&settings,0,sizeof(settings));settings.tuya_host="api.example.invalid";
    settings.tuya_client_id="example-client";settings.tuya_secret="example-secret";
    settings.tuya_device_id="example-device";settings.tuya_ca="synthetic-test-ca";
#ifndef HH_LIGHT_RED
    settings.tuya_colour_code="colour_data";
#endif
    hh_tuya_light_start((mask&1u)!=0,(mask&2u)!=0);
}
static void event(unsigned mask){HH_Action a={0};a.type=HH_ACT_KEY_EVENT;a.event_seq=++sequence;
    a.keys_present[0]=(mask&1u)!=0;a.keys_present[1]=(mask&2u)!=0;hh_tuya_light_action(&a);}
static void all_boot_masks_are_silent_and_first_real_crossing_works(void){
    for(unsigned mask=0;mask<4;mask++){reset(mask);hh_tuya_light_tick();CHECK(calls==0);
        event(mask?0:1);hh_tuya_light_tick();CHECK(commands==1&&token_calls==1&&http_errors==0);
        CHECK(strstr(requests[1].body,mask?"false":"true")!=NULL);}
    for(unsigned before=0;before<4;before++)for(unsigned after=0;after<4;after++){
        reset(before);event(after);hh_tuya_light_tick();CHECK(commands==((before==0)!=(after==0)?1:0));
    }
}
static void full_masks_avoid_counter_drift_and_duplicates(void){
    reset(3);event(1);event(3);event(2);hh_tuya_light_tick();CHECK(calls==0);
    event(0);event(0);hh_tuya_light_tick();CHECK(commands==1);
    event(0);hh_tuya_light_tick();CHECK(commands==1);
    HH_Action old={.type=HH_ACT_KEY_EVENT,.event_seq=sequence-1};old.keys_present[0]=true;
    hh_tuya_light_action(&old);hh_tuya_light_tick();CHECK(commands==1);
    event(3);hh_tuya_light_tick();CHECK(commands==2&&token_calls==1);
}
static void rapid_boundaries_coalesce_to_latest_desired(void){
    reset(0);event(1);event(0);hh_tuya_light_tick();CHECK(commands==1);
    CHECK(strstr(requests[1].body,"false")!=NULL);
    reset(3);event(0);event(3);hh_tuya_light_tick();CHECK(commands==1);
    CHECK(strstr(requests[1].body,"true")!=NULL);
}
static void real_crypto_matches_independent_token_and_body_vectors(void){
    reset(0);event(1);hh_tuya_light_tick();CHECK(calls==2);
    CHECK(!strcmp(requests[0].timestamp,"1793923200000"));
    CHECK(!strcmp(requests[0].sign,TOKEN_SIGN)&&!strcmp(requests[1].sign,ON_SIGN));
    CHECK(!strcmp(requests[1].token,"test-access-token")&&http_errors==0);
#ifndef HH_LIGHT_RED
    CHECK(sizeof(light)<=384);
#endif
}
static void unsynced_time_or_missing_settings_never_requests(void){
    reset(0);event(1);synced=false;hh_tuya_light_tick();CHECK(calls==0);
    synced=true;hh_tuya_light_tick();CHECK(commands==1);
    reset(0);event(1);have_settings=false;hh_tuya_light_tick();CHECK(calls==0);
    reset(0);event(1);settings.tuya_ca="";hh_tuya_light_tick();CHECK(calls==0);
}
static void expired_actions_drop_without_nonboundary_age_refresh(void){
    reset(0);event(1);now_ms+=9000;event(3);now_ms+=1001;hh_tuya_light_tick();CHECK(calls==0);
    event(0);hh_tuya_light_tick();CHECK(commands==1);
}
static void newer_boundary_during_token_fetch(void){
    event(0);
}
static void stale_time_during_token_fetch(void){now_ms+=10001;}
static void changes_during_https_never_send_an_old_command(void){
    reset(0);event(1);request_hook=newer_boundary_during_token_fetch;hh_tuya_light_tick();
    CHECK(token_calls==1&&commands==0);hh_tuya_light_tick();CHECK(commands==1);
    CHECK(strstr(requests[1].body,"false")!=NULL);
    reset(0);event(1);request_hook=stale_time_during_token_fetch;hh_tuya_light_tick();CHECK(commands==0);
}
static void malformed_or_failed_token_responses_never_control(void){
    const char *bad[]={"{}","{\"success\":false}","{\"success\":true,\"result\":{\"access_token\":\"x\"}}","{\"success\":true,\"result\":{\"access_token\":\"x\",\"expire_time\":0}}","{\"success\":true,\"result\":{\"access_token\":\"x\\r\\nBad: injected\",\"expire_time\":7200}}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){reset(0);event(1);token_reply=bad[i];hh_tuya_light_tick();CHECK(commands==0);}
    reset(0);event(1);reply_status=401;hh_tuya_light_tick();CHECK(commands==0);
    reset(0);event(1);transport_ok=false;hh_tuya_light_tick();CHECK(commands==0);
}
static void explicit_expired_token_retries_only_once(void){
    reset(0);event(1);expire_replies=1;hh_tuya_light_tick();CHECK(commands==2&&token_calls==2);
    reset(0);event(1);expire_replies=3;hh_tuya_light_tick();CHECK(commands==2&&token_calls==2);
    hh_tuya_light_tick();CHECK(commands==2);
}
static void invalid_token_or_other_business_failure_does_not_blindly_retry(void){
    reset(0);event(1);expire_value=1011;expire_replies=1;hh_tuya_light_tick();CHECK(commands==1&&token_calls==1);
    hh_tuya_light_tick();CHECK(commands==1);
    reset(0);event(1);command_reply="{\"success\":false,\"code\":1106}";hh_tuya_light_tick();
    hh_tuya_light_tick();CHECK(commands==1&&token_calls==1);
}
static void token_cache_uses_expiry_and_monotonic_clock(void){
    reset(0);event(1);hh_tuya_light_tick();CHECK(token_calls==1);
    utc_sec+=86400;event(0);hh_tuya_light_tick();CHECK(token_calls==1&&commands==2);
    now_ms+=7140000;event(1);hh_tuya_light_tick();CHECK(token_calls==2&&commands==3);
}
static void foreground_actions_never_call_https_and_timer_wrap_is_safe(void){
    reset(0);now_ms=UINT32_MAX-100u;event(1);CHECK(calls==0);now_ms+=200;hh_tuya_light_tick();CHECK(commands==1);
    event(0);now_ms+=10001;hh_tuya_light_tick();CHECK(commands==1);
    reset(0);sequence=UINT32_MAX-1u;event(1);hh_tuya_light_tick();event(0);hh_tuya_light_tick();
    CHECK(commands==2);
}
static void foreground_deferral_keeps_only_a_fresh_claim(void){
    reset(0);event(1);defer_command=1;hh_tuya_light_tick();CHECK(commands==1&&token_calls==1);
    hh_tuya_light_tick();CHECK(commands==2&&token_calls==1);
    reset(0);event(1);defer_command=1;hh_tuya_light_tick();now_ms+=10001;
    hh_tuya_light_tick();CHECK(commands==1);
    reset(0);event(1);command_transport_fail=true;hh_tuya_light_tick();
    hh_tuya_light_tick();CHECK(commands==1);
}
static void expired_retry_budget_survives_a_deferred_retry(void){
    reset(0);event(1);expire_replies=3;defer_command=2;
    hh_tuya_light_tick();CHECK(commands==2&&token_calls==2);
    hh_tuya_light_tick();CHECK(commands==3&&token_calls==2);
    hh_tuya_light_tick();CHECK(commands==3&&token_calls==2);
}
#define RUN(t) do{int before=failures;t();if(before==failures)printf("PASS %s\n",#t);}while(0)
int main(void){
    RUN(all_boot_masks_are_silent_and_first_real_crossing_works);RUN(full_masks_avoid_counter_drift_and_duplicates);
    RUN(rapid_boundaries_coalesce_to_latest_desired);RUN(real_crypto_matches_independent_token_and_body_vectors);
    RUN(unsynced_time_or_missing_settings_never_requests);RUN(expired_actions_drop_without_nonboundary_age_refresh);
    RUN(changes_during_https_never_send_an_old_command);RUN(malformed_or_failed_token_responses_never_control);
    RUN(explicit_expired_token_retries_only_once);RUN(invalid_token_or_other_business_failure_does_not_blindly_retry);
    RUN(token_cache_uses_expiry_and_monotonic_clock);RUN(foreground_actions_never_call_https_and_timer_wrap_is_safe);
    RUN(foreground_deferral_keeps_only_a_fresh_claim);RUN(expired_retry_budget_survives_a_deferred_retry);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
