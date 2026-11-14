/* Real delivered storage, WAV validator and seed C; synthetic TF files only. */
#define main storage_suite_unused_main
#include "test_storage.c"
#undef main
#include "../../firmware/src/homehub_seed.h"
static unsigned reminder_calls,departure_calls,arrival_calls;
static bool reminder_ok=true,departure_ok=true,arrival_ok=true;
bool hh_reminder_install(void){reminder_calls++;return reminder_ok;}
bool hh_departure_install(void){departure_calls++;return departure_ok;}
bool hh_arrival_install(void){arrival_calls++;return arrival_ok;}
#define TRUE true
#define FALSE false

static const char *required_names[]={"arrive.wav","leave.wav","take_key.wav"};
static unsigned card_reads,card_frees,card_writes,card_removes,card_renames;
static unsigned card_read_fail_at;
static char card_notice[128];
static uint8_t fixture_wav[684];
static void card_log(const char *format,...){va_list args;va_start(args,format);vsnprintf(card_notice,sizeof(card_notice),format,args);va_end(args);}
#define PR_NOTICE card_log
static bool card_read(const char *path,uint8_t **data,uint32_t *size,uint32_t limit){
    card_reads++;if(card_read_fail_at==card_reads){*data=NULL;*size=0;return false;}
    return hh_file_read(path,data,size,limit);
}
static void card_free(void *p){if(p)card_frees++;tkl_system_psram_free(p);}
static TUYA_FILE card_open(const char *path,const char *mode){if(strchr(mode,'w')||strchr(mode,'a')||strchr(mode,'+'))card_writes++;return tkl_fopen(path,mode);}
static int card_write(void *p,int size,TUYA_FILE file){card_writes++;return tkl_fwrite(p,size,file);}
static bool card_atomic(const char *path,const uint8_t *data,uint32_t size){card_writes++;return hh_file_write_atomic(path,data,size);}
static int card_remove(const char *path){card_removes++;return tkl_fs_remove(path);}
static int card_rename(const char *from,const char *to){card_renames++;return tkl_fs_rename(from,to);}
#define hh_file_read card_read
#define tkl_system_psram_free card_free
#define tkl_fopen card_open
#define tkl_fwrite card_write
#define hh_file_write_atomic card_atomic
#define tkl_fs_remove card_remove
#define tkl_fs_rename card_rename
#define tkl_fs_is_exist storage_probe_exists
#include "../../firmware/src/homehub_seed.c"
#undef hh_file_read
#undef tkl_system_psram_free
#undef tkl_fopen
#undef tkl_fwrite
#undef hh_file_write_atomic
#undef tkl_fs_remove
#undef tkl_fs_rename
#undef tkl_fs_is_exist

