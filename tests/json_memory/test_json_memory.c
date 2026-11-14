/* Synthetic JSON only: compile the real app entry and SDK cJSON, not a clone. */
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {size_t bytes;unsigned pool;} Allocation;
static unsigned allocations[2],live[2],wrong_free,hook_calls,threads,failures;
static void *pool_malloc(size_t bytes,unsigned pool){
    Allocation *a=malloc(sizeof(*a)+bytes);if(!a)return NULL;
    a->bytes=bytes;a->pool=pool;allocations[pool]++;live[pool]++;return a+1;
}
static void pool_free(void *p,unsigned expected){
    if(!p)return;Allocation *a=(Allocation*)p-1;
    if(a->pool!=expected)wrong_free++;
    live[a->pool]--;free(a);
}
static void *tal_malloc(size_t n){return pool_malloc(n,0);}
static void tal_free(void *p){pool_free(p,0);}
static void *tal_psram_malloc(size_t n){return pool_malloc(n,1);}
static void tal_psram_free(void *p){pool_free(p,1);}
static void startup_hooks(cJSON_Hooks *hooks){hook_calls++;cJSON_InitHooks(hooks);}
static void check(int ok,const char *what){if(!ok){printf("FAIL %s\n",what);failures++;}}
static void roundtrip(void){
    const char *text="{\"sessionAttributes\":{\"tts.order.supports\":[{\"format\":\"mp3\",\"sampleRate\":16000}]}}";
    cJSON *root=cJSON_Parse(text);check(root!=NULL,"real SDK JSON parse succeeds");
    char *out=root?cJSON_PrintUnformatted(root):NULL;
    check(out&&strstr(out,"mp3"),"real SDK session-like JSON prints");
    cJSON_Delete(root);
    /* AI_VERSION=2 agent session/start paths release printed JSON with Free,
     * whose real tal_memory.h selects PSRAM under ENABLE_EXT_RAM. */
#if defined(ENABLE_EXT_RAM) && ENABLE_EXT_RAM == 1
    tal_psram_free(out);
#else
    tal_free(out);
#endif
}
typedef void *THREAD_HANDLE;
typedef struct {unsigned stackDepth,priority;const char *thrdname;} THREAD_CFG_T;
#define THREAD_PRIO_2 2
static void app_thread(void *p){(void)p;}
static int tal_thread_create_and_start(THREAD_HANDLE *h,void *a,void *b,void(*fn)(void*),void *arg,THREAD_CFG_T *cfg){
    (void)h;(void)a;(void)b;(void)fn;(void)arg;(void)cfg;
    check(hook_calls==1,"hooks installed once before first app thread");
    threads++;roundtrip();return 0;
}
#define cJSON_InitHooks startup_hooks
#include HH_STARTUP_SOURCE
#undef cJSON_InitHooks
int main(void){
    (void)tal_psram_malloc;(void)tal_psram_free;
    /* Replace libc with the counted native pool before the actual startup.
     * This checks routing against the SDK's expected allocation API, not a
     * hardware leak: T5's unified free may dispatch by pointer address. */
    cJSON_Hooks native={tal_malloc,tal_free};cJSON_InitHooks(&native);
    tuya_app_main();
    for(unsigned i=0;i<64;i++)roundtrip();
    check(threads==1,"actual entry creates app thread once");
    check(hook_calls==1,"hooks never switch during subsequent JSON use");
    check(wrong_free==0,"all SDK-style print buffers free in allocating pool");
    check(live[0]==0&&live[1]==0,"parse and print allocations fully released");
#if defined(ENABLE_EXT_RAM) && ENABLE_EXT_RAM == 1
    check(allocations[1]>0&&allocations[0]==0,"all JSON allocations route to PSRAM");
#else
    check(allocations[0]>0&&allocations[1]==0,"no-PSRAM build uses native fallback");
#endif
    printf("JSON memory ext_ram=%u %u failure(s)\n",(unsigned)ENABLE_EXT_RAM,failures);
    return failures?1:0;
}
