#include "homehub_cloud_http.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"
#include "tal_api.h"
#include "mbedtls/ssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static unsigned failures,allocations,frees,live,max_alloc,creates,destroys,closes,connects,tls_creates,tls_frees,tls_disconnects;
static unsigned clock_ms,reads,writes,largest_poll,socket_timeouts,fail_alloc_at;
static int expected_port,connected_port;
static bool connect_fail,tls_fail,cancel_after_connect,cancel_after_read,cancel_after_write,flip_busy_back,stall_reads;
static const char *wire;
static size_t wire_pos,wire_length,read_chunk;
static int fake_tcp,fake_tls;
static tuya_tls_config_t observed_tls;
static char sent[16384],logs[2048];static size_t sent_bytes;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__func__,__LINE__,#c);failures++;return;}}while(0)
#define RUN(t) do{unsigned before=failures;t();printf("%s %s\n",before==failures?"PASS":"FAIL",#t);fflush(stdout);}while(0)
void http_fake_log(const char *format,...){size_t n=strlen(logs);if(n>=sizeof(logs)-1)return;va_list a;va_start(a,format);vsnprintf(logs+n,sizeof(logs)-n,format,a);va_end(a);}
void *tal_psram_malloc(size_t size){allocations++;if(size>max_alloc)max_alloc=(unsigned)size;if(fail_alloc_at==allocations)return NULL;void*p=malloc(size);if(p)live++;return p;}
void tal_psram_free(void *p){if(p){frees++;live--;free(p);}}
uint64_t tal_system_get_millisecond(void){return clock_ms;}
void tal_system_sleep(uint32_t ms){clock_ms+=ms;}
int tal_net_set_timeout(int fd,int ms,int type){(void)type;if(fd!=7||ms>500)failures++;socket_timeouts++;return 0;}
tuya_transporter_t tuya_transporter_create(TUYA_TRANSPORT_TYPE_E kind,tuya_transporter_t dependency){(void)dependency;creates++;if(kind!=TRANSPORT_TYPE_TCP)failures++;return (tuya_transporter_t)&fake_tcp;}
OPERATE_RET tuya_transporter_destroy(tuya_transporter_t t){(void)t;destroys++;return 0;}
OPERATE_RET tuya_transporter_close(tuya_transporter_t t){(void)t;closes++;return 0;}
OPERATE_RET tuya_transporter_ctrl(tuya_transporter_t t,uint32_t cmd,void *args){(void)t;if(cmd==TUYA_TRANSPORTER_GET_TCP_SOCKET)*(int*)args=7;return 0;}
OPERATE_RET tuya_transporter_connect(tuya_transporter_t t,const char *host,int port,int timeout_ms){(void)t;(void)timeout_ms;connects++;connected_port=port;if(strcmp(host,"project.example")||port!=expected_port)failures++;if(cancel_after_connect)hh_cloud_http_background_idle(false);return connect_fail?-1:0;}
OPERATE_RET tuya_transporter_read(tuya_transporter_t t,uint8_t *buf,int bytes,int timeout_ms){
    (void)t;reads++;if((unsigned)timeout_ms>largest_poll)largest_poll=(unsigned)timeout_ms;
    if(stall_reads){clock_ms+=(unsigned)timeout_ms;return OPRT_RESOURCE_NOT_READY;}
    size_t left=wire_length-wire_pos,n=left<(size_t)bytes?left:(size_t)bytes;if(read_chunk&&n>read_chunk)n=read_chunk;
    memcpy(buf,wire+wire_pos,n);wire_pos+=n;clock_ms++;
    if(cancel_after_read){hh_cloud_http_background_idle(false);if(flip_busy_back)hh_cloud_http_background_idle(true);}
    return (int)n;
}
OPERATE_RET tuya_transporter_write(tuya_transporter_t t,uint8_t *buf,int bytes,int timeout_ms){(void)t;if((unsigned)timeout_ms>largest_poll)largest_poll=(unsigned)timeout_ms;writes++;if(sent_bytes+(size_t)bytes>=sizeof(sent))return -1;memcpy(sent+sent_bytes,buf,(size_t)bytes);sent_bytes+=(size_t)bytes;sent[sent_bytes]=0;clock_ms++;if(cancel_after_write)hh_cloud_http_background_idle(false);return bytes;}
tuya_tls_hander *tuya_tls_connect_create(void){tls_creates++;return (tuya_tls_hander*)&fake_tls;}
void tuya_tls_connect_destroy(tuya_tls_hander t){(void)t;tls_frees++;}
OPERATE_RET tuya_tls_config_set(tuya_tls_hander t,tuya_tls_config_t *cfg){(void)t;observed_tls=*cfg;return 0;}
OPERATE_RET tuya_tls_connect(tuya_tls_hander t,char *host,int port,int fd,int seconds){(void)t;(void)seconds;if(strcmp(host,"project.example")||port!=expected_port||fd!=7)failures++;return tls_fail?-1:0;}
OPERATE_RET tuya_tls_disconnect(tuya_tls_hander t){(void)t;tls_disconnects++;return 0;}
int tuya_tls_read(tuya_tls_hander t,uint8_t *buf,uint32_t bytes){(void)t;return observed_tls.f_recv(observed_tls.user_data,buf,bytes);}
int tuya_tls_write(tuya_tls_hander t,uint8_t *buf,uint32_t bytes){(void)t;return observed_tls.f_send(observed_tls.user_data,buf,bytes);}
static HH_CloudRequest request(void){HH_CloudRequest r={.method="GET",.host="project.example",.path="/rest/v1/test",.ca_pem="-----BEGIN CERTIFICATE-----\nSYNTHETIC\n-----END CERTIFICATE-----\n",.max_body=1024,.timeout_ms=8000};return r;}
static void reset(const char *response){
    if(live){printf("FAIL previous test leaked %u allocations\n",live);failures++;}
    allocations=frees=live=max_alloc=creates=destroys=closes=connects=tls_creates=tls_frees=tls_disconnects=0;
    clock_ms=reads=writes=largest_poll=socket_timeouts=fail_alloc_at=0;connect_fail=tls_fail=cancel_after_connect=cancel_after_read=cancel_after_write=flip_busy_back=stall_reads=false;
    expected_port=443;connected_port=0;
    wire=response;wire_length=strlen(wire);wire_pos=0;read_chunk=0;sent_bytes=0;sent[0]=logs[0]=0;memset(&observed_tls,0,sizeof(observed_tls));hh_cloud_http_background_idle(true);
}
static void real_parser_get_and_single_owner(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");HH_CloudRequest r=request();HH_CloudResponse s={0};
    CHECK(hh_cloud_http_request(&r,&s));CHECK(s.status==200&&s.body_bytes==2&&!memcmp(s.body,"[]",2));CHECK(s.storage&&s.body>=s.storage);
    CHECK(observed_tls.verify&&observed_tls.mode==TUYA_TLS_SERVER_CERT_MODE&&observed_tls.ca_cert==r.ca_pem&&observed_tls.ca_cert_size==(int)strlen(r.ca_pem)+1);
    CHECK(observed_tls.hostname&&!strcmp(observed_tls.hostname,r.host));CHECK(strstr(sent,"GET /rest/v1/test HTTP/1.1")&&connects==1&&tls_creates==1&&tls_frees==1);
    CHECK(largest_poll<=500&&socket_timeouts==2);hh_cloud_http_free(&s);hh_cloud_http_free(&s);CHECK(!live&&allocations==frees&&!s.storage&&!s.body);
}
static void patch_204_empty_body_is_success(void){
    reset("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");HH_CloudRequest r=request();r.method="PATCH";r.body=(const uint8_t*)"{}";r.body_bytes=2;r.max_body=0;HH_CloudResponse s={0};
    CHECK(hh_cloud_http_request(&r,&s));CHECK(s.status==204&&!s.body_bytes&&!s.body&&strstr(sent,"PATCH "));hh_cloud_http_free(&s);CHECK(!live);
}
static void real_chunked_parser_handles_fragmented_input(void){
    reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");read_chunk=3;HH_CloudRequest r=request();r.max_body=5;HH_CloudResponse s={0};
    CHECK(hh_cloud_http_request(&r,&s));CHECK(s.body_bytes==5&&!memcmp(s.body,"abcde",5));hh_cloud_http_free(&s);CHECK(!live);
}
static void body_limit_is_enforced_without_server_sized_allocation(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 1000000000\r\n\r\nx");HH_CloudRequest r=request();r.max_body=4;HH_CloudResponse s={0};
    CHECK(!hh_cloud_http_request(&r,&s));CHECK(max_alloc<=4100&&!live);hh_cloud_http_free(&s);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n12345");r=request();r.max_body=4;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live);
}
static void truncated_body_fails_and_releases_transport(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nshort");HH_CloudRequest r=request();HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&closes==1&&destroys==1&&tls_frees==1);
}
static void redirects_and_http_errors_do_not_follow_or_retry(void){
    reset("HTTP/1.1 302 Found\r\nLocation: https://evil.example/\r\nContent-Length: 1\r\n\r\nx");HH_CloudRequest r=request();HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==302&&connects==1);hh_cloud_http_free(&s);CHECK(!live);
    reset("HTTP/1.1 403 Forbidden\r\nContent-Length: 2\r\n\r\n{}");CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==403&&connects==1);hh_cloud_http_free(&s);CHECK(!live);
}
static void missing_ca_or_header_injection_is_rejected_before_connect(void){
    reset("");HH_CloudRequest r=request();HH_CloudResponse s={0};r.ca_pem=NULL;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!creates&&!allocations);
    r=request();HH_CloudHeader h={"Authorization","Bearer SYNTHETIC-SECRET\r\nx:y"};r.headers=&h;r.header_count=1;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!creates&&!allocations&&!strstr(logs,"SYNTHETIC-SECRET"));
    r=request();r.host="project.example@evil.example";CHECK(!hh_cloud_http_request(&r,&s));r=request();r.path="/x\r\ny";CHECK(!hh_cloud_http_request(&r,&s));CHECK(!creates);
}
static void foreground_gate_is_not_started_and_reentry_cannot_resume_old_io(void){
    reset("");hh_cloud_http_background_idle(false);HH_CloudRequest r=request();HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==-1&&!connects&&!creates&&!live);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");cancel_after_connect=true;CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==0&&!writes&&!live);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");cancel_after_read=flip_busy_back=true;CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==0&&!live);
}
static void no_data_obeys_overall_http_budget_and_short_polls(void){
    reset("");stall_reads=true;HH_CloudRequest r=request();r.timeout_ms=750;HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(clock_ms<=1000&&largest_poll<=500&&!live);
}
static void allocation_connect_and_tls_failures_release_once(void){
    HH_CloudRequest r=request();HH_CloudResponse s={0};
    for(unsigned a=1;a<=2;a++){reset("");fail_alloc_at=a;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&!connects);}
    reset("");connect_fail=true;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&closes==1&&destroys==1);
    reset("");tls_fail=true;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&closes==1&&destroys==1&&tls_frees==1);
}
static void long_auth_headers_fit_and_request_has_one_owner(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");char token[1201];memset(token,'A',1200);token[1200]=0;HH_CloudHeader h[]={{"apikey",token},{"Authorization",token},{"Content-Type","application/json"}};
    HH_CloudRequest r=request();r.headers=h;r.header_count=3;HH_CloudResponse s={0};CHECK(hh_cloud_http_request(&r,&s));CHECK(strstr(sent,token)&&allocations==2);hh_cloud_http_free(&s);CHECK(!live);
}
static void binary_body_and_close_delimited_body_are_exact(void){
    static const char binary[]="HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n\0\1\0\2";
    reset(binary);wire_length=sizeof(binary)-1;read_chunk=7;HH_CloudRequest r=request();r.max_body=4;HH_CloudResponse s={0};
    CHECK(hh_cloud_http_request(&r,&s));CHECK(s.body_bytes==4&&!memcmp(s.body,"\0\1\0\2",4));hh_cloud_http_free(&s);CHECK(!live);
    reset("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc");r=request();CHECK(hh_cloud_http_request(&r,&s));CHECK(s.body_bytes==3&&!memcmp(s.body,"abc",3));hh_cloud_http_free(&s);CHECK(!live);
}
static void managed_headers_and_invalid_caps_never_open_a_connection(void){
    HH_CloudRequest r;HH_CloudResponse s={0};const char *names[]={"Content-Length","transfer-encoding","hOsT"};
    for(unsigned i=0;i<3;i++){reset("");r=request();HH_CloudHeader h={names[i],"anything"};r.headers=&h;r.header_count=1;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!creates&&!allocations);}
    reset("");r=request();r.max_body=1024u*1024u;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!creates&&!allocations);
}
static void oversized_headers_and_chunks_stop_without_overrun(void){
    char large[7000];strcpy(large,"HTTP/1.1 200 OK\r\nX-Large: ");size_t n=strlen(large);memset(large+n,'A',5000);strcpy(large+n+5000,"\r\nContent-Length: 0\r\n\r\n");
    reset(large);HH_CloudRequest r=request();HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&max_alloc<=5120);
    reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nabcdef\r\n0\r\n\r\n");r.max_body=4;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&max_alloc<=4100);
}
static void cancellation_after_send_and_clock_wrap_remain_bounded(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");cancel_after_write=true;HH_CloudRequest r=request();HH_CloudResponse s={0};CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==0&&writes==1&&!reads&&!live);
    reset("");clock_ms=UINT32_MAX-100u;stall_reads=true;r.timeout_ms=750;unsigned initial=clock_ms;CHECK(!hh_cloud_http_request(&r,&s));CHECK((unsigned)(clock_ms-initial)<=1000&&!live);
}
static void explicit_plain_8000_uses_tcp_and_host_authority(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");expected_port=8000;
    HH_CloudRequest r=request();r.plain_http=true;r.port=8000;r.ca_pem=NULL;HH_CloudResponse s={0};
    bool ok=hh_cloud_http_request(&r,&s),body=s.status==200&&s.body_bytes==2&&!memcmp(s.body,"[]",2);
    hh_cloud_http_free(&s);CHECK(ok&&body);CHECK(connected_port==8000&&connects==1&&!tls_creates&&!tls_disconnects&&!tls_frees);
    CHECK(strstr(sent,"Host: project.example:8000\r\n")&&largest_poll<=250&&!live);
}
static void plain_default_80_omits_default_port(void){
    reset("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");expected_port=80;
    HH_CloudRequest r=request();r.plain_http=true;r.ca_pem=NULL;HH_CloudResponse s={0};
    bool ok=hh_cloud_http_request(&r,&s),empty=s.status==204&&!s.body&&s.body_bytes==0;
    hh_cloud_http_free(&s);CHECK(ok&&empty);CHECK(connected_port==80&&!tls_creates&&strstr(sent,"Host: project.example\r\n")&&!strstr(sent,"Host: project.example:80"));CHECK(!live);
}
static void plain_cannot_silently_accept_tls_ca_or_missing_explicit_mode(void){
    reset("");HH_CloudRequest r=request();r.plain_http=true;r.port=8000;HH_CloudResponse s={0};
    CHECK(!hh_cloud_http_request(&r,&s));CHECK(!allocations&&!connects&&!tls_creates);
    r.ca_pem="";CHECK(!hh_cloud_http_request(&r,&s));CHECK(!allocations&&!connects);
    r=request();r.ca_pem=NULL;r.port=8000;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!allocations&&!connects);
}
static void nondefault_https_port_keeps_verified_tls_and_host_only_sni(void){
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");expected_port=8443;
    HH_CloudRequest r=request();r.port=8443;HH_CloudResponse s={0};bool ok=hh_cloud_http_request(&r,&s);hh_cloud_http_free(&s);
    CHECK(ok);CHECK(connected_port==8443&&tls_creates==1&&observed_tls.verify&&observed_tls.port==8443);
    CHECK(!strcmp(observed_tls.hostname,"project.example")&&observed_tls.ca_cert==r.ca_pem&&strstr(sent,"Host: project.example:8443\r\n"));CHECK(!live);
}
static void plain_uses_same_chunk_limit_cancel_and_timeout_contract(void){
    reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");expected_port=8000;read_chunk=3;
    HH_CloudRequest r=request();r.plain_http=true;r.port=8000;r.ca_pem=NULL;r.max_body=5;HH_CloudResponse s={0};
    bool ok=hh_cloud_http_request(&r,&s),body=s.body_bytes==5&&!memcmp(s.body,"abcde",5);hh_cloud_http_free(&s);CHECK(ok&&body&&!tls_creates&&!live);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\ntoo large");expected_port=8000;CHECK(!hh_cloud_http_request(&r,&s));CHECK(!live&&max_alloc<=4101&&!tls_creates);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n[]");expected_port=8000;cancel_after_read=flip_busy_back=true;CHECK(!hh_cloud_http_request(&r,&s));CHECK(s.status==0&&!live&&!tls_creates);
    reset("");expected_port=8000;stall_reads=true;r.timeout_ms=750;CHECK(!hh_cloud_http_request(&r,&s));CHECK(clock_ms>=750&&clock_ms<=1000&&largest_poll<=250&&!live&&!tls_creates);
}
int main(void){
    RUN(real_parser_get_and_single_owner);RUN(patch_204_empty_body_is_success);RUN(real_chunked_parser_handles_fragmented_input);
    RUN(body_limit_is_enforced_without_server_sized_allocation);RUN(truncated_body_fails_and_releases_transport);RUN(redirects_and_http_errors_do_not_follow_or_retry);
    RUN(missing_ca_or_header_injection_is_rejected_before_connect);RUN(foreground_gate_is_not_started_and_reentry_cannot_resume_old_io);
    RUN(no_data_obeys_overall_http_budget_and_short_polls);RUN(allocation_connect_and_tls_failures_release_once);RUN(long_auth_headers_fit_and_request_has_one_owner);
    RUN(binary_body_and_close_delimited_body_are_exact);RUN(managed_headers_and_invalid_caps_never_open_a_connection);
    RUN(oversized_headers_and_chunks_stop_without_overrun);RUN(cancellation_after_send_and_clock_wrap_remain_bounded);
    RUN(explicit_plain_8000_uses_tcp_and_host_authority);RUN(plain_default_80_omits_default_port);
    RUN(plain_cannot_silently_accept_tls_ca_or_missing_explicit_mode);RUN(nondefault_https_port_keeps_verified_tls_and_host_only_sni);
    RUN(plain_uses_same_chunk_limit_cancel_and_timeout_contract);
    printf("%u failure(s)\n",failures);return failures?1:0;
}
