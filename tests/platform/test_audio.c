/* Reuse host filesystem/queue scaffolding, then include actual audio source. */
#include "../../firmware/src/homehub_chat.h"
#define HH_TEST_EVENT_EXTRA_TYPES ,EV_CHAT_REPORT,EV_CHAT_TEXT
#define HH_TEST_EVENT_EXTRA_FIELDS HH_ChatEvent chat_event; bool chat_is_user_text; char text[192];
#define main storage_suite_unused_main
#include "test_storage.c"
#undef main
#include "stubs/tdl_audio_manage.h"
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>
#endif
static uint32_t fake_rate=16000,fake_time,play_calls,fail_play_at,open_calls,stop_calls,stop_during_sleep_turn;
static char last_record_stop_log[256];
static void audio_log(const char *format,...){
    if(strncmp(format,"HH record-stop",14))return;
    va_list args;va_start(args,format);vsnprintf(last_record_stop_log,sizeof(last_record_stop_log),format,args);va_end(args);
}
static bool stop_failure;
static bool threshold_fifo,writer_started;
static uint32_t speaker_fifo_bytes;
static MUTEX_HANDLE test_mic_mutex;
static unsigned mic_lock_depth,blocked_mic_operations,backend_count,backend_pcm_count;
static bool backend_begin_reject,backend_pcm_reject,backend_submit_reject;
static uint8_t backend_pcm_bytes[16][640];
static uint32_t backend_pcm_sizes[16],backend_turns[32];
static char backend_order[33];
static void backend_note(char command,uint32_t turn){if(backend_count<32){backend_order[backend_count]=command;backend_turns[backend_count]=turn;}backend_count++;}
bool hh_chat_cloud_begin(uint32_t turn){backend_note('B',turn);return !backend_begin_reject;}
bool hh_chat_cloud_pcm(uint32_t turn,const uint8_t *data,uint32_t bytes){
    backend_note('P',turn);
    if(backend_pcm_reject||bytes>640||backend_pcm_count>=16)return false;
    memcpy(backend_pcm_bytes[backend_pcm_count],data,bytes);backend_pcm_sizes[backend_pcm_count++]=bytes;return true;
}
bool hh_chat_cloud_submit(uint32_t turn){backend_note('S',turn);return !backend_submit_reject;}
void hh_chat_cloud_cancel(uint32_t turn){backend_note('C',turn);}
static int audio_mutex_create(MUTEX_HANDLE *mutex){static uintptr_t next_mutex=10;*mutex=(void*)++next_mutex;if(!test_mic_mutex)test_mic_mutex=*mutex;return OPRT_OK;}
static int audio_mutex_lock(MUTEX_HANDLE mutex){if(mutex==test_mic_mutex)mic_lock_depth++;return OPRT_OK;}
static int audio_mutex_unlock(MUTEX_HANDLE mutex){if(mutex==test_mic_mutex)mic_lock_depth--;return OPRT_OK;}
static void audio_post(const HH_Event *event){if(mic_lock_depth)blocked_mic_operations++;hh_post(event);}
static HH_Action queued_audio_action;
static bool audio_action_pending;
static int audio_queue_fetch(QUEUE_HANDLE queue,void *item,unsigned wait){
    (void)queue;(void)wait;
    if(!audio_action_pending)longjmp(worker_done,1);
    memcpy(item,&queued_audio_action,sizeof(queued_audio_action));audio_action_pending=false;return OPRT_OK;
}
int tdl_audio_find(const char*n,TDL_AUDIO_HANDLE_T*h){(void)n;*h=(void*)1;return 0;}
int tdl_audio_open(TDL_AUDIO_HANDLE_T h,TDL_AUDIO_MIC_CB cb){(void)h;(void)cb;open_calls++;return 0;}
int tdl_audio_get_info(TDL_AUDIO_HANDLE_T h,TDL_AUDIO_INFO_T*i){(void)h;i->sample_rate=fake_rate;i->sample_bits=16;i->sample_ch_num=1;return 0;}
int tdl_audio_volume_set(TDL_AUDIO_HANDLE_T h,uint8_t v){(void)h;(void)v;return 0;}
int tdl_audio_play(TDL_AUDIO_HANDLE_T h,uint8_t*d,uint32_t n){(void)h;(void)d;if(mic_lock_depth)blocked_mic_operations++;play_calls++;
    if(fail_play_at&&play_calls==fail_play_at)return -1;
    if(threshold_fifo){speaker_fifo_bytes+=n;if(speaker_fifo_bytes>=1280)writer_started=true;}return 0;}
