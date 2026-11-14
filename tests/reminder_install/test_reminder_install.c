#include "homehub_platform.h"
#define HOMEHUB_REMINDER_ASSET_H
#define HOMEHUB_DEPARTURE_ASSET_H
#define HOMEHUB_ARRIVAL_ASSET_H
#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "actual_wav_helpers.h"

#define PRIMARY HH_ROOT "/voices/take_key.wav"
#define DEPARTURE HH_ROOT "/voices/leave.wav"
#define ARRIVAL HH_ROOT "/voices/arrive.wav"
typedef struct {char path[256];uint8_t data[512];uint32_t size;bool exists;} File;
static File files[8];
static unsigned failures,reads,stats,writes,primary_writes,backup_writes,lock_depth,live_allocs;
static unsigned departure_writes,arrival_writes;
static const char *read_fail,*stat_fail;
static int stat_error;
static bool stat_exists,backup_write_fail,backup_verify_fail,primary_write_fail,primary_verify_fail;
static bool backup_verified_before_primary;
static bool departure_backup_verified;
static bool arrival_backup_verified,arrival_write_fail;
static uint8_t old_wav[48];
MUTEX_HANDLE hh_fs_mutex=(void*)1;
bool hh_storage_ready;
#define CHECK(v) do{if(!(v)){printf("FAIL %s:%d %s\n",__func__,__LINE__,#v);failures++;return;}}while(0)
#define RUN(f) do{unsigned before=failures;f();printf("%s %s\n",before==failures?"PASS":"FAIL",#f);}while(0)
static File *file_at(const char *path,bool create){for(unsigned i=0;i<8;i++)if(files[i].path[0]&&!strcmp(files[i].path,path))return &files[i];if(create)for(unsigned i=0;i<8;i++)if(!files[i].path[0]){snprintf(files[i].path,sizeof(files[i].path),"%s",path);return &files[i];}return NULL;}
static void put(const char *path,const uint8_t *data,uint32_t size){File*f=file_at(path,true);if(!f||size>sizeof(f->data)){failures++;return;}memcpy(f->data,data,size);f->size=size;f->exists=true;}
static bool equals(const char *path,const uint8_t *data,uint32_t size){File*f=file_at(path,false);return f&&f->exists&&f->size==size&&!memcmp(f->data,data,size);}
static File *backup(void){for(unsigned i=0;i<8;i++)if(strstr(files[i].path,".bak.wav"))return &files[i];return NULL;}
int tal_mutex_lock(MUTEX_HANDLE m){(void)m;if(lock_depth)failures++;lock_depth++;return 0;}
int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;if(lock_depth!=1)failures++;lock_depth--;return 0;}
int32_t tkl_fs_is_exist(const char *path,BOOL_T *exists){stats++;if(lock_depth!=1)failures++;if(stat_fail&&!strcmp(stat_fail,path)){*exists=stat_exists;return stat_error;}File*f=file_at(path,false);*exists=f&&f->exists;return *exists?0:4;}
void tkl_system_psram_free(void *p){if(p){if(!live_allocs)failures++;else live_allocs--;free(p);}}
bool hh_file_read(const char *path,uint8_t **data,uint32_t *size,uint32_t limit){
    reads++;if(lock_depth)failures++;File*f=file_at(path,false);
    if((read_fail&&!strcmp(read_fail,path))||!f||!f->exists||f->size>limit)return false;
    if((backup_verify_fail&&strstr(path,".bak.wav"))||(primary_verify_fail&&primary_writes&&!strcmp(path,PRIMARY)))return false;
    *data=malloc(f->size);memcpy(*data,f->data,f->size);*size=f->size;live_allocs++;
    if(strstr(path,".bak.wav")&&f->size==sizeof(old_wav)&&!memcmp(f->data,old_wav,sizeof(old_wav))){
        if(strstr(path,"/leave."))departure_backup_verified=true;
        else if(strstr(path,"/arrive."))arrival_backup_verified=true;
        else backup_verified_before_primary=true;
    }
    return true;
}
bool hh_file_write_atomic(const char *path,const uint8_t *data,uint32_t size){
    if(lock_depth)failures++;writes++;
    if(!strcmp(path,PRIMARY)){primary_writes++;File*existing=file_at(path,false);if(existing&&existing->exists&&!backup_verified_before_primary)failures++;if(primary_write_fail){if(existing)existing->exists=false;return false;}}
    else if(!strcmp(path,DEPARTURE)){departure_writes++;File*existing=file_at(path,false);if(existing&&existing->exists&&!departure_backup_verified)failures++;}
    else if(!strcmp(path,ARRIVAL)){arrival_writes++;File*existing=file_at(path,false);if(existing&&existing->exists&&!arrival_backup_verified)failures++;if(arrival_write_fail){if(existing)existing->exists=false;return false;}}
    else if(strstr(path,".bak.wav")){backup_writes++;if(backup_write_fail)return false;}
    else {failures++;return false;}
    put(path,data,size);return true;
}
#include "../../firmware/src/homehub_reminder_install.c"
#include "../../firmware/src/homehub_seed.c"
static void reset(void){if(live_allocs||lock_depth)failures++;memset(files,0,sizeof(files));reads=stats=writes=primary_writes=backup_writes=lock_depth=departure_writes=arrival_writes=0;read_fail=stat_fail=NULL;stat_error=-1;stat_exists=false;backup_write_fail=backup_verify_fail=primary_write_fail=primary_verify_fail=false;backup_verified_before_primary=departure_backup_verified=arrival_backup_verified=arrival_write_fail=false;hh_storage_ready=true;hh_reminder_wav_size=sizeof(hh_reminder_wav);hh_reminder_wav_checksum=FIXTURE_CHECKSUM;hh_departure_wav_size=sizeof(hh_departure_wav);hh_departure_wav_checksum=FIXTURE_DEPARTURE_CHECKSUM;hh_arrival_wav_size=sizeof(hh_arrival_wav);hh_arrival_wav_checksum=FIXTURE_ARRIVAL_CHECKSUM;memcpy(old_wav,hh_reminder_wav,sizeof(old_wav));old_wav[44]^=0x42;}
static void initial_missing_file_installs_only_new_reminder(void){reset();CHECK(hh_reminder_install());CHECK(equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size));CHECK(primary_writes==1&&backup_writes==0&&live_allocs==0);}
static void old_valid_wav_is_backed_up_and_verified_before_replacement(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));CHECK(hh_reminder_install());File*b=backup();CHECK(b&&b->exists&&b->size==sizeof(old_wav)&&!memcmp(b->data,old_wav,sizeof(old_wav)));CHECK(!strcmp(b->path,FIXTURE_OLD_BACKUP));CHECK(backup_verified_before_primary&&backup_writes==1&&primary_writes==1);CHECK(equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&live_allocs==0);}
static void identical_current_file_is_never_rewritten(void){reset();put(PRIMARY,hh_reminder_wav,hh_reminder_wav_size);CHECK(hh_reminder_install());CHECK(hh_reminder_install());CHECK(!writes&&!backup()&&!live_allocs);}
static void primary_read_and_unknown_stat_failures_preserve_original(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));read_fail=PRIMARY;CHECK(!hh_reminder_install()&&!writes&&equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);const int codes[]={-1,1,0,4};for(unsigned i=0;i<4;i++){reset();put(PRIMARY,old_wav,sizeof(old_wav));stat_fail=PRIMARY;stat_error=codes[i];stat_exists=i==3;CHECK(!hh_reminder_install()&&!writes&&equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);}}
static void backup_write_or_verification_failure_never_changes_primary(void){for(unsigned i=0;i<2;i++){reset();put(PRIMARY,old_wav,sizeof(old_wav));backup_write_fail=i==0;backup_verify_fail=i==1;CHECK(!hh_reminder_install());CHECK(!primary_writes&&equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);backup_write_fail=backup_verify_fail=false;CHECK(hh_reminder_install()&&equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size));}}
static void conflicting_backup_is_not_overwritten_or_trusted(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));backup_verify_fail=true;CHECK(!hh_reminder_install());File*b=backup();CHECK(b&&b->exists);b->data[44]^=0x11;backup_verify_fail=false;unsigned before=writes;CHECK(!hh_reminder_install());CHECK(writes==before&&!primary_writes&&equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);}
static void existing_backup_stat_or_read_failure_preserves_both_files(void){for(unsigned i=0;i<2;i++){reset();put(PRIMARY,old_wav,sizeof(old_wav));backup_verify_fail=true;CHECK(!hh_reminder_install());File*b=backup();CHECK(b&&b->exists);backup_verify_fail=false;if(i==0){stat_fail=b->path;stat_error=1;}else read_fail=b->path;unsigned before=writes;CHECK(!hh_reminder_install()&&writes==before&&!primary_writes);CHECK(equals(PRIMARY,old_wav,sizeof(old_wav))&&equals(b->path,old_wav,sizeof(old_wav))&&!live_allocs);}}
static void failed_primary_replace_can_recover_on_next_startup(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));primary_write_fail=true;CHECK(!hh_reminder_install());File*b=backup();CHECK(b&&b->exists&&!memcmp(b->data,old_wav,sizeof(old_wav)));CHECK(!file_at(PRIMARY,false)->exists&&!live_allocs);primary_write_fail=false;CHECK(hh_reminder_install());CHECK(equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&backup_writes==1&&!live_allocs);}
static void primary_verification_failure_is_not_reported_as_installed(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));primary_verify_fail=true;CHECK(!hh_reminder_install());CHECK(primary_writes==1&&backup_writes==1&&!live_allocs);primary_verify_fail=false;CHECK(hh_reminder_install());CHECK(primary_writes==1&&backup_writes==1&&equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&!live_allocs);}
static void corrupt_existing_or_asset_checksum_error_is_read_only(void){reset();old_wav[0]='X';put(PRIMARY,old_wav,sizeof(old_wav));CHECK(!hh_reminder_install()&&!writes&&equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);reset();put(PRIMARY,old_wav,sizeof(old_wav));hh_reminder_wav_checksum^=1;CHECK(!hh_reminder_install()&&!writes&&!stats&&!reads&&equals(PRIMARY,old_wav,sizeof(old_wav)));}
static void malformed_embedded_wav_is_rejected_before_storage_access(void){reset();hh_reminder_wav_size-=2;CHECK(!hh_reminder_install()&&!writes&&!stats&&!reads&&!live_allocs);}
static void missing_parent_fatfs_code_is_accepted_as_absence(void){reset();stat_fail=PRIMARY;stat_error=5;stat_exists=false;CHECK(hh_reminder_install());CHECK(equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&!backup_writes&&!live_allocs);}
static void seed_installs_all_three_and_preserves_message_files(void){reset();put(ARRIVAL,old_wav,sizeof(old_wav));put(DEPARTURE,old_wav,sizeof(old_wav));put(HH_ROOT"/messages/local_saved.wav",old_wav,sizeof(old_wav));put(HH_ROOT"/messages/px_42.wav",old_wav,sizeof(old_wav));CHECK(hh_seed_voices());CHECK(primary_writes==1&&departure_writes==1&&arrival_writes==1&&backup_writes==2);CHECK(equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size)&&equals(DEPARTURE,hh_departure_wav,hh_departure_wav_size));CHECK(equals(HH_ROOT"/messages/local_saved.wav",old_wav,sizeof(old_wav))&&equals(HH_ROOT"/messages/px_42.wav",old_wav,sizeof(old_wav))&&!live_allocs);}

