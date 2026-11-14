/* Execute the actual board storage source against a deterministic host TKL.
 * This verifies error handling, not FAT/SD power loss or Tuya hardware APIs. */
#include "../../firmware/core/homehub_core.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#endif

#define HOMEHUB_PLATFORM_H
#define HH_ROOT "/sdcard/homehub"
#define HH_WAV_MAX (960000u+44u)
#define OPRT_OK 0
#define DEV_SDCARD 1
#define QUEUE_WAIT_FOREVER 0xffffffffu
#define THREAD_PRIO_2 2
#define PR_ERR(...) ((void)0)
typedef FILE *TUYA_FILE;
typedef bool BOOL_T;
typedef void *TUYA_DIR;
typedef void *MUTEX_HANDLE;
typedef void *THREAD_HANDLE;
typedef struct { unsigned stackDepth,priority; const char *thrdname; } THREAD_CFG_T;
typedef struct { void *items[16]; unsigned head,count,capacity; size_t item_size; } FakeQueue;
typedef FakeQueue *QUEUE_HANDLE;
typedef struct { char wifi_ssid[33],wifi_password[65],server_url[192],device_token[129],tuya_product_id[33]; bool chat_enabled,pir_enabled; uint8_t volume; } HH_DeviceConfig;
#ifndef HH_TEST_EVENT_EXTRA_TYPES
#define HH_TEST_EVENT_EXTRA_TYPES
#endif
#ifndef HH_TEST_EVENT_EXTRA_FIELDS
#define HH_TEST_EVENT_EXTRA_FIELDS
#endif
typedef enum { EV_SNAPSHOT_SAVED,EV_RECORD_STARTED,EV_RECORD_SAVED,EV_PLAY_FINISHED,EV_GREETING_READY HH_TEST_EVENT_EXTRA_TYPES } HH_EventType;
typedef struct { HH_EventType type; bool ok; HH_Action action; HH_TEST_EVENT_EXTRA_FIELDS } HH_Event;

static char runtime_root[512];
static unsigned rename_failures,fsync_failures,close_failures,fsync_calls,close_calls;
static unsigned event_count,event_failures;
static HH_Event latest_event;
static HH_Event event_history[64];
static bool allocation_failure;
static FILE *sync_file;
static jmp_buf worker_done;
static FakeQueue fake_queue;
static unsigned diagnostic_rng,diagnostic_logs;
static int diagnostic_stat_error;
static char diagnostic_log[4][256];
static void storage_diag_log(const char *format,...){
    if(diagnostic_logs>=4)return;va_list args;va_start(args,format);
    vsnprintf(diagnostic_log[diagnostic_logs++],sizeof(diagnostic_log[0]),format,args);va_end(args);
}
#define HH_STORAGE_DIAG storage_diag_log