static void card_path(unsigned index,char *path,size_t bytes){snprintf(path,bytes,HH_ROOT"/voices/%s",required_names[index]);}
static void setup_card(void){
    reset();memset(fixture_wav,0x5a,sizeof(fixture_wav));hh_wav_header(fixture_wav,640);
    reminder_calls=departure_calls=arrival_calls=0;reminder_ok=departure_ok=arrival_ok=true;
    for(unsigned i=0;i<3;i++){char path[256];card_path(i,path,sizeof(path));TUYA_FILE f=tkl_fopen(path,"wb");if(f){tkl_fwrite(fixture_wav,sizeof(fixture_wav),f);tkl_fclose(f);}}
    card_reads=card_frees=card_writes=card_removes=card_renames=card_read_fail_at=0;card_notice[0]=0;
}
static bool original_file(unsigned index){
    char path[256];uint8_t *data=NULL;uint32_t bytes=0;card_path(index,path,sizeof(path));
    bool ok=hh_file_read(path,&data,&bytes,HH_WAV_MAX)&&bytes==sizeof(fixture_wav)&&!memcmp(data,fixture_wav,bytes);
    if(data)tkl_system_psram_free(data);return ok;
}
static bool no_card_mutations(void){return card_writes==0&&card_removes==0&&card_renames==0;}
static void three_valid_card_wavs_are_read_and_preserved(void){
    setup_card();CHECK(hh_seed_voices());CHECK(card_reads==3&&card_frees==3&&no_card_mutations());
    CHECK(!strcmp(card_notice,"TF test voices: 3 valid, 0 failed"));
    for(unsigned i=0;i<3;i++)CHECK(original_file(i));
}
static void repeated_startup_never_rewrites_voice_files(void){
    setup_card();CHECK(hh_seed_voices());CHECK(hh_seed_voices());CHECK(card_reads==6&&card_frees==6&&no_card_mutations());
    for(unsigned i=0;i<3;i++)CHECK(original_file(i));
}
static void failed_read_is_reported_without_replacing_files(void){
    setup_card();card_read_fail_at=2;CHECK(!hh_seed_voices());CHECK(card_reads==3&&card_frees==2&&no_card_mutations());
    CHECK(!strcmp(card_notice,"TF test voices: 2 valid, 1 failed"));for(unsigned i=0;i<3;i++)CHECK(original_file(i));
}
static void corrupt_existing_wav_is_reported_and_preserved(void){
    setup_card();char path[256];card_path(1,path,sizeof(path));TUYA_FILE f=tkl_fopen(path,"r+b");CHECK(f!=NULL);fputc('X',f);fclose(f);
    CHECK(!hh_seed_voices());CHECK(card_reads==3&&card_frees==3&&no_card_mutations());
    uint8_t *data=NULL;uint32_t bytes=0;CHECK(hh_file_read(path,&data,&bytes,HH_WAV_MAX));CHECK(bytes==sizeof(fixture_wav)&&data[0]=='X'&&!memcmp(data+1,fixture_wav+1,bytes-1));tkl_system_psram_free(data);
    CHECK(original_file(0)&&original_file(2));
}
static void missing_wav_is_not_created(void){
    setup_card();char path[256];card_path(2,path,sizeof(path));CHECK(tkl_fs_remove(path)==0);
    CHECK(!hh_seed_voices());CHECK(card_reads==3&&card_frees==2&&no_card_mutations());
    uint8_t *data=NULL;uint32_t bytes=0;CHECK(!hh_file_read(path,&data,&bytes,HH_WAV_MAX));CHECK(data==NULL);CHECK(original_file(0)&&original_file(1));
}
static void allocation_failure_is_read_only_and_recoverable(void){
    setup_card();allocation_failure=true;CHECK(!hh_seed_voices());CHECK(card_reads==3&&card_frees==2&&no_card_mutations());
    CHECK(hh_seed_voices());CHECK(card_reads==6&&card_frees==5&&no_card_mutations());
}
static void unmounted_card_is_not_accessed(void){
    setup_card();hh_storage_ready=false;CHECK(!hh_seed_voices());CHECK(card_reads==0&&card_frees==0&&no_card_mutations());
}
static void each_installer_failure_still_attempts_all_three_and_card_validation(void){
    for(unsigned i=0;i<3;i++){setup_card();if(i==0)reminder_ok=false;else if(i==1)departure_ok=false;else arrival_ok=false;CHECK(!hh_seed_voices());
    CHECK(reminder_calls==1&&departure_calls==1&&arrival_calls==1&&card_reads==3&&no_card_mutations());}
}

int main(int argc,char **argv){
    if(argc!=2)return 2;snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(three_valid_card_wavs_are_read_and_preserved);RUN(repeated_startup_never_rewrites_voice_files);
    RUN(each_installer_failure_still_attempts_all_three_and_card_validation);
    RUN(failed_read_is_reported_without_replacing_files);RUN(corrupt_existing_wav_is_reported_and_preserved);
    RUN(missing_wav_is_not_created);RUN(allocation_failure_is_read_only_and_recoverable);RUN(unmounted_card_is_not_accessed);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