int tdl_audio_play_stop(TDL_AUDIO_HANDLE_T h){(void)h;stop_calls++;return stop_failure?-1:0;}
static uint32_t tal_system_get_millisecond(void){return fake_time;}
static void tal_system_sleep(uint32_t ms){if(mic_lock_depth)blocked_mic_operations++;fake_time+=ms;
    if(threshold_fifo&&writer_started){uint32_t consumed=ms*32u;if(consumed>speaker_fifo_bytes)consumed=speaker_fifo_bytes;speaker_fifo_bytes-=consumed;}
    if(stop_during_sleep_turn){uint32_t turn=stop_during_sleep_turn;stop_during_sleep_turn=0;hh_audio_chat_sink_stop(turn);}}
#define tal_queue_fetch audio_queue_fetch
#define tal_mutex_create_init audio_mutex_create
#define tal_mutex_lock audio_mutex_lock
#define tal_mutex_unlock audio_mutex_unlock
#define hh_post audio_post
#define PR_NOTICE audio_log
#include "../../firmware/src/homehub_audio.c"
#undef PR_NOTICE
#undef hh_post
#undef tal_mutex_unlock
#undef tal_mutex_lock
#undef tal_mutex_create_init
#undef tal_queue_fetch

static HH_Action action(const char *id){HH_Action a;memset(&a,0,sizeof(a));a.type=HH_ACT_RECORD_START;snprintf(a.id,sizeof(a.id),"%s",id);return a;}
static void reset_audio(void){reset();fake_rate=16000;fake_time=play_calls=fail_play_at=open_calls=stop_calls=stop_during_sleep_turn=0;stop_failure=false;test_mic_mutex=NULL;mic_lock_depth=blocked_mic_operations=backend_count=backend_pcm_count=0;
    threshold_fifo=writer_started=false;speaker_fifo_bytes=0;
    last_record_stop_log[0]=0;
    backend_begin_reject=backend_pcm_reject=backend_submit_reject=false;memset(backend_order,0,sizeof(backend_order));memset(backend_turns,0,sizeof(backend_turns));
    audio=NULL;audio_ready=false;capture=NULL;captured=0;recording=false;chat_owner_turn=chat_captured_bytes=chat_failure_turn=0;
    chat_sink_deadline_ms=0;chat_sink_has_output=false;
    chat_capture_open=chat_sink_open=chat_submitted=chat_failed=chat_stopping=local_playing=false;hh_audio_init();}
