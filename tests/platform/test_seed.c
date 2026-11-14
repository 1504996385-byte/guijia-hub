/* Include delivered storage/WAV helpers and actual seeder. Host TKL models
 * the pinned T5 FatFS adapter: absent file/path returns FRESULT 4/5 unchanged. */
#define main storage_suite_unused_main
#include "test_storage.c"
#undef main
#include <errno.h>
#include "../../firmware/src/homehub_seed.h"
static unsigned reminder_calls,departure_calls,arrival_calls;
static bool reminder_ok=true,departure_ok=true,arrival_ok=true;
bool hh_reminder_install(void){reminder_calls++;return reminder_ok;}
bool hh_departure_install(void){departure_calls++;return departure_ok;}
bool hh_arrival_install(void){arrival_calls++;return arrival_ok;}

typedef bool BOOL_T;
#define TRUE true
#define FALSE false
#define PR_NOTICE(...) ((void)0)

static unsigned queries,write_opens,renames,remove_targets;
static unsigned query_error_at;
static int query_error_code;
static bool fail_open,fail_write,fail_rename,create_on_second_query;
static const unsigned char human_bytes[]={0xff,0x00,0x21,0x7f,'h','u','m','a','n'};

static int seed_stat(const char *path,struct stat *info) {
    char mapped[768];host_path(path,mapped,sizeof(mapped));queries++;
    if(query_error_at==queries){errno=EIO;return query_error_code;}
    if(create_on_second_query&&queries==2){FILE*f=fopen(mapped,"wb");if(f){fwrite(human_bytes,1,sizeof(human_bytes),f);fclose(f);}}
    return stat(mapped,info);
}
static int tkl_fs_is_exist(const char *path,BOOL_T *exists) {
    struct stat info;int result=seed_stat(path,&info);
    if(query_error_at==queries){*exists=FALSE;return query_error_code;}
    if(result<0&&errno==ENOENT)result=4; /* FatFS FR_NO_FILE */
    *exists=result==0;return result;
}
static TUYA_FILE seed_open(const char *path,const char *mode) {
    if(strchr(mode,'w')){write_opens++;if(fail_open){fail_open=false;errno=EIO;return NULL;}}
    return tkl_fopen(path,mode);
}
static int seed_write(void *bytes,int count,TUYA_FILE file) {
    if(fail_write){fail_write=false;return tkl_fwrite(bytes,count/2,file);}
    return tkl_fwrite(bytes,count,file);
}
static int seed_remove(const char *path) {
    size_t n=strlen(path);if(n<9||strcmp(path+n-9,".seed.tmp")!=0)remove_targets++;
    return tkl_fs_remove(path);
}
static int seed_rename(const char *from,const char *to) {
    renames++;if(fail_rename){fail_rename=false;errno=EIO;return -1;}
    return tkl_fs_rename(from,to);
}

#define stat(path,info) seed_stat((path),(info))
#define tkl_fopen seed_open
#define tkl_fwrite seed_write
#define tkl_fs_remove seed_remove
#define tkl_fs_rename seed_rename
#ifndef HH_SEED_SOURCE
#define HH_SEED_SOURCE "../../firmware/src/homehub_seed.c"
#endif
#include HH_SEED_SOURCE
#undef stat
#undef tkl_fopen
#undef tkl_fwrite
#undef tkl_fs_remove
#undef tkl_fs_rename