static void host_path(const char *input,char *output,size_t size) {
    const char *suffix=input;
    if(strncmp(input,HH_ROOT,strlen(HH_ROOT))==0) suffix=input+strlen(HH_ROOT);
    else if(strncmp(input,"/sdcard",7)==0)suffix=input+7;
    snprintf(output,size,"%s%s",runtime_root,suffix);
}
static int tal_mutex_create_init(MUTEX_HANDLE *mutex){*mutex=(void*)1;return 0;}
static int tal_mutex_lock(MUTEX_HANDLE mutex){(void)mutex;return 0;}
static int tal_mutex_unlock(MUTEX_HANDLE mutex){(void)mutex;return 0;}
static int tal_queue_create_init(QUEUE_HANDLE *queue,size_t item_size,unsigned capacity){memset(&fake_queue,0,sizeof(fake_queue));fake_queue.item_size=item_size;fake_queue.capacity=capacity<16?capacity:16;*queue=&fake_queue;return 0;}
static int tal_queue_post(QUEUE_HANDLE queue,void *item,unsigned wait){(void)wait;if(!queue||queue->count==queue->capacity)return -1;memcpy(&queue->items[(queue->head+queue->count)%16],item,queue->item_size);queue->count++;return 0;}
static int tal_queue_fetch(QUEUE_HANDLE queue,void *item,unsigned wait){(void)wait;if(!queue||!queue->count)longjmp(worker_done,1);memcpy(item,&queue->items[queue->head],queue->item_size);queue->head=(queue->head+1)%16;queue->count--;return 0;}
static int tal_thread_create_and_start(THREAD_HANDLE *thread,void*a,void*b,void(*function)(void*),void*ctx,THREAD_CFG_T*cfg){(void)thread;(void)a;(void)b;(void)function;(void)ctx;(void)cfg;return 0;}
static int tkl_fs_mount(const char *root,int kind){(void)root;(void)kind;return 0;}
static int tkl_fs_mkdir(const char *path){(void)path;return 0;}
static int storage_probe_exists(const char *path,BOOL_T *exists){
    char mapped[768];struct stat info;host_path(path,mapped,sizeof(mapped));*exists=false;
    if(diagnostic_stat_error)return diagnostic_stat_error;
    if(stat(mapped,&info)==0){*exists=true;return 0;}return errno==ENOENT?4:-1;
}
static int tkl_dir_open(const char *path,TUYA_DIR *dir){char mapped[768];struct stat info;host_path(path,mapped,sizeof(mapped));
    if(stat(mapped,&info)||!(info.st_mode&S_IFDIR))return -1;*dir=(void*)1;return 0;}
static int tkl_dir_close(TUYA_DIR dir){(void)dir;return 0;}
int tkl_system_get_random(uint32_t range){return (int)(++diagnostic_rng%range);}
int tkl_system_get_free_heap_size(void){return 123456;}
int32_t tkl_system_psram_get_free_heap_size(void){return 8000000;}
static TUYA_FILE tkl_fopen(const char *path,const char *mode){char mapped[768];host_path(path,mapped,sizeof(mapped));FILE*f=fopen(mapped,mode);if(f&&strchr(mode,'w'))sync_file=f;return f;}
static int tkl_fseek(TUYA_FILE file,int offset,int whence){return fseek(file,offset,whence);}
static int tkl_ftell(TUYA_FILE file){return (int)ftell(file);}
static int tkl_fread(void *buffer,int size,TUYA_FILE file){return (int)fread(buffer,1,(size_t)size,file);}
static int tkl_fwrite(void *buffer,int size,TUYA_FILE file){return (int)fwrite(buffer,1,(size_t)size,file);}
/* Match the SDK's no-op fflush to ensure tests require the real sync API. */
static int tkl_fflush(TUYA_FILE file){(void)file;return 0;}
static int tkl_fileno(TUYA_FILE file){sync_file=file;return 1;}
static int tkl_fsync(int fd){(void)fd;fsync_calls++;if(fsync_failures){fsync_failures--;return -1;}return sync_file?fflush(sync_file):-1;}
static int tkl_fclose(TUYA_FILE file){int result=fclose(file);close_calls++;if(file==sync_file)sync_file=NULL;if(close_failures){close_failures--;return -1;}return result;}
static int tkl_fs_remove(const char *path){char mapped[768];host_path(path,mapped,sizeof(mapped));return remove(mapped);}
static int tkl_fs_rename(const char *from,const char *to){char old_path[768],new_path[768];host_path(from,old_path,sizeof(old_path));host_path(to,new_path,sizeof(new_path));if(rename_failures){rename_failures--;return -1;}return rename(old_path,new_path);}
void *tkl_system_psram_malloc(size_t size){if(allocation_failure){allocation_failure=false;return NULL;}return malloc(size);}
void tkl_system_psram_free(void *pointer){free(pointer);}
void hh_post(const HH_Event *event){latest_event=*event;if(event_count<64)event_history[event_count]=*event;event_count++;if(!event->ok)event_failures++;}
#include "stubs/cJSON.h"
cJSON*cJSON_Parse(const char*t){(void)t;return NULL;}
cJSON*cJSON_GetObjectItemCaseSensitive(const cJSON*j,const char*k){(void)j;(void)k;return NULL;}
int cJSON_IsString(const cJSON*j){(void)j;return 0;}
int cJSON_IsTrue(const cJSON*j){(void)j;return 0;}
int cJSON_IsNumber(const cJSON*j){(void)j;return 0;}
void cJSON_Delete(cJSON*j){(void)j;}

