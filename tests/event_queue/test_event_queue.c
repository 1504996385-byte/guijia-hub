#include "homehub_event_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct{unsigned size,capacity,head,count;uint8_t *data;}FakeQueue;
static unsigned failures,create_calls,queue_live,queue_frees,post_calls,fetch_calls,psram_calls,psram_frees;
static size_t native_payload,psram_live,last_psram_bytes;
static unsigned fail_create_at,fail_seed_post_at,fail_posts;
static bool fail_allocation,create_error_with_handle,overwrite_returned_slot;
static QUEUE_HANDLE fail_post_queue,watch_return_queue,last_fetch_queue;
static uint32_t last_fetch_timeout;
static HH_Event *watched_output;static HH_Event expected_output;
#define CHECK(v) do{if(!(v)){printf("FAIL %s:%d %s\n",__func__,__LINE__,#v);failures++;return;}}while(0)
#define RUN(f) do{unsigned before=failures;f();printf("%s %s\n",before==failures?"PASS":"FAIL",#f);}while(0)
void *tkl_system_psram_malloc(size_t n){psram_calls++;last_psram_bytes=n;if(fail_allocation)return NULL;void *p=malloc(n);if(p)psram_live+=n;return p;}
void tkl_system_psram_free(void*p){if(!p)return;if(queue_live)failures++;free(p);psram_live=0;psram_frees++;}
int tal_queue_create_init(QUEUE_HANDLE *handle,int size,int capacity){
    create_calls++;*handle=NULL;
    if(create_calls==fail_create_at&&!create_error_with_handle)return -1;
    FakeQueue*q=calloc(1,sizeof(*q));q->size=(unsigned)size;q->capacity=(unsigned)capacity;q->data=calloc((size_t)size,(size_t)capacity);
    if(size!=(int)sizeof(HH_Event*)||capacity!=64)failures++;
    queue_live++;native_payload+=(size_t)size*(size_t)capacity;*handle=q;
    return create_calls==fail_create_at?-1:0;
}
int tal_queue_post(QUEUE_HANDLE handle,void*data,uint32_t timeout){
    (void)timeout;post_calls++;FakeQueue*q=handle;if(!q||!data)return -1;
    if(post_calls==fail_seed_post_at)return -1;
    if(handle==fail_post_queue&&fail_posts){fail_posts--;return -1;}
    if(q->count==q->capacity)return -1;
    if(handle==watch_return_queue&&watched_output&&memcmp(watched_output,&expected_output,sizeof(expected_output)))failures++;
    memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,data,q->size);q->count++;
    if(handle==watch_return_queue&&overwrite_returned_slot){HH_Event *slot=*(HH_Event**)data;memset(slot,0xa5,sizeof(*slot));}
    return 0;
}
int tal_queue_fetch(QUEUE_HANDLE handle,void*data,uint32_t timeout){
    fetch_calls++;last_fetch_queue=handle;last_fetch_timeout=timeout;FakeQueue*q=handle;
    if(!q||!data||!q->count)return -1;
    memcpy(data,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;return 0;
}
void tal_queue_free(QUEUE_HANDLE handle){if(!handle)return;FakeQueue*q=handle;native_payload-=(size_t)q->size*q->capacity;free(q->data);free(q);queue_live--;queue_frees++;}
#include "../../firmware/src/homehub_event_queue.c"

static void reset(void){
    if(event_queue.pending)tal_queue_free(event_queue.pending);
    if(event_queue.free_slots)tal_queue_free(event_queue.free_slots);
    if(event_queue.pool)tkl_system_psram_free(event_queue.pool);
    memset(&event_queue,0,sizeof(event_queue));
    create_calls=queue_live=queue_frees=post_calls=fetch_calls=psram_calls=psram_frees=0;
    native_payload=psram_live=last_psram_bytes=0;fail_create_at=fail_seed_post_at=fail_posts=0;
    fail_allocation=create_error_with_handle=overwrite_returned_slot=false;
    fail_post_queue=watch_return_queue=last_fetch_queue=NULL;watched_output=NULL;last_fetch_timeout=0;
}
static HH_Event value(unsigned n){HH_Event e;memset(&e,0,sizeof(e));e.type=EV_CHAT_TEXT;e.action.type=HH_ACT_UI;e.action.chat_turn=n+1;e.action.recipient=(uint8_t)(n%2);e.ok=(n%2)!=0;snprintf(e.text,sizeof(e.text),"synthetic event %u",n);return e;}
static void initialization_preallocates_only_pool_and_pointer_queue_payloads(void){reset();CHECK(hh_event_queue_init());CHECK(queue_live==2&&create_calls==2&&psram_calls==1);CHECK(last_psram_bytes==64u*sizeof(HH_Event));CHECK(native_payload==128u*sizeof(HH_Event*));CHECK(hh_event_queue_init()&&create_calls==2&&psram_calls==1);printf("pool=%u native_pointer_payload_host=%u event=%u slots=64\n",(unsigned)last_psram_bytes,(unsigned)native_payload,(unsigned)sizeof(HH_Event));}
static void sixty_four_deep_copied_events_are_fifo_and_pool_is_reusable(void){
    reset();CHECK(hh_event_queue_init());HH_Event expected[64],e,out;
    for(unsigned i=0;i<64;i++){e=value(i);expected[i]=e;CHECK(hh_event_try_post(&e,0));memset(&e,0xee,sizeof(e));}
    e=value(65);CHECK(!hh_event_try_post(&e,27));CHECK(last_fetch_queue==event_queue.free_slots&&last_fetch_timeout==27);
    for(unsigned i=0;i<64;i++){CHECK(hh_event_fetch(&out,91));CHECK(!memcmp(&out,&expected[i],sizeof(out)));}
    CHECK(!hh_event_fetch(&out,0));CHECK(hh_event_try_post(&e,0)&&hh_event_fetch(&out,0));CHECK(!memcmp(&e,&out,sizeof(e)));CHECK(psram_calls==1&&psram_frees==0);
}
static void fetch_copies_before_slot_return_or_immediate_slot_reuse(void){reset();CHECK(hh_event_queue_init());HH_Event in=value(7),out;CHECK(hh_event_try_post(&in,0));expected_output=in;watched_output=&out;watch_return_queue=event_queue.free_slots;overwrite_returned_slot=true;memset(&out,0,sizeof(out));CHECK(hh_event_fetch(&out,0));CHECK(!memcmp(&out,&in,sizeof(out)));}
static void failed_pending_post_returns_the_claimed_slot_without_losing_capacity(void){reset();CHECK(hh_event_queue_init());HH_Event e=value(1);fail_post_queue=event_queue.pending;fail_posts=1;CHECK(!hh_event_try_post(&e,0));CHECK(((FakeQueue*)event_queue.free_slots)->count==64&&((FakeQueue*)event_queue.pending)->count==0);for(unsigned i=0;i<64;i++)CHECK(hh_event_try_post(&e,0));CHECK(!hh_event_try_post(&e,0));}
static void transient_free_return_failure_preserves_both_delivery_and_capacity(void){reset();CHECK(hh_event_queue_init());HH_Event e=value(2),out;CHECK(hh_event_try_post(&e,0));fail_post_queue=event_queue.free_slots;fail_posts=1;CHECK(hh_event_fetch(&out,0)&&!memcmp(&e,&out,sizeof(e)));CHECK(((FakeQueue*)event_queue.free_slots)->count==64);fail_post_queue=event_queue.pending;fail_posts=1;CHECK(!hh_event_try_post(&e,0));CHECK(((FakeQueue*)event_queue.free_slots)->count==64);}
static void null_and_uninitialized_access_do_not_consume_or_modify_events(void){reset();HH_Event e=value(3),out= e;CHECK(!hh_event_try_post(&e,0)&&!hh_event_fetch(&out,0));CHECK(!memcmp(&e,&out,sizeof(e)));CHECK(hh_event_queue_init());unsigned calls=fetch_calls;CHECK(!hh_event_try_post(NULL,0)&&!hh_event_fetch(NULL,0));CHECK(fetch_calls==calls);CHECK(hh_event_try_post(&e,0));CHECK(!hh_event_fetch(NULL,0));CHECK(hh_event_fetch(&out,123)&&!memcmp(&e,&out,sizeof(e)));}
static void allocation_failure_cleans_and_can_retry(void){reset();fail_allocation=true;CHECK(!hh_event_queue_init());CHECK(psram_calls==1&&!psram_live&&!queue_live&&!native_payload&&!create_calls);fail_allocation=false;CHECK(hh_event_queue_init());CHECK(queue_live==2&&psram_live==64u*sizeof(HH_Event));}
static void either_queue_creation_failure_releases_all_partial_resources(void){for(unsigned at=1;at<=2;at++)for(unsigned partial=0;partial<2;partial++){reset();fail_create_at=at;create_error_with_handle=partial!=0;CHECK(!hh_event_queue_init());CHECK(create_calls==at&&!queue_live&&!native_payload&&!psram_live&&psram_frees==1);fail_create_at=0;CHECK(hh_event_queue_init());CHECK(queue_live==2);}}
static void initialization_seed_failure_deletes_nonempty_queues_before_pool(void){const unsigned points[]={1,32,64};for(unsigned i=0;i<3;i++){reset();fail_seed_post_at=points[i];CHECK(!hh_event_queue_init());CHECK(!queue_live&&!native_payload&&!psram_live&&queue_frees==2&&psram_frees==1);fail_seed_post_at=0;CHECK(hh_event_queue_init());CHECK(((FakeQueue*)event_queue.free_slots)->count==64);}}
int main(void){RUN(initialization_preallocates_only_pool_and_pointer_queue_payloads);RUN(sixty_four_deep_copied_events_are_fifo_and_pool_is_reusable);RUN(fetch_copies_before_slot_return_or_immediate_slot_reuse);RUN(failed_pending_post_returns_the_claimed_slot_without_losing_capacity);RUN(transient_free_return_failure_preserves_both_delivery_and_capacity);RUN(null_and_uninitialized_access_do_not_consume_or_modify_events);RUN(allocation_failure_cleans_and_can_retry);RUN(either_queue_creation_failure_releases_all_partial_resources);RUN(initialization_seed_failure_deletes_nonempty_queues_before_pool);reset();printf("%u failure(s)\n",failures);return failures?1:0;}