static void missing_departure_installs_independently(void){reset();CHECK(hh_departure_install());CHECK(equals(DEPARTURE,hh_departure_wav,hh_departure_wav_size)&&departure_writes==1&&!primary_writes&&!backup_writes&&!live_allocs);}
static void old_departure_gets_its_own_verified_backup(void){reset();put(DEPARTURE,old_wav,sizeof(old_wav));CHECK(hh_departure_install());CHECK(equals(FIXTURE_OLD_DEPARTURE_BACKUP,old_wav,sizeof(old_wav)));CHECK(equals(DEPARTURE,hh_departure_wav,hh_departure_wav_size)&&departure_backup_verified&&backup_writes==1&&departure_writes==1&&!primary_writes&&!live_allocs);}
static void current_departure_is_never_rewritten(void){reset();put(DEPARTURE,hh_departure_wav,hh_departure_wav_size);CHECK(hh_departure_install()&&hh_departure_install());CHECK(!writes&&!live_allocs);}
static void unreadable_departure_is_preserved(void){reset();put(DEPARTURE,old_wav,sizeof(old_wav));read_fail=DEPARTURE;CHECK(!hh_departure_install());CHECK(!writes&&equals(DEPARTURE,old_wav,sizeof(old_wav))&&!live_allocs);}
static void reminder_failure_does_not_skip_other_installers(void){reset();put(PRIMARY,old_wav,sizeof(old_wav));read_fail=PRIMARY;CHECK(!hh_seed_voices());CHECK(departure_writes==1&&arrival_writes==1&&equals(DEPARTURE,hh_departure_wav,hh_departure_wav_size)&&equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size));CHECK(equals(PRIMARY,old_wav,sizeof(old_wav))&&!live_allocs);}
static void missing_arrival_installs_independently(void){reset();CHECK(hh_arrival_install());CHECK(arrival_writes==1&&!primary_writes&&!departure_writes&&!backup_writes&&equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size)&&!live_allocs);}
static void arrival_migration_preserves_verified_old_backup_and_is_idempotent(void){reset();put(ARRIVAL,old_wav,sizeof(old_wav));CHECK(hh_arrival_install());CHECK(arrival_backup_verified&&arrival_writes==1&&backup_writes==1&&equals(FIXTURE_OLD_ARRIVAL_BACKUP,old_wav,sizeof(old_wav)));CHECK(equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size));CHECK(hh_arrival_install()&&arrival_writes==1&&backup_writes==1&&!live_allocs);}
static void unreadable_or_unknown_arrival_is_preserved(void){for(unsigned i=0;i<2;i++){reset();put(ARRIVAL,old_wav,sizeof(old_wav));if(i==0)read_fail=ARRIVAL;else{stat_fail=ARRIVAL;stat_error=1;}CHECK(!hh_arrival_install()&&!writes&&equals(ARRIVAL,old_wav,sizeof(old_wav))&&!live_allocs);}}
static void failed_arrival_replace_can_recover_without_losing_backup(void){reset();put(ARRIVAL,old_wav,sizeof(old_wav));arrival_write_fail=true;CHECK(!hh_arrival_install());CHECK(equals(FIXTURE_OLD_ARRIVAL_BACKUP,old_wav,sizeof(old_wav))&&!file_at(ARRIVAL,false)->exists&&!live_allocs);arrival_write_fail=false;CHECK(hh_arrival_install()&&equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size)&&backup_writes==1&&!live_allocs);}
static void departure_failure_does_not_skip_reminder_or_arrival(void){reset();put(DEPARTURE,old_wav,sizeof(old_wav));read_fail=DEPARTURE;CHECK(!hh_seed_voices());CHECK(primary_writes==1&&arrival_writes==1&&equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&equals(ARRIVAL,hh_arrival_wav,hh_arrival_wav_size));CHECK(equals(DEPARTURE,old_wav,sizeof(old_wav))&&!live_allocs);}
static void arrival_failure_does_not_skip_reminder_or_departure(void){reset();put(ARRIVAL,old_wav,sizeof(old_wav));read_fail=ARRIVAL;CHECK(!hh_seed_voices());CHECK(primary_writes==1&&departure_writes==1&&equals(PRIMARY,hh_reminder_wav,hh_reminder_wav_size)&&equals(DEPARTURE,hh_departure_wav,hh_departure_wav_size));CHECK(equals(ARRIVAL,old_wav,sizeof(old_wav))&&!live_allocs);}