static void recording_limit_keeps_first_30_seconds(void){
    uint8_t frame[640],*wav=NULL;uint32_t size=0,pcm=0;HH_Action a=action("maximum");reset_audio();memset(frame,0x11,sizeof(frame));start_record(&a);
    CHECK(latest_event.type==EV_RECORD_STARTED&&latest_event.ok);
    for(unsigned i=0;i<1503;i++)microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    stop_record(&a,false);CHECK(latest_event.type==EV_RECORD_SAVED&&latest_event.ok);
    CHECK(hh_file_read(latest_event.action.audio_path,&wav,&size,HH_WAV_MAX));CHECK(hh_wav_validate(wav,size,&pcm));
    CHECK(pcm==960000u);CHECK(wav[44]==0x11&&wav[size-1]==0x11);tkl_system_psram_free(wav);
}
static void borrowed_pcm_is_copied_before_callback_returns(void){
    uint8_t frame[640],*wav=NULL;uint32_t size=0;HH_Action a=action("copied");reset_audio();start_record(&a);
    for(unsigned i=0;i<13;i++){memset(frame,0x23,sizeof(frame));microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));memset(frame,0,sizeof(frame));}
    stop_record(&a,false);CHECK(latest_event.ok);CHECK(hh_file_read(latest_event.action.audio_path,&wav,&size,HH_WAV_MAX));
    CHECK(wav[44]==0x23&&wav[size-1]==0x23);tkl_system_psram_free(wav);
}
static void cancellation_never_saves_and_late_pcm_is_ignored(void){
    uint8_t frame[640]={0};unsigned before;HH_Action a=action("cancelled");reset_audio();start_record(&a);
    for(unsigned i=0;i<13;i++)microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    before=event_count;stop_record(&a,true);CHECK(event_count==before);
    CHECK(!recording&&capture==NULL);microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(captured==8320);
}
static void unsupported_audio_format_disables_capture(void){
    HH_Action a=action("wrong_rate");reset_audio();fake_rate=8000;audio_ready=false;
    CHECK(!hh_audio_init());start_record(&a);CHECK(latest_event.type==EV_RECORD_STARTED&&!latest_event.ok);CHECK(!recording&&capture==NULL);
}
static void audio_save_sync_failure_returns_failure(void){
    uint8_t frame[640]={0};HH_Action a=action("save_failed");reset_audio();start_record(&a);
    for(unsigned i=0;i<13;i++)microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    fsync_failures=1;stop_record(&a,false);CHECK(latest_event.type==EV_RECORD_SAVED&&!latest_event.ok);CHECK(!recording&&capture==NULL);
}
static void speaker_write_error_cannot_succeed(void){
    uint8_t wav[684]={0};reset_audio();hh_wav_header(wav,640);
    CHECK(hh_file_write_atomic(HH_ROOT"/play.wav",wav,sizeof(wav)));fail_play_at=1;CHECK(!play(HH_ROOT"/play.wav"));
}
static void prepare_voice_files(void){
    char directory[768];reset_audio();host_path(HH_ROOT"/voices",directory,sizeof(directory));
#ifdef _WIN32
    _mkdir(directory);
#else
    mkdir(directory,0777);
#endif
    tkl_fs_remove(HH_ROOT"/voices/take_key.wav");tkl_fs_remove(HH_ROOT"/voices/arrive.wav");
    tkl_fs_remove(HH_ROOT"/voices/leave.wav");tkl_fs_remove(HH_ROOT"/voices/leave_03.wav");
}
static bool write_voice(const char *path){uint8_t wav[684]={0};hh_wav_header(wav,640);return hh_file_write_atomic(path,wav,sizeof(wav));}
static void run_audio_action(HH_ActionType type,const char *path,HH_Trigger trigger){
    memset(&queued_audio_action,0,sizeof(queued_audio_action));queued_audio_action.type=type;
    queued_audio_action.trigger=trigger;queued_audio_action.event_seq=73;
    snprintf(queued_audio_action.audio_path,sizeof(queued_audio_action.audio_path),"%s",path);
    audio_action_pending=true;if(setjmp(worker_done)==0)hh_audio_worker(NULL);
}
static void key_reminder_resolves_fixed_asset_and_preserves_action_type(void){
    prepare_voice_files();CHECK(write_voice(HH_ROOT"/voices/take_key.wav"));
    run_audio_action(HH_ACT_KEY_REMINDER_REQUEST,HH_ROOT"/voices/take_key.wav",HH_LEAVE);
    CHECK(event_count==1);CHECK(latest_event.type==EV_GREETING_READY&&latest_event.ok);
    CHECK(latest_event.action.type==HH_ACT_KEY_REMINDER_REQUEST&&latest_event.action.event_seq==73);
    CHECK(strcmp(latest_event.action.audio_path,HH_ROOT"/voices/take_key.wav")==0);CHECK(play_calls==0);
}
static void missing_key_reminder_never_falls_back_to_available_greetings(void){
    prepare_voice_files();CHECK(write_voice(HH_ROOT"/voices/leave.wav"));CHECK(write_voice(HH_ROOT"/voices/arrive.wav"));
    run_audio_action(HH_ACT_KEY_REMINDER_REQUEST,HH_ROOT"/voices/take_key.wav",HH_LEAVE);
    CHECK(event_count==1);CHECK(latest_event.type==EV_GREETING_READY&&!latest_event.ok);
    CHECK(strcmp(latest_event.action.audio_path,HH_ROOT"/voices/take_key.wav")==0);CHECK(play_calls==0);
}
static void invalid_key_reminder_never_falls_back_to_available_greetings(void){
    const uint8_t invalid[]={1,2,3};prepare_voice_files();CHECK(write_voice(HH_ROOT"/voices/leave.wav"));
    CHECK(hh_file_write_atomic(HH_ROOT"/voices/take_key.wav",invalid,sizeof(invalid)));
    run_audio_action(HH_ACT_KEY_REMINDER_REQUEST,HH_ROOT"/voices/take_key.wav",HH_LEAVE);
    CHECK(event_count==1);CHECK(latest_event.type==EV_GREETING_READY&&!latest_event.ok);
    CHECK(strcmp(latest_event.action.audio_path,HH_ROOT"/voices/take_key.wav")==0);
}
static void ordinary_greeting_keeps_alias_fallback(void){
    prepare_voice_files();CHECK(write_voice(HH_ROOT"/voices/leave.wav"));
    run_audio_action(HH_ACT_GREETING_REQUEST,HH_ROOT"/voices/leave_03.wav",HH_LEAVE);
    CHECK(event_count==1);CHECK(latest_event.type==EV_GREETING_READY&&latest_event.ok);
    CHECK(strcmp(latest_event.action.audio_path,HH_ROOT"/voices/leave.wav")==0);
    CHECK(latest_event.action.type==HH_ACT_GREETING_REQUEST);
}
static void ordinary_greeting_keeps_existing_selected_asset(void){
    prepare_voice_files();CHECK(write_voice(HH_ROOT"/voices/leave_03.wav"));CHECK(write_voice(HH_ROOT"/voices/leave.wav"));
    run_audio_action(HH_ACT_GREETING_REQUEST,HH_ROOT"/voices/leave_03.wav",HH_LEAVE);
    CHECK(event_count==1);CHECK(latest_event.type==EV_GREETING_READY&&latest_event.ok);
    CHECK(strcmp(latest_event.action.audio_path,HH_ROOT"/voices/leave_03.wav")==0);
}
static void run_chat_action(HH_ActionType type,uint32_t turn){
    memset(&queued_audio_action,0,sizeof(queued_audio_action));queued_audio_action.type=type;queued_audio_action.chat_turn=turn;
    audio_action_pending=true;if(setjmp(worker_done)==0)hh_audio_worker(NULL);
}
static void chat_start_uses_single_open_and_reports_accepted_turn(void){
    reset_audio();run_chat_action(HH_ACT_CHAT_START,101);
    CHECK(backend_count==1&&backend_order[0]=='B'&&backend_turns[0]==101);
    CHECK(event_count==1&&latest_event.type==EV_CHAT_REPORT&&latest_event.chat_event==HH_CHAT_STARTED&&latest_event.ok);
    CHECK(latest_event.action.chat_turn==101&&open_calls==1&&blocked_mic_operations==0);
}
static void rejected_chat_begin_never_captures_pcm(void){
    uint8_t frame[640]={0};reset_audio();backend_begin_reject=true;run_chat_action(HH_ACT_CHAT_START,102);
    CHECK(event_count==1&&latest_event.type==EV_CHAT_REPORT&&latest_event.chat_event==HH_CHAT_FAILED&&!latest_event.ok);
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(backend_pcm_count==0&&open_calls==1);
}
static void chat_pcm_is_copied_in_bounded_chunks_before_submit(void){
    uint8_t frame[1280];reset_audio();run_chat_action(HH_ACT_CHAT_START,103);CHECK(backend_order[0]=='B');
    memset(frame,0x37,sizeof(frame));microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));memset(frame,0,sizeof(frame));
    CHECK(backend_pcm_count==2&&backend_pcm_sizes[0]==640&&backend_pcm_sizes[1]==640);
    CHECK(backend_pcm_bytes[0][0]==0x37&&backend_pcm_bytes[1][639]==0x37);
    run_chat_action(HH_ACT_CHAT_SUBMIT,103);CHECK(strcmp(backend_order,"BPPS")==0);
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(backend_pcm_count==2&&blocked_mic_operations==0);
}
static void chat_stop_closes_capture_but_does_not_release_local_audio(void){
    uint8_t frame[640]={0};HH_Action local=action("blocked_chat");reset_audio();run_chat_action(HH_ACT_CHAT_START,104);
    run_chat_action(HH_ACT_CHAT_STOP,104);run_chat_action(HH_ACT_CHAT_STOP,104);
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(backend_pcm_count==0);
    CHECK(backend_count>=2&&backend_order[backend_count-1]=='C');start_record(&local);CHECK(!latest_event.ok&&capture==NULL);
    CHECK(open_calls==1&&blocked_mic_operations==0);
}
static void full_chat_pcm_queue_cancels_without_blocking_microphone(void){
    uint8_t frame[640]={0};reset_audio();run_chat_action(HH_ACT_CHAT_START,105);CHECK(backend_order[0]=='B');
    backend_pcm_reject=true;microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    CHECK(backend_order[backend_count-1]=='C');unsigned before=backend_count;
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(backend_count==before&&blocked_mic_operations==0);
    run_chat_action(HH_ACT_UI,0);CHECK(latest_event.type==EV_CHAT_REPORT&&latest_event.chat_event==HH_CHAT_FAILED);
}
static void quiet_without_chat_owner_never_stops_local_capture(void){
    HH_Action local=action("local_stays");reset_audio();start_record(&local);CHECK(recording&&capture);
    CHECK(hh_audio_chat_quiesce(900,false));CHECK(recording&&capture&&stop_calls==0);stop_record(&local,true);
}
static void sink_requires_submitted_current_owner_and_paces_output(void){
    uint8_t pcm[1280]={0};reset_audio();run_chat_action(HH_ACT_CHAT_START,201);
    CHECK(!hh_audio_chat_sink_begin(201));run_chat_action(HH_ACT_CHAT_SUBMIT,201);
    CHECK(!hh_audio_chat_sink_begin(999));CHECK(hh_audio_chat_sink_begin(201));
    CHECK(!hh_audio_chat_sink_write(999,pcm,sizeof(pcm))&&play_calls==0);
    CHECK(hh_audio_chat_sink_write(201,pcm,sizeof(pcm)));CHECK(play_calls==2&&fake_time>=40&&blocked_mic_operations==0);
}
static void natural_quiesce_keeps_tail_then_releases_local_audio(void){
    uint8_t pcm[640]={0};HH_Action local=action("after_chat");reset_audio();run_chat_action(HH_ACT_CHAT_START,202);
    run_chat_action(HH_ACT_CHAT_SUBMIT,202);CHECK(hh_audio_chat_sink_begin(202));CHECK(hh_audio_chat_sink_write(202,pcm,sizeof(pcm)));
    uint32_t before=fake_time;hh_audio_chat_sink_stop(202);CHECK(stop_calls==0);
    CHECK(!hh_audio_chat_sink_write(202,pcm,sizeof(pcm)));CHECK(hh_audio_chat_quiesce(202,true));
    CHECK(fake_time-before>=1260&&play_calls==4&&stop_calls==1&&blocked_mic_operations==0);
    CHECK(latest_event.chat_event!=HH_CHAT_QUIESCED);start_record(&local);CHECK(latest_event.ok&&recording);stop_record(&local,true);
}
static void stale_quiesce_and_stop_cannot_release_new_chat_owner(void){
    uint8_t frame[640]={0};HH_Action local=action("still_owned");reset_audio();run_chat_action(HH_ACT_CHAT_START,203);
    CHECK(!hh_audio_chat_quiesce(202,false));hh_audio_chat_sink_stop(202);run_chat_action(HH_ACT_CHAT_STOP,202);
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));CHECK(backend_pcm_count==1&&backend_turns[backend_count-1]==203);
    start_record(&local);CHECK(!latest_event.ok&&!recording);CHECK(hh_audio_chat_quiesce(203,false));
}
static void sink_write_failure_reports_failed_then_can_quiesce(void){
    uint8_t pcm[640]={0};reset_audio();run_chat_action(HH_ACT_CHAT_START,204);run_chat_action(HH_ACT_CHAT_SUBMIT,204);
    CHECK(hh_audio_chat_sink_begin(204));fail_play_at=1;CHECK(!hh_audio_chat_sink_write(204,pcm,sizeof(pcm)));
    CHECK(hh_audio_chat_quiesce(204,false));CHECK(latest_event.type==EV_CHAT_REPORT&&latest_event.chat_event==HH_CHAT_FAILED&&!latest_event.ok);
    CHECK(latest_event.action.chat_turn==204&&stop_calls==1&&blocked_mic_operations==0);
}
static void stop_failure_retains_owner_until_successful_retry(void){
    HH_Action local=action("stop_failed");reset_audio();run_chat_action(HH_ACT_CHAT_START,205);run_chat_action(HH_ACT_CHAT_STOP,205);
    stop_failure=true;CHECK(!hh_audio_chat_quiesce(205,false));start_record(&local);CHECK(!latest_event.ok&&capture==NULL);
    stop_failure=false;CHECK(hh_audio_chat_quiesce(205,false));start_record(&local);CHECK(latest_event.ok);stop_record(&local,true);
}
static void sink_stop_during_write_prevents_later_chunks(void){
    uint8_t pcm[1280]={0};reset_audio();run_chat_action(HH_ACT_CHAT_START,206);run_chat_action(HH_ACT_CHAT_SUBMIT,206);
    CHECK(hh_audio_chat_sink_begin(206));stop_during_sleep_turn=206;
    CHECK(!hh_audio_chat_sink_write(206,pcm,sizeof(pcm)));CHECK(play_calls==1&&blocked_mic_operations==0);
    CHECK(hh_audio_chat_quiesce(206,false));CHECK(!hh_audio_chat_sink_write(206,pcm,sizeof(pcm)));
}
static void unsupported_chat_format_and_submit_rejection_cancel_safely(void){
    uint8_t frame[640]={0};reset_audio();run_chat_action(HH_ACT_CHAT_START,207);unsigned events_before=event_count;
    microphone((TDL_AUDIO_FRAME_FORMAT_E)99,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    CHECK(backend_order[backend_count-1]=='C'&&event_count==events_before&&blocked_mic_operations==0);
    run_chat_action(HH_ACT_UI,0);CHECK(latest_event.chat_event==HH_CHAT_FAILED);
    CHECK(hh_audio_chat_quiesce(207,false));run_chat_action(HH_ACT_CHAT_START,208);backend_submit_reject=true;
    run_chat_action(HH_ACT_CHAT_SUBMIT,208);CHECK(latest_event.chat_event==HH_CHAT_FAILED&&!latest_event.ok);
    CHECK(latest_event.action.chat_turn==208&&backend_order[backend_count-1]=='C'&&blocked_mic_operations==0);
}
static void local_capture_blocks_chat_and_chat_blocks_local_play(void){
    HH_Action local=action("local_priority");uint8_t wav[684]={0};reset_audio();start_record(&local);CHECK(recording);
    run_chat_action(HH_ACT_CHAT_START,209);CHECK(latest_event.chat_event==HH_CHAT_FAILED&&backend_count==0&&recording);
    stop_record(&local,true);hh_wav_header(wav,640);CHECK(hh_file_write_atomic(HH_ROOT"/play.wav",wav,sizeof(wav)));
    run_chat_action(HH_ACT_CHAT_START,210);CHECK(!play(HH_ROOT"/play.wav")&&play_calls==0);
}
static void cancelled_subthreshold_pcm_is_flushed_before_owner_handoff(void){
    uint8_t pcm[640]={0};reset_audio();threshold_fifo=true;run_chat_action(HH_ACT_CHAT_START,301);run_chat_action(HH_ACT_CHAT_SUBMIT,301);
    CHECK(hh_audio_chat_sink_begin(301));CHECK(hh_audio_chat_sink_write(301,pcm,sizeof(pcm)));CHECK(speaker_fifo_bytes==640&&!writer_started);
    hh_audio_chat_sink_stop(301);CHECK(hh_audio_chat_quiesce(301,false));CHECK(speaker_fifo_bytes==0&&blocked_mic_operations==0);
}
static void reopening_same_sink_retains_prior_output_until_quiet(void){
    uint8_t pcm[640]={0};reset_audio();threshold_fifo=true;run_chat_action(HH_ACT_CHAT_START,302);run_chat_action(HH_ACT_CHAT_SUBMIT,302);
    CHECK(hh_audio_chat_sink_begin(302));CHECK(hh_audio_chat_sink_write(302,pcm,sizeof(pcm)));hh_audio_chat_sink_stop(302);
    CHECK(hh_audio_chat_sink_begin(302));hh_audio_chat_sink_stop(302);uint32_t before=fake_time;
    CHECK(hh_audio_chat_quiesce(302,false));CHECK(speaker_fifo_bytes==0&&fake_time-before>=1200);
}
static void local_midplay_error_flushes_accepted_pcm_before_chat(void){
    uint8_t wav[1324]={0};reset_audio();threshold_fifo=true;hh_wav_header(wav,1280);
    CHECK(hh_file_write_atomic(HH_ROOT"/local_midfail.wav",wav,sizeof(wav)));fail_play_at=2;
    CHECK(!play(HH_ROOT"/local_midfail.wav"));CHECK(speaker_fifo_bytes==0);
    run_chat_action(HH_ACT_CHAT_START,303);CHECK(latest_event.chat_event==HH_CHAT_STARTED&&latest_event.ok);
}
static void failed_cancel_padding_cannot_release_owner(void){
    uint8_t pcm[640]={0};HH_Action local=action("unsafe_handoff");reset_audio();threshold_fifo=true;
    run_chat_action(HH_ACT_CHAT_START,304);run_chat_action(HH_ACT_CHAT_SUBMIT,304);CHECK(hh_audio_chat_sink_begin(304));
    CHECK(hh_audio_chat_sink_write(304,pcm,sizeof(pcm)));fail_play_at=play_calls+1;
    CHECK(!hh_audio_chat_quiesce(304,false));start_record(&local);CHECK(!latest_event.ok&&capture==NULL);
    CHECK(hh_audio_chat_quiesce(304,false)&&speaker_fifo_bytes==0);
}
static void save_failure_with_pcm_is_distinguished_from_no_pcm(void){
    char messages[768],unavailable[768];uint8_t frame[640]={0};HH_Action a=action("open_failed");reset_audio();start_record(&a);
    for(unsigned i=0;i<13;i++)microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,frame,sizeof(frame));
    host_path(HH_ROOT"/messages",messages,sizeof(messages));host_path(HH_ROOT"/messages-unavailable",unavailable,sizeof(unavailable));
    CHECK(rename(messages,unavailable)==0);stop_record(&a,false);int restored=rename(unavailable,messages);
    CHECK(restored==0&&latest_event.type==EV_RECORD_SAVED&&!latest_event.ok);
    CHECK(strstr(last_record_stop_log,"pcm_bytes=8320")&&strstr(last_record_stop_log,"frames=13"));
    CHECK(strstr(last_record_stop_log,"wav=1 write=0")&&strstr(last_record_stop_log,"reason=storage"));
    a=action("no_pcm");start_record(&a);stop_record(&a,false);
    CHECK(!latest_event.ok&&strstr(last_record_stop_log,"pcm_bytes=0")&&strstr(last_record_stop_log,"reason=short_pcm"));
}
static void valid_idle_pcm_updates_voice_progress_without_recording(void){
    uint8_t pcm[640]={0};uint32_t seq=99,last=99;reset_audio();
    CHECK(!hh_audio_voice_progress(&seq,&last));CHECK(seq==0&&last==0);
    fake_time=20;microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,pcm,sizeof(pcm));
    CHECK(hh_audio_voice_progress(&seq,&last));CHECK(seq==1&&last==20);
    fake_time=50;CHECK(hh_audio_voice_progress(&seq,&last));CHECK(seq==1&&last==20);
    microphone((TDL_AUDIO_FRAME_FORMAT_E)99,TDL_AUDIO_TEST_STATUS,pcm,sizeof(pcm));
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,NULL,sizeof(pcm));
    microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,pcm,1);
    CHECK(hh_audio_voice_progress(&seq,&last));CHECK(seq==1&&last==20);
    fake_time=60;microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,pcm,sizeof(pcm));
    CHECK(hh_audio_voice_progress(&seq,&last));CHECK(seq==2&&last==60);
    CHECK(!hh_audio_voice_progress(NULL,&last));CHECK(!hh_audio_voice_progress(&seq,NULL));
    CHECK(open_calls==1&&backend_count==0&&event_count==0);
}
static void local_capture_reservation_blocks_voice_but_keeps_frame_progress(void){
    uint8_t pcm[640]={0};uint32_t seq=0,last=0;HH_Action a=action("voice_gate");reset_audio();
    start_record(&a);fake_time=40;microphone(TDL_AUDIO_FRAME_FORMAT_PCM,TDL_AUDIO_TEST_STATUS,pcm,sizeof(pcm));
    CHECK(!hh_audio_voice_progress(&seq,&last));CHECK(seq==1&&last==40);
    stop_record(&a,true);CHECK(hh_audio_voice_progress(&seq,&last));CHECK(seq==1&&last==40);
    CHECK(open_calls==1&&backend_count==0);
}
int main(int argc,char**argv){if(argc!=2)return 2;snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(recording_limit_keeps_first_30_seconds);RUN(borrowed_pcm_is_copied_before_callback_returns);
    RUN(cancellation_never_saves_and_late_pcm_is_ignored);RUN(unsupported_audio_format_disables_capture);
    RUN(audio_save_sync_failure_returns_failure);RUN(speaker_write_error_cannot_succeed);
    RUN(key_reminder_resolves_fixed_asset_and_preserves_action_type);
    RUN(missing_key_reminder_never_falls_back_to_available_greetings);
    RUN(invalid_key_reminder_never_falls_back_to_available_greetings);
    RUN(ordinary_greeting_keeps_alias_fallback);RUN(ordinary_greeting_keeps_existing_selected_asset);
    RUN(chat_start_uses_single_open_and_reports_accepted_turn);RUN(rejected_chat_begin_never_captures_pcm);
    RUN(chat_pcm_is_copied_in_bounded_chunks_before_submit);RUN(chat_stop_closes_capture_but_does_not_release_local_audio);
    RUN(full_chat_pcm_queue_cancels_without_blocking_microphone);
    RUN(quiet_without_chat_owner_never_stops_local_capture);RUN(sink_requires_submitted_current_owner_and_paces_output);
    RUN(natural_quiesce_keeps_tail_then_releases_local_audio);RUN(stale_quiesce_and_stop_cannot_release_new_chat_owner);
    RUN(sink_write_failure_reports_failed_then_can_quiesce);RUN(stop_failure_retains_owner_until_successful_retry);
    RUN(sink_stop_during_write_prevents_later_chunks);RUN(unsupported_chat_format_and_submit_rejection_cancel_safely);
    RUN(local_capture_blocks_chat_and_chat_blocks_local_play);
    RUN(cancelled_subthreshold_pcm_is_flushed_before_owner_handoff);RUN(reopening_same_sink_retains_prior_output_until_quiet);
    RUN(local_midplay_error_flushes_accepted_pcm_before_chat);RUN(failed_cancel_padding_cannot_release_owner);
    RUN(save_failure_with_pcm_is_distinguished_from_no_pcm);
    RUN(valid_idle_pcm_updates_voice_progress_without_recording);
    RUN(local_capture_reservation_blocks_voice_but_keeps_frame_progress);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
