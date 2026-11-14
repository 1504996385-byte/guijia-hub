#include "homehub_platform.h"
#include "tkl_memory.h"
#include "tkl_system.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool hh_storage_ready;
MUTEX_HANDLE hh_fs_mutex;
HH_DeviceConfig hh_device;
static QUEUE_HANDLE snapshot_jobs;
static uint32_t snapshot_sequence;
static bool startup_probe_done,write_failure_probe_done;
#ifndef HH_STORAGE_DIAG
#define HH_STORAGE_DIAG PR_NOTICE
#endif
/* This T5 platform hook is implemented by tkl_memory.c. */
extern int32_t tkl_system_psram_get_free_heap_size(void);

/* Caller holds hh_fs_mutex. Only files verified absent with the pinned FatFs
 * missing-name codes are created; an existing/unknown target is never opened. */
static void storage_write_probe(const char *label,const char *directory)
{
    char path[HH_PATH_MAX+1]={0};const char *stage="directory";
    TUYA_DIR dir=NULL;TUYA_FILE file=NULL;bool created=false;
    int dir_rc=tkl_dir_open(directory,&dir),stat_rc=-99,write_rc=-99,sync_rc=-99,close_rc=-99,remove_rc=-99;
    if(dir_rc!=0)goto done;
    dir_rc=tkl_dir_close(dir);if(dir_rc!=0){stage="directory-close";goto done;}
    stage="collision";
    for(unsigned attempt=0;attempt<8;attempt++){
        BOOL_T exists=0;
        int length=snprintf(path,sizeof(path),"%s/HP%06x.TMP",directory,(unsigned)tkl_system_get_random(0x1000000u)&0xffffffu);
        if(length<0||length>=(int)sizeof(path)){stage="path";goto done;}
        stat_rc=tkl_fs_is_exist(path,&exists);
        if(stat_rc==OPRT_OK&&exists)continue;
        if(stat_rc!=4&&stat_rc!=5){stage="stat";goto done;}
        stage="open";file=tkl_fopen(path,"wb");if(!file)goto done;
        created=true;stage="write";
        uint8_t marker[4]={'H','H','P','R'};write_rc=tkl_fwrite(marker,sizeof(marker),file);
        if(write_rc!=4)goto done;
        stage="sync";sync_rc=tkl_fsync(tkl_fileno(file));if(sync_rc!=OPRT_OK)goto done;
        stage="close";close_rc=tkl_fclose(file);file=NULL;if(close_rc!=0)goto done;
        stage="ok";break;
    }
done:
    if(file)close_rc=tkl_fclose(file);
    if(created){remove_rc=tkl_fs_remove(path);if(remove_rc!=0&&strcmp(stage,"ok")==0)stage="remove";}
    HH_STORAGE_DIAG("HH fs-probe area=%s dir=%d stat=%d write=%d sync=%d close=%d remove=%d stage=%s native=%d psram=%d",
        label,dir_rc,stat_rc,write_rc,sync_rc,close_rc,remove_rc,stage,
        tkl_system_get_free_heap_size(),(int)tkl_system_psram_get_free_heap_size());
}
typedef struct { uint32_t magic, sequence, bytes, checksum; HH_Snapshot snapshot; } Envelope;
typedef struct { Envelope envelope; uint32_t ticket; } SnapshotJob;