#ifndef HH_STORAGE_SOURCE
#define HH_STORAGE_SOURCE "../../firmware/src/homehub_storage.c"
#endif
#define tkl_fs_is_exist storage_probe_exists
#include HH_STORAGE_SOURCE
#undef tkl_fs_is_exist

static int failures;
#define CHECK(condition) do{if(!(condition)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#condition);failures++;return;}}while(0)
static void run_worker(void){if(setjmp(worker_done)==0)snapshot_worker(NULL);}
static HH_Snapshot snapshot(const char *marker){HH_Snapshot s;memset(&s,0,sizeof(s));s.version=HH_SNAPSHOT_VERSION;s.seen_count=1;s.seen_next=1;snprintf(s.seen_ids[0],HH_ID_MAX+1,"%s",marker);return s;}
static void reset(void){rename_failures=fsync_failures=close_failures=fsync_calls=close_calls=0;event_count=event_failures=0;allocation_failure=false;sync_file=NULL;snapshot_sequence=0;hh_storage_init();tkl_fs_remove(HH_ROOT"/state0.dat");tkl_fs_remove(HH_ROOT"/state1.dat");tkl_fs_remove(HH_ROOT"/probe.dat");tkl_fs_remove(HH_ROOT"/probe.dat.tmp");}
static void consecutive_failed_renames_preserve_last_good_generation(void){
    HH_Snapshot first=snapshot("first"),last_good=snapshot("last_good"),bad1=snapshot("uncommitted1"),bad2=snapshot("uncommitted2"),loaded;
    reset();hh_save_snapshot_async(&first);hh_save_snapshot_async(&last_good);run_worker();
    CHECK(event_count==2&&event_failures==0);
    rename_failures=2;hh_save_snapshot_async(&bad1);hh_save_snapshot_async(&bad2);run_worker();
    CHECK(event_failures==2);CHECK(hh_load_snapshot(&loaded));CHECK(strcmp(loaded.seen_ids[0],"last_good")==0);
}
static void fsync_failure_is_not_reported_as_saved(void){
    const uint8_t bytes[]={1,2,3};reset();fsync_failures=1;
    CHECK(!hh_file_write_atomic(HH_ROOT"/probe.dat",bytes,sizeof(bytes)));CHECK(fsync_calls==1);
}
static void close_failure_is_not_reported_as_saved(void){
    const uint8_t bytes[]={1,2,3};reset();close_failures=1;
    CHECK(!hh_file_write_atomic(HH_ROOT"/probe.dat",bytes,sizeof(bytes)));CHECK(close_calls==1);
}
static void allocation_failure_returns_without_self_post(void){
    HH_Snapshot s=snapshot("allocation");reset();allocation_failure=true;
    CHECK(!hh_save_snapshot_async(&s));CHECK(event_count==0);
}
static void queue_full_returns_without_self_post(void){
    HH_Snapshot s=snapshot("queued");reset();
    for(unsigned i=0;i<8;i++)CHECK(hh_save_snapshot_async(&s));
    CHECK(!hh_save_snapshot_async(&s));CHECK(event_count==0);
    run_worker();CHECK(event_count==8&&event_failures==0);
}
static void corrupted_newer_snapshot_falls_back_to_previous_valid_slot(void){
    HH_Snapshot first=snapshot("first"),second=snapshot("second"),loaded;reset();
    hh_save_snapshot_async(&first);hh_save_snapshot_async(&second);run_worker();
    TUYA_FILE f=tkl_fopen(HH_ROOT"/state0.dat","r+b");CHECK(f!=NULL);CHECK(fseek(f,20,SEEK_SET)==0);fputc(0xab,f);fclose(f);
    CHECK(hh_load_snapshot(&loaded));CHECK(strcmp(loaded.seen_ids[0],"first")==0);
}
static void wav_layout_and_length_are_strict(void){
    uint8_t bytes[684];uint32_t pcm=0;memset(bytes,0,sizeof(bytes));hh_wav_header(bytes,640);
    CHECK(hh_wav_validate(bytes,sizeof(bytes),&pcm));CHECK(pcm==640);
    bytes[24]=0;CHECK(!hh_wav_validate(bytes,sizeof(bytes),NULL));hh_wav_header(bytes,640);
    CHECK(!hh_wav_validate(bytes,sizeof(bytes)-2,NULL));bytes[22]=2;CHECK(!hh_wav_validate(bytes,sizeof(bytes),NULL));
    hh_wav_header(bytes,640);bytes[34]=8;CHECK(!hh_wav_validate(bytes,sizeof(bytes),NULL));
}
static void tagged_snapshot_ack_keeps_success_and_failure_tickets(void){
    HH_Snapshot a=snapshot("tagged_a"),b=snapshot("tagged_b");reset();
    CHECK(hh_save_snapshot_tagged(&a,101));run_worker();
    CHECK(event_history[0].ok&&event_history[0].action.event_seq==101);
    rename_failures=1;CHECK(hh_save_snapshot_tagged(&b,202));run_worker();
    CHECK(!event_history[1].ok&&event_history[1].action.event_seq==202);
    CHECK(hh_save_snapshot_tagged(&a,303));CHECK(hh_save_snapshot_tagged(&b,404));run_worker();
    CHECK(event_count==4);
    CHECK(event_history[2].ok&&event_history[2].action.event_seq==303);
    CHECK(event_history[3].ok&&event_history[3].action.event_seq==404);
}
#define RUN(test) do{int before=failures;test();if(before==failures)printf("PASS %s\n",#test);}while(0)
static void diagnostic_directories(void){char path[768];host_path(HH_ROOT"/messages",path,sizeof(path));
#ifdef _WIN32
    _mkdir(path);host_path(HH_ROOT"/voices",path,sizeof(path));_mkdir(path);
#else
    mkdir(path,0777);host_path(HH_ROOT"/voices",path,sizeof(path));mkdir(path,0777);
#endif
}
static void startup_write_diagnostic_preserves_existing_files(void){
    uint8_t original[4]={7,8,9,10},loaded[4]={0};diagnostic_directories();diagnostic_rng=diagnostic_logs=0;
    TUYA_FILE file=tkl_fopen("/sdcard/HP000001.TMP","wb");CHECK(file!=NULL);CHECK(tkl_fwrite(original,4,file)==4);CHECK(tkl_fclose(file)==0);
    hh_storage_init();CHECK(diagnostic_logs==3);
    file=tkl_fopen("/sdcard/HP000001.TMP","rb");CHECK(file!=NULL);CHECK(tkl_fread(loaded,4,file)==4);tkl_fclose(file);
    CHECK(!memcmp(original,loaded,4));CHECK(strstr(diagnostic_log[0],"stage=ok")&&strstr(diagnostic_log[1],"stage=ok")&&strstr(diagnostic_log[2],"stage=ok"));
    unsigned before=diagnostic_logs;hh_storage_init();CHECK(diagnostic_logs==before);tkl_fs_remove("/sdcard/HP000001.TMP");
}
int main(int argc,char**argv){if(argc!=2)return 2;snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(startup_write_diagnostic_preserves_existing_files);
    RUN(consecutive_failed_renames_preserve_last_good_generation);RUN(fsync_failure_is_not_reported_as_saved);
    RUN(close_failure_is_not_reported_as_saved);RUN(allocation_failure_returns_without_self_post);RUN(queue_full_returns_without_self_post);
    RUN(corrupted_newer_snapshot_falls_back_to_previous_valid_slot);RUN(wav_layout_and_length_are_strict);
    RUN(tagged_snapshot_ack_keeps_success_and_failure_tickets);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