static void unmounted_card_is_not_touched(void){reset();hh_storage_ready=false;CHECK(!hh_reminder_install()&&!hh_seed_voices());CHECK(!reads&&!stats&&!writes&&!live_allocs);}
int main(void){RUN(missing_arrival_installs_independently);RUN(arrival_migration_preserves_verified_old_backup_and_is_idempotent);RUN(unreadable_or_unknown_arrival_is_preserved);RUN(failed_arrival_replace_can_recover_without_losing_backup);RUN(departure_failure_does_not_skip_reminder_or_arrival);RUN(arrival_failure_does_not_skip_reminder_or_departure);RUN(missing_departure_installs_independently);RUN(old_departure_gets_its_own_verified_backup);RUN(current_departure_is_never_rewritten);RUN(unreadable_departure_is_preserved);RUN(reminder_failure_does_not_skip_other_installers);RUN(initial_missing_file_installs_only_new_reminder);RUN(old_valid_wav_is_backed_up_and_verified_before_replacement);RUN(identical_current_file_is_never_rewritten);RUN(primary_read_and_unknown_stat_failures_preserve_original);RUN(backup_write_or_verification_failure_never_changes_primary);RUN(conflicting_backup_is_not_overwritten_or_trusted);RUN(existing_backup_stat_or_read_failure_preserves_both_files);RUN(failed_primary_replace_can_recover_on_next_startup);RUN(primary_verification_failure_is_not_reported_as_installed);RUN(corrupt_existing_or_asset_checksum_error_is_read_only);RUN(malformed_embedded_wav_is_rejected_before_storage_access);RUN(missing_parent_fatfs_code_is_accepted_as_absence);RUN(seed_installs_all_three_and_preserves_message_files);RUN(unmounted_card_is_not_touched);reset();printf("%u failure(s)\n",failures);return failures?1:0;}