uint32_t hh_checksum(const void *data, size_t len)
{
    const uint8_t *p = data; uint32_t h = 2166136261u;
    while (len--) h = (h ^ *p++) * 16777619u;
    return h;
}
bool hh_valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (!n || n > HH_ID_MAX) return false;
    for (size_t i=0; i<n; i++) if (!((id[i]>='a'&&id[i]<='z') ||
        (id[i]>='A'&&id[i]<='Z') || (id[i]>='0'&&id[i]<='9') || id[i]=='_' || id[i]=='-')) return false;
    return true;
}
bool hh_file_read(const char *path, uint8_t **data, uint32_t *size, uint32_t limit)
{
    bool ok=false; TUYA_FILE file=NULL; uint8_t *buf=NULL; int n;
    if (!hh_storage_ready || !path || !data || !size) return false;
    tal_mutex_lock(hh_fs_mutex);
    file=tkl_fopen(path,"rb");
    if (!file) goto done;
    if (tkl_fseek(file,0,2)<0) goto done;
    n=tkl_ftell(file);
    if (n<=0 || (uint32_t)n>limit || tkl_fseek(file,0,0)<0) goto done;
    buf=tkl_system_psram_malloc((uint32_t)n+1u);
    if (!buf || tkl_fread(buf,n,file)!=n) goto done;
    buf[n]=0; *data=buf; *size=n; buf=NULL; ok=true;
done:
    if (file) tkl_fclose(file);
    if (buf) tkl_system_psram_free(buf);
    tal_mutex_unlock(hh_fs_mutex); return ok;
}
bool hh_file_write_atomic(const char *path, const uint8_t *data, uint32_t size)
{
    bool ok=false; char temporary[HH_PATH_MAX+16]; TUYA_FILE file=NULL;
    const char *stage="open";
    if (!hh_storage_ready || !path || !data || strlen(path)>HH_PATH_MAX) return false;
    snprintf(temporary,sizeof(temporary),"%s.tmp",path);
    tal_mutex_lock(hh_fs_mutex);
    file=tkl_fopen(temporary,"wb");
    if (!file) goto done;
    stage="write";if(tkl_fwrite((void *)data,size,file)!=(int)size)goto done;
    stage="sync";if(tkl_fsync(tkl_fileno(file))!=OPRT_OK)goto done;
    stage="close";
    int close_result=tkl_fclose(file);file=NULL;
    if(close_result!=0)goto done;
    /* Alternating snapshot slots preserve the previous valid generation even
       on FAT implementations where replacement rename is not supported. */
    tkl_fs_remove(path);
    stage="rename";ok=tkl_fs_rename(temporary,path)==0;
done:
    if (file) tkl_fclose(file);
    if (!ok) tkl_fs_remove(temporary);
    if(!ok){
        HH_STORAGE_DIAG("HH fs-write stage=%s bytes=%u native=%d psram=%d",stage,(unsigned)size,
            tkl_system_get_free_heap_size(),(int)tkl_system_psram_get_free_heap_size());
        if(!write_failure_probe_done){
            char parent[HH_PATH_MAX+1];snprintf(parent,sizeof(parent),"%s",path);char *slash=strrchr(parent,'/');
            write_failure_probe_done=true;
            if(slash){*slash=0;storage_write_probe("write-failure",parent);}
        }
    }
    tal_mutex_unlock(hh_fs_mutex); return ok;
}
static uint32_t u32(const uint8_t *p) { return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void p32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i)); }
void hh_wav_header(uint8_t h[44],uint32_t n)
{
    memset(h,0,44); memcpy(h,"RIFF",4);p32(h+4,n+36);memcpy(h+8,"WAVEfmt ",8);
    p32(h+16,16);h[20]=1;h[22]=1;p32(h+24,16000);p32(h+28,32000);
    h[32]=2;h[34]=16;memcpy(h+36,"data",4);p32(h+40,n);
}
bool hh_wav_validate(const uint8_t *p,uint32_t size,uint32_t *pcm)
{
    if(!p || size<44 || size>HH_WAV_MAX || memcmp(p,"RIFF",4) || memcmp(p+8,"WAVEfmt ",8) ||
       u32(p+16)!=16 || p[20]!=1 || p[21] || p[22]!=1 || p[23] || u32(p+24)!=16000 ||
       u32(p+28)!=32000 || p[32]!=2 || p[33] || p[34]!=16 || p[35] ||
       memcmp(p+36,"data",4) || u32(p+40)!=size-44 || u32(p+4)!=size-8 || (size&1u)) return false;
    if(pcm)*pcm=size-44;
    return size>44;
}
static void copy_json(cJSON *json,const char *key,char *out,size_t size)
{
    cJSON *item=cJSON_GetObjectItemCaseSensitive(json,key);
    if(cJSON_IsString(item)&&strlen(item->valuestring)<size)snprintf(out,size,"%s",item->valuestring);
}
bool hh_load_config(void)
{
    uint8_t *bytes=NULL; uint32_t size=0; memset(&hh_device,0,sizeof(hh_device));hh_device.volume=100;
    hh_device.chat_enabled=true;
    /* This assembled target has SR505 on IO47. An explicit TF setting may
       disable it; an absent file/field must retain the assembled default. */
    hh_device.pir_enabled=true;
    snprintf(hh_device.tuya_product_id,sizeof(hh_device.tuya_product_id),"3rhb25ktaqov4akw");
    if(!hh_file_read(HH_ROOT "/config.json",&bytes,&size,4096))return false;
    cJSON *j=cJSON_Parse((char*)bytes);tkl_system_psram_free(bytes);if(!j)return false;
    copy_json(j,"wifi_ssid",hh_device.wifi_ssid,sizeof(hh_device.wifi_ssid));
    copy_json(j,"wifi_password",hh_device.wifi_password,sizeof(hh_device.wifi_password));
    copy_json(j,"server_url",hh_device.server_url,sizeof(hh_device.server_url));
    copy_json(j,"device_token",hh_device.device_token,sizeof(hh_device.device_token));
    cJSON *chat=cJSON_GetObjectItemCaseSensitive(j,"chat_enabled");
    if(chat)hh_device.chat_enabled=cJSON_IsTrue(chat);
    cJSON *product=cJSON_GetObjectItemCaseSensitive(j,"tuya_product_id");
    if(product){
        if(cJSON_IsString(product)&&strlen(product->valuestring)<sizeof(hh_device.tuya_product_id)&&hh_valid_id(product->valuestring))
            snprintf(hh_device.tuya_product_id,sizeof(hh_device.tuya_product_id),"%s",product->valuestring);
        else {hh_device.tuya_product_id[0]=0;hh_device.chat_enabled=false;}
    }
    cJSON *pir=cJSON_GetObjectItemCaseSensitive(j,"pir_enabled");
    if(pir)hh_device.pir_enabled=cJSON_IsTrue(pir);
    cJSON *volume=cJSON_GetObjectItemCaseSensitive(j,"volume");
    if(cJSON_IsNumber(volume)&&volume->valueint>=0&&volume->valueint<=100)hh_device.volume=volume->valueint;
    cJSON_Delete(j);return true;
}
bool hh_load_snapshot(HH_Snapshot *snapshot)
{
    bool found=false; uint32_t newest=0;
    for(unsigned i=0;i<2;i++){
        char path[96];uint8_t *bytes=NULL;uint32_t size=0;snprintf(path,sizeof(path),HH_ROOT "/state%u.dat",i);
        if(!hh_file_read(path,&bytes,&size,sizeof(Envelope)))continue;
        Envelope *e=(Envelope*)bytes;
        if(size==sizeof(*e)&&e->magic==0x48484231u&&e->bytes==sizeof(HH_Snapshot)&&
           e->checksum==hh_checksum(&e->snapshot,sizeof(e->snapshot))&&(!found||(int32_t)(e->sequence-newest)>0)){
            *snapshot=e->snapshot;newest=e->sequence;found=true;
        }
        tkl_system_psram_free(bytes);
    }
    snapshot_sequence=newest;return found;
}
static void snapshot_worker(void *unused)
{
    (void)unused;SnapshotJob *job;
    while(1)if(OPRT_OK==tal_queue_fetch(snapshot_jobs,&job,QUEUE_WAIT_FOREVER)){
        Envelope *e=&job->envelope;
        char path[96];e->magic=0x48484231u;e->sequence=snapshot_sequence+1;e->bytes=sizeof(e->snapshot);
        e->checksum=hh_checksum(&e->snapshot,sizeof(e->snapshot));
        snprintf(path,sizeof(path),HH_ROOT "/state%u.dat",e->sequence&1u);
        HH_Event event={.type=EV_SNAPSHOT_SAVED};event.action.event_seq=job->ticket;
        event.ok=hh_file_write_atomic(path,(uint8_t*)e,sizeof(*e));
        if(event.ok)snapshot_sequence=e->sequence;
        hh_post(&event);tkl_system_psram_free(job);
    }
}
bool hh_save_snapshot_tagged(const HH_Snapshot *snapshot,uint32_t ticket)
{
    SnapshotJob *job=tkl_system_psram_malloc(sizeof(*job));
    if(!job){PR_ERR("Snapshot allocation failed");return false;}
    memset(job,0,sizeof(*job));job->envelope.snapshot=*snapshot;job->ticket=ticket;
    if(tal_queue_post(snapshot_jobs,&job,0)!=OPRT_OK){tkl_system_psram_free(job);PR_ERR("Snapshot queue full");return false;}
    return true;
}
bool hh_save_snapshot_async(const HH_Snapshot *snapshot)
{
    return hh_save_snapshot_tagged(snapshot,0);
}
bool hh_storage_init(void)
{
    tal_mutex_create_init(&hh_fs_mutex);
    hh_storage_ready=tkl_fs_mount("/sdcard",DEV_SDCARD)==OPRT_OK;
    if(hh_storage_ready){
        tkl_fs_mkdir(HH_ROOT);tkl_fs_mkdir(HH_ROOT "/messages");tkl_fs_mkdir(HH_ROOT "/outbox");
        tkl_fs_mkdir(HH_ROOT "/voices");
        if(!startup_probe_done){
            startup_probe_done=true;tal_mutex_lock(hh_fs_mutex);
            storage_write_probe("root","/sdcard");storage_write_probe("messages",HH_ROOT "/messages");
            storage_write_probe("voices",HH_ROOT "/voices");tal_mutex_unlock(hh_fs_mutex);
        }
    }
    tal_queue_create_init(&snapshot_jobs,sizeof(SnapshotJob*),8);
    THREAD_HANDLE thread=NULL;THREAD_CFG_T cfg={.stackDepth=4096,.priority=THREAD_PRIO_2,.thrdname="hh_store"};
    tal_thread_create_and_start(&thread,NULL,NULL,snapshot_worker,NULL,&cfg);
    return hh_storage_ready;
}
