#include "homehub_cloud_http.h"
#include "tal_api.h"
#include "tal_network.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"
#include "core_http_client.h"
#include "http_parser.h"
#include "mbedtls/ssl.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CLOUD_HEADERS_BYTES 4096u
#define CLOUD_MAX_BODY ((1024u*1024u)-CLOUD_HEADERS_BYTES-1u)
#define CLOUD_IO_POLL_MS 250u
#define CLOUD_TIMEOUT_MS 8000u
static volatile bool background_idle;
static volatile uint32_t background_epoch;
typedef struct {
    tuya_transporter_t tcp;
    tuya_tls_hander tls;
    uint32_t epoch,started_ms,budget_ms;
    bool plain_http;
} CloudIo;

void hh_cloud_http_background_idle(bool idle)
{
    /* The application is the sole writer. An intervening foreground interval
     * invalidates an old request even if the device becomes idle again. */
    if(!idle){background_idle=false;background_epoch++;}
    else background_idle=true;
}
static uint32_t milliseconds(void){return (uint32_t)tal_system_get_millisecond();}
static bool allowed(const CloudIo *io)
{
    return background_idle && background_epoch==io->epoch &&
        (uint32_t)(milliseconds()-io->started_ms)<io->budget_ms;
}
static int poll_ms(const CloudIo *io)
{
    uint32_t remaining=io->budget_ms-(uint32_t)(milliseconds()-io->started_ms);
    return (int)(remaining<CLOUD_IO_POLL_MS?remaining:CLOUD_IO_POLL_MS);
}
static int bio_recv(void *context,uint8_t *data,size_t bytes)
{
    CloudIo *io=context;if(!allowed(io))return -1;
    int count=(int)(bytes>4096u?4096u:bytes);
    int rc=tuya_transporter_read(io->tcp,data,count,poll_ms(io));
    if(!allowed(io))return -1;
    return rc==OPRT_RESOURCE_NOT_READY?MBEDTLS_ERR_SSL_WANT_READ:rc;
}
static int bio_send(void *context,const uint8_t *data,size_t bytes)
{
    CloudIo *io=context;if(!allowed(io))return -1;
    int count=(int)(bytes>1024u?1024u:bytes);
    int rc=tuya_transporter_write(io->tcp,(uint8_t*)data,count,poll_ms(io));
    if(!allowed(io))return -1;
    return rc==OPRT_RESOURCE_NOT_READY?MBEDTLS_ERR_SSL_WANT_WRITE:rc;
}
static int32_t receive_http(NetworkContext_t *context,void *data,size_t bytes)
{
    CloudIo *io=(CloudIo*)context;
    while(allowed(io)){
        uint32_t count=(uint32_t)(bytes>4096u?4096u:bytes);
        int rc=io->plain_http?bio_recv(io,data,count):tuya_tls_read(io->tls,data,count);
        if(!allowed(io))return -1;
        if(rc!=MBEDTLS_ERR_SSL_WANT_READ && rc!=MBEDTLS_ERR_SSL_WANT_WRITE)return rc;
        tal_system_sleep(1);
    }
    return -1;
}
static int32_t send_http(NetworkContext_t *context,const void *data,size_t bytes)
{
    CloudIo *io=(CloudIo*)context;if(!allowed(io))return -1;
    uint32_t count=(uint32_t)(bytes>1024u?1024u:bytes);
    int rc=io->plain_http?bio_send(io,data,count):tuya_tls_write(io->tls,(uint8_t*)data,count);
    return allowed(io)?rc:-1;
}
static size_t bounded_length(const char *text,size_t maximum)
{
    if(!text)return SIZE_MAX;
    size_t n=0;while(n<=maximum && text[n])n++;
    return n<=maximum?n:SIZE_MAX;
}
static bool token_char(unsigned char c)
{
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("!#$%&'*+-.^_`|~",c)!=NULL;
}
static bool header_is(const char *a,const char *b)
{
    while(*a && *b){
        unsigned char left=(unsigned char)*a++,right=(unsigned char)*b++;
        if(left>='A'&&left<='Z')left=(unsigned char)(left+'a'-'A');
        if(right>='A'&&right<='Z')right=(unsigned char)(right+'a'-'A');
        if(left!=right)return false;
    }
    return !*a && !*b;
}
static bool valid_request(const HH_CloudRequest *r)
{
    if(!r || r->header_count>12u || (r->header_count&&!r->headers) ||
       r->max_body>CLOUD_MAX_BODY || r->body_bytes>CLOUD_MAX_BODY ||
       (r->body_bytes&&!r->body))return false;
    size_t method=bounded_length(r->method,8),host=bounded_length(r->host,253),path=bounded_length(r->path,2048);
    if(method==SIZE_MAX||host==SIZE_MAX||path==SIZE_MAX||!host||!path)return false;
    if(r->plain_http){
        /* A CA-bearing TLS request must never silently become plaintext. */
        if(r->ca_pem)return false;
    }else{
        size_t ca=bounded_length(r->ca_pem,32768);
        if(ca==SIZE_MAX||!ca)return false;
    }
    if(strcmp(r->method,"GET")&&strcmp(r->method,"POST")&&strcmp(r->method,"PATCH")&&
       strcmp(r->method,"PUT")&&strcmp(r->method,"DELETE")&&strcmp(r->method,"HEAD"))return false;
    if(r->path[0]!='/' || (path>1u&&r->path[1]=='/'))return false;
    for(size_t i=0;i<path;i++)if((unsigned char)r->path[i]<33u||(unsigned char)r->path[i]>126u||r->path[i]=='\\')return false;
    size_t label=0;
    for(size_t i=0;i<host;i++){
        unsigned char c=(unsigned char)r->host[i];
        if(c=='.'){if(!label||r->host[i-1]=='-')return false;label=0;}
        else {if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-')||(!label&&c=='-')||++label>63u)return false;}
    }
    if(!label||r->host[host-1]=='-')return false;
    /* Prevalidate before SDK serialization: its CR/LF error path otherwise
     * logs the complete offending value, which could contain a credential. */
    size_t total=method+host+path+128u;
    for(size_t i=0;i<r->header_count;i++){
        size_t key=bounded_length(r->headers[i].key,64),value=bounded_length(r->headers[i].value,2048);
        if(!key||key==SIZE_MAX||value==SIZE_MAX)return false;
        if(header_is(r->headers[i].key,"Host") ||
           header_is(r->headers[i].key,"Content-Length") ||
           header_is(r->headers[i].key,"Transfer-Encoding"))return false;
        for(size_t n=0;n<key;n++)if(!token_char((unsigned char)r->headers[i].key[n]))return false;
        for(size_t n=0;n<value;n++)if((unsigned char)r->headers[i].value[n]<32u||(unsigned char)r->headers[i].value[n]>126u)return false;
        total+=key+value+4u;
    }
    return total<CLOUD_HEADERS_BYTES-64u;
}
typedef struct {
    uint8_t *body;size_t bytes,maximum;
    bool headers_done,complete,head;
} ParsedResponse;
static int parsed_begin(http_parser *parser)
{
    ParsedResponse *result=parser->data;
    return result->complete?-1:0;
}
static int parsed_headers(http_parser *parser)
{
    ParsedResponse *result=parser->data;
    if(parser->nread>CLOUD_HEADERS_BYTES)return -1;
    if(!(parser->flags&F_CHUNKED) && parser->content_length!=ULLONG_MAX &&
       parser->content_length>result->maximum && !result->head &&
       parser->status_code!=204 && parser->status_code!=304)return -1;
    result->headers_done=true;
    return result->head?1:0;
}
static int parsed_body(http_parser *parser,const char *data,size_t bytes)
{
    ParsedResponse *result=parser->data;
    if(bytes>result->maximum-result->bytes)return -1;
    memcpy(result->body+result->bytes,data,bytes);result->bytes+=bytes;return 0;
}
static int parsed_complete(http_parser *parser)
{
    ParsedResponse *result=parser->data;result->complete=true;return 0;
}
static bool send_all(CloudIo *io,const uint8_t *data,size_t bytes)
{
    size_t sent=0;
    while(sent<bytes && allowed(io)){
        int32_t rc=send_http((NetworkContext_t*)io,data+sent,bytes-sent);
        if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE){tal_system_sleep(1);continue;}
        if(rc<=0||(size_t)rc>bytes-sent)return false;
        sent+=(size_t)rc;
    }
    return sent==bytes && allowed(io);
}
static bool receive_bounded(CloudIo *io,uint8_t *storage,size_t maximum,bool head,
                            size_t *body_bytes,int *status)
{
    ParsedResponse result={.body=storage+CLOUD_HEADERS_BYTES,.maximum=maximum,.head=head};
    http_parser parser;http_parser_init(&parser,HTTP_RESPONSE);parser.data=&result;
    http_parser_settings settings={0};settings.on_message_begin=parsed_begin;
    settings.on_headers_complete=parsed_headers;settings.on_body=parsed_body;
    settings.on_message_complete=parsed_complete;
    size_t header_bytes=0;uint32_t header_tail=0;bool header_end=false;
    while(!result.complete && allowed(io)){
        int32_t rc=receive_http((NetworkContext_t*)io,storage,CLOUD_HEADERS_BYTES);
        if(rc<0)return false;
        /* The SDK parser resets nread before message completion, and its
         * headers-complete callback can see the preceding read's nread.
         * Bound the wire header independently, including fragmented lines. */
        if(!header_end){
            for(size_t i=0;i<(size_t)rc;i++){
                if(++header_bytes>CLOUD_HEADERS_BYTES)return false;
                header_tail=(header_tail<<8)|storage[i];
                if(header_tail==0x0d0a0d0au){header_end=true;break;}
            }
        }
        size_t used=http_parser_execute(&parser,&settings,(const char*)storage,(size_t)rc);
        if(HTTP_PARSER_ERRNO(&parser)!=HPE_OK || used!=(size_t)rc ||
           (!result.headers_done && parser.nread>CLOUD_HEADERS_BYTES))return false;
        if(!rc&&!result.complete)return false;
    }
    if(!result.complete||!allowed(io))return false;
    *body_bytes=result.bytes;*status=parser.status_code;return true;
}
void hh_cloud_http_free(HH_CloudResponse *response)
{
    if(!response)return;
    if(response->storage)HTTP_FREE(response->storage);
    memset(response,0,sizeof(*response));
}
bool hh_cloud_http_request(const HH_CloudRequest *request,HH_CloudResponse *response)
{
    if(!response)return false;
    memset(response,0,sizeof(*response));
    if(!background_idle){response->status=-1;return false;}
    if(!valid_request(request))return false;
    CloudIo io={.epoch=background_epoch,.started_ms=milliseconds(),.plain_http=request->plain_http,
        .budget_ms=request->timeout_ms&&request->timeout_ms<CLOUD_TIMEOUT_MS?request->timeout_ms:CLOUD_TIMEOUT_MS};
    unsigned default_port=request->plain_http?80u:443u;
    unsigned port=request->port?request->port:default_port;
    uint8_t *headers=NULL,*storage=NULL;bool ok=false;
    headers=HTTP_MALLOC(CLOUD_HEADERS_BYTES);
    if(!headers)goto done;
    size_t capacity=CLOUD_HEADERS_BYTES+request->max_body;
    storage=HTTP_MALLOC(capacity);
    if(!storage)goto done;
    char authority[260];const char *host_header=request->host;
    if(port!=default_port){
        int length=snprintf(authority,sizeof(authority),"%s:%u",request->host,port);
        if(length<0 || (size_t)length>=sizeof(authority))goto done;
        host_header=authority;
    }
    HTTPRequestHeaders_t outgoing={.pBuffer=headers,.bufferLen=CLOUD_HEADERS_BYTES};
    HTTPRequestInfo_t info={.pMethod=request->method,.methodLen=strlen(request->method),
        .pHost=host_header,.hostLen=strlen(host_header),.pPath=request->path,.pathLen=strlen(request->path)};
    if(HTTPClient_InitializeRequestHeaders(&outgoing,&info)!=HTTPSuccess)goto done;
    for(size_t i=0;i<request->header_count;i++)
        if(HTTPClient_AddHeader(&outgoing,request->headers[i].key,strlen(request->headers[i].key),
            request->headers[i].value,strlen(request->headers[i].value))!=HTTPSuccess)goto done;
    if(request->body_bytes){
        char length[24];snprintf(length,sizeof(length),"%lu",(unsigned long)request->body_bytes);
        if(HTTPClient_AddHeader(&outgoing,"Content-Length",14,length,strlen(length))!=HTTPSuccess)goto done;
    }
    if(!allowed(&io)){if(!background_idle||background_epoch!=io.epoch)response->status=-1;goto done;}
    io.tcp=tuya_transporter_create(TRANSPORT_TYPE_TCP,NULL);
    if(!io.tcp)goto done;
    tuya_tcp_config_t tcp_config={.isBlock=1,.sendTimeoutMs=CLOUD_IO_POLL_MS,.recvTimeoutMs=CLOUD_IO_POLL_MS};
    if(tuya_transporter_ctrl(io.tcp,TUYA_TRANSPORTER_SET_TCP_CONFIG,&tcp_config)!=OPRT_OK)goto done;
    if(!allowed(&io)){if(!background_idle||background_epoch!=io.epoch)response->status=-1;goto done;}
    /* SDK DNS/connect and its certificate handshake are synchronous. They do
     * not expose cooperative cancellation; check the budget on both sides.
     * HTTP and post-handshake BIO I/O below are short, cancellable polls. */
    if(tuya_transporter_connect(io.tcp,request->host,(int)port,(int)io.budget_ms)!=OPRT_OK||!allowed(&io))goto done;
    int fd=-1;
    if(tuya_transporter_ctrl(io.tcp,TUYA_TRANSPORTER_GET_TCP_SOCKET,&fd)!=OPRT_OK||fd<0)goto done;
    if(!request->plain_http){
        io.tls=tuya_tls_connect_create();if(!io.tls)goto done;
        tuya_tls_config_t tls_config={.mode=TUYA_TLS_SERVER_CERT_MODE,.verify=true,
            .hostname=(char*)request->host,.port=(int)port,.ca_cert=(char*)request->ca_pem,
            .ca_cert_size=(int)strlen(request->ca_pem)+1,.f_send=bio_send,.f_recv=bio_recv,.user_data=&io};
        if(tuya_tls_config_set(io.tls,&tls_config)!=OPRT_OK)goto done;
        /* This SDK argument is seconds despite the transport's millisecond API. */
        if(tuya_tls_connect(io.tls,(char*)request->host,(int)port,fd,1)!=OPRT_OK||!allowed(&io))goto done;
    }
    if(tal_net_set_timeout(fd,CLOUD_IO_POLL_MS,TRANS_SEND)!=OPRT_OK ||
       tal_net_set_timeout(fd,CLOUD_IO_POLL_MS,TRANS_RECV)!=OPRT_OK)goto done;
    /* The SDK's forked HTTPClient_Send chunk callback writes through NULL.
     * Keep its header serializer, and use the same SDK parser with explicitly
     * bounded body callbacks instead. No server length drives allocation. */
    if(!send_all(&io,outgoing.pBuffer,outgoing.headersLen) ||
       (request->body_bytes&&!send_all(&io,request->body,request->body_bytes)))goto done;
    size_t body_bytes=0;int status=0;
    if(!receive_bounded(&io,storage,request->max_body,!strcmp(request->method,"HEAD"),&body_bytes,&status))goto done;
    response->storage=storage;response->body=body_bytes?storage+CLOUD_HEADERS_BYTES:NULL;storage=NULL;
    response->body_bytes=body_bytes;response->status=status;
    ok=response->status>=200&&response->status<300;
done:
    if(io.tls){(void)tuya_tls_disconnect(io.tls);tuya_tls_connect_destroy(io.tls);}
    if(io.tcp){(void)tuya_transporter_close(io.tcp);(void)tuya_transporter_destroy(io.tcp);}
    if(headers)HTTP_FREE(headers);
    if(storage)HTTP_FREE(storage);
    return ok;
}