static void asset_path(unsigned i,char *out,size_t bytes,bool temporary) {
    snprintf(out,bytes,HH_ROOT"/voices/%s%s",hh_seed_assets[i].name,temporary?".seed.tmp":"");
}
static bool exists_host(const char *path) {char mapped[768];struct stat info;host_path(path,mapped,sizeof(mapped));return stat(mapped,&info)==0;}
static void reset_seed(void) {
    reset();queries=write_opens=renames=remove_targets=query_error_at=0;
    reminder_calls=departure_calls=arrival_calls=0;reminder_ok=departure_ok=arrival_ok=true;
    query_error_code=-1;
    fail_open=fail_write=fail_rename=create_on_second_query=false;
    for(unsigned i=0;i<hh_seed_asset_count;i++){char path[256];asset_path(i,path,sizeof(path),false);tkl_fs_remove(path);asset_path(i,path,sizeof(path),true);tkl_fs_remove(path);}
}
static bool no_temporaries(void) {
    for(unsigned i=0;i<hh_seed_asset_count;i++){char path[256];asset_path(i,path,sizeof(path),true);if(exists_host(path))return false;}
    return true;
}
static void tf_unmounted_does_not_query_or_write(void) {
    reset_seed();hh_storage_ready=false;CHECK(!hh_seed_voices());CHECK(queries==0&&write_opens==0&&renames==0&&remove_targets==0);
}
static void existing_arbitrary_human_and_empty_files_are_preserved(void) {
    reset_seed();CHECK(hh_seed_asset_count==3);
    for(unsigned i=0;i<3;i++){char path[256];asset_path(i,path,sizeof(path),false);TUYA_FILE f=tkl_fopen(path,"wb");CHECK(f!=NULL);if(i<2)CHECK(tkl_fwrite((void*)human_bytes,sizeof(human_bytes),f)==sizeof(human_bytes));CHECK(tkl_fclose(f)==0);}
    CHECK(hh_seed_voices());CHECK(write_opens==0&&renames==0&&remove_targets==0);
    for(unsigned i=0;i<3;i++){char path[256];unsigned char data[32]={0};asset_path(i,path,sizeof(path),false);TUYA_FILE f=tkl_fopen(path,"rb");CHECK(f!=NULL);int n=tkl_fread(data,sizeof(data),f);tkl_fclose(f);CHECK(n==(i<2?(int)sizeof(human_bytes):0));if(i<2)CHECK(memcmp(data,human_bytes,sizeof(human_bytes))==0);}
}
static void initial_io_error_is_not_treated_as_absence(void) {
    char first[256];reset_seed();query_error_at=1;CHECK(!hh_seed_voices());asset_path(0,first,sizeof(first),false);
    CHECK(!exists_host(first));CHECK(write_opens==hh_seed_asset_count-1);CHECK(remove_targets==0&&no_temporaries());
}
static void unknown_and_disk_error_codes_do_not_write_destinations(void) {
    const int errors[]={-1,1,3,0};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++){
        char first[256];reset_seed();query_error_at=1;query_error_code=errors[i];
        CHECK(!hh_seed_voices());asset_path(0,first,sizeof(first),false);CHECK(!exists_host(first));
        CHECK(write_opens==hh_seed_asset_count-1&&remove_targets==0&&no_temporaries());
    }
}
static void verified_no_path_code_is_accepted_as_absence(void) {
    reset_seed();query_error_at=1;query_error_code=5;CHECK(hh_seed_voices());CHECK(write_opens==3&&renames==3);
}
static void second_existence_io_error_cleans_temporary(void) {
    char first[256];reset_seed();query_error_at=2;CHECK(!hh_seed_voices());asset_path(0,first,sizeof(first),false);
    CHECK(!exists_host(first));CHECK(remove_targets==0&&no_temporaries());
}
static void late_existing_file_is_preserved_before_rename(void) {
    char first[256];unsigned char data[32]={0};reset_seed();create_on_second_query=true;
    CHECK(hh_seed_voices());asset_path(0,first,sizeof(first),false);TUYA_FILE f=tkl_fopen(first,"rb");CHECK(f!=NULL);
    CHECK(tkl_fread(data,sizeof(data),f)==sizeof(human_bytes));tkl_fclose(f);CHECK(memcmp(data,human_bytes,sizeof(human_bytes))==0);
    CHECK(renames==hh_seed_asset_count-1&&remove_targets==0&&no_temporaries());
}
static void failed_open_is_explicit_and_leaves_no_temporary(void) {
    reset_seed();fail_open=true;CHECK(!hh_seed_voices());CHECK(remove_targets==0&&no_temporaries());
}
static void partial_write_is_explicit_and_leaves_no_temporary(void) {
    reset_seed();fail_write=true;CHECK(!hh_seed_voices());CHECK(remove_targets==0&&no_temporaries());
}
static void fsync_failure_is_explicit_and_leaves_no_temporary(void) {
    reset_seed();fsync_failures=1;CHECK(!hh_seed_voices());CHECK(remove_targets==0&&no_temporaries());
}
static void close_failure_is_explicit_and_leaves_no_temporary(void) {
    reset_seed();close_failures=1;CHECK(!hh_seed_voices());CHECK(remove_targets==0&&no_temporaries());
}
static void rename_failure_is_explicit_and_leaves_no_temporary(void) {
    reset_seed();fail_rename=true;CHECK(!hh_seed_voices());CHECK(remove_targets==0&&no_temporaries());
}
static void absent_files_are_seeded_with_exact_asset_bytes(void) {
    reset_seed();CHECK(hh_seed_asset_count==3);CHECK(hh_seed_voices());
    CHECK(write_opens==3&&renames==3&&remove_targets==0&&no_temporaries());
    for(unsigned i=0;i<3;i++){char path[256];uint8_t *bytes=NULL;uint32_t count=0;asset_path(i,path,sizeof(path),false);CHECK(hh_file_read(path,&bytes,&count,HH_WAV_MAX));CHECK(count==hh_seed_assets[i].bytes);CHECK(memcmp(bytes,hh_seed_assets[i].data,count)==0);CHECK(hh_wav_validate(bytes,count,NULL));tkl_system_psram_free(bytes);}
}
static void each_installer_failure_still_attempts_all_three_and_legacy_seed(void){
    for(unsigned i=0;i<3;i++){reset_seed();if(i==0)reminder_ok=false;else if(i==1)departure_ok=false;else arrival_ok=false;CHECK(!hh_seed_voices());
    CHECK(reminder_calls==1&&departure_calls==1&&arrival_calls==1&&write_opens==3);}
}

int main(int argc,char**argv) {
    if(argc!=2)return 2;snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(tf_unmounted_does_not_query_or_write);RUN(existing_arbitrary_human_and_empty_files_are_preserved);
    RUN(each_installer_failure_still_attempts_all_three_and_legacy_seed);
    RUN(initial_io_error_is_not_treated_as_absence);RUN(second_existence_io_error_cleans_temporary);
    RUN(unknown_and_disk_error_codes_do_not_write_destinations);RUN(verified_no_path_code_is_accepted_as_absence);
    RUN(late_existing_file_is_preserved_before_rename);RUN(failed_open_is_explicit_and_leaves_no_temporary);
    RUN(partial_write_is_explicit_and_leaves_no_temporary);RUN(fsync_failure_is_explicit_and_leaves_no_temporary);
    RUN(close_failure_is_explicit_and_leaves_no_temporary);RUN(rename_failure_is_explicit_and_leaves_no_temporary);
    /* Leave successful assets on disk for independent Python source/hash checks. */
    RUN(absent_files_are_seeded_with_exact_asset_bytes);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
