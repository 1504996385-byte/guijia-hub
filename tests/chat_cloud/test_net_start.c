/* Compile the actual HTTP service source; fakes never execute its worker. */
#include "stubs/net_start_fake.h"
HH_DeviceConfig hh_device;
#include "../../firmware/src/homehub_net.c"
static int failures;
#define CHECK(x) do{if(!(x)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);failures++;return;}}while(0)
#define RUN(x) do{int before=failures;x();if(before==failures)printf("PASS %s\n",#x);}while(0)
static void reset_start(const char *server,const char *token,const char *ssid)
{
    memset(&hh_device,0,sizeof(hh_device));
    if(server)snprintf(hh_device.server_url,sizeof(hh_device.server_url),"%s",server);
    if(token)snprintf(hh_device.device_token,sizeof(hh_device.device_token),"%s",token);
    if(ssid)snprintf(hh_device.wifi_ssid,sizeof(hh_device.wifi_ssid),"%s",ssid);
    net_jobs=NULL;configured=tls=false;ca=NULL;ca_size=0;
    host[0]=base_path[0]=authorization[0]=0;port=0;
    net_test_queue_creates=net_test_thread_creates=0;
    net_test_http_requests=0;net_test_http_path[0]=0;
}
static void empty_http_configuration_allocates_nothing(void)
{
    reset_start(NULL,NULL,NULL);hh_net_start();
    CHECK(net_test_queue_creates==0&&net_test_thread_creates==0&&net_jobs==NULL);
}
static void missing_token_allocates_nothing(void)
{
    reset_start("http://example.test/api",NULL,"configured-ssid");hh_net_start();
    CHECK(net_test_queue_creates==0&&net_test_thread_creates==0&&net_jobs==NULL);
}
static void missing_server_allocates_nothing(void)
{
    reset_start(NULL,"test-token","configured-ssid");hh_net_start();
    CHECK(net_test_queue_creates==0&&net_test_thread_creates==0&&net_jobs==NULL);
}
static void invalid_url_allocates_nothing(void)
{
    const char *urls[]={"ftp://example.test","http://","http://example.test:0","http://example.test:65536","http://user@example.test"};
    for(unsigned i=0;i<sizeof(urls)/sizeof(urls[0]);i++){
        reset_start(urls[i],"test-token",NULL);hh_net_start();
        CHECK(net_test_queue_creates==0&&net_test_thread_creates==0&&net_jobs==NULL);
    }
}
static void valid_http_starts_without_explicit_ssid(void)
{
    reset_start("http://example.test/api","test-token",NULL);hh_net_start();
    CHECK(net_test_queue_creates==1&&net_test_thread_creates==1&&net_jobs!=NULL&&configured);
}
static void valid_http_starts_with_explicit_ssid(void)
{
    reset_start("http://example.test:8080/api","test-token","configured-ssid");hh_net_start();
    CHECK(net_test_queue_creates==1&&net_test_thread_creates==1&&net_jobs!=NULL&&configured);
}
static void leave_greeting_refresh_does_not_send_an_http_request(void)
{
    HH_Action a={0};reset_start("http://example.test/api","test-token",NULL);hh_net_start();
    a.type=HH_ACT_KEY_EVENT;a.trigger=HH_LEAVE;a.recipient=1;refresh_greeting(&a);
    CHECK(net_test_http_requests==0);
}
static void arrival_greeting_refresh_still_sends_an_http_request(void)
{
    HH_Action a={0};reset_start("http://example.test/api","test-token",NULL);hh_net_start();
    a.type=HH_ACT_KEY_EVENT;a.trigger=HH_ARRIVE;a.recipient=1;refresh_greeting(&a);
    CHECK(net_test_http_requests==1);
    CHECK(strstr(net_test_http_path,"/api/device/greeting?recipient=1&event=arrive")!=NULL);
}
int main(void)
{
    RUN(empty_http_configuration_allocates_nothing);RUN(missing_token_allocates_nothing);
    RUN(missing_server_allocates_nothing);RUN(invalid_url_allocates_nothing);
    RUN(valid_http_starts_without_explicit_ssid);RUN(valid_http_starts_with_explicit_ssid);
    RUN(leave_greeting_refresh_does_not_send_an_http_request);
    RUN(arrival_greeting_refresh_still_sends_an_http_request);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
