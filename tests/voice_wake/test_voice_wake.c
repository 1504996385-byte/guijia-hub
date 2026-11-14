#include "homehub_voice.h"
#include "tal_api.h"
#include "tkl_kws.h"
#include "tkl_vad.h"
#include <stdio.h>
#include <string.h>

static unsigned failures,clock_ms,mutex_depth,vendor_calls,kws_init_calls,reg_calls;
static unsigned enable_calls,disable_calls,vad_init_calls,vad_start_calls,vad_stop_calls,vad_reads;
static int mutex_rc,kws_rc,reg_rc,vad_rc,start_rc,enable_rc;
static bool kws_enabled;
static TKL_KWS_WAKEUP_CB wake_cb;
static TKL_VAD_CONFIG_T vad_config;
static TKL_VAD_STATUS_T vad_status;
static HH_Core core;
static bool keys[2],buttons[2],key_held;
static uint32_t mic_sequence,mic_ms;
static unsigned actions[HH_ACT_CHAT_STOP+1];
#define CHECK(value) do{if(!(value)){printf("FAIL %s:%d %s\n",__func__,__LINE__,#value);failures++;return;}}while(0)
#define RUN(test) do{unsigned before=failures;test();printf("%s %s\n",before==failures?"PASS":"FAIL",#test);}while(0)
void voice_fake_notice(const char *format,...){(void)format;}
int tal_mutex_create_init(MUTEX_HANDLE *m){*m=(void*)1;return mutex_rc;}
int tal_mutex_lock(MUTEX_HANDLE m){(void)m;mutex_depth++;return 0;}
int tal_mutex_unlock(MUTEX_HANDLE m){(void)m;mutex_depth--;return 0;}
uint64_t tal_system_get_millisecond(void){return clock_ms;}
void tkl_ai_disable_vendor_vad(void){vendor_calls++;}
int tkl_kws_init(void){kws_init_calls++;return kws_rc;}
int tkl_kws_reg_wakeup_cb(TKL_KWS_WAKEUP_CB cb){reg_calls++;wake_cb=cb;return reg_rc;}
int tkl_kws_enable(void){enable_calls++;kws_enabled=enable_rc==0;return enable_rc;}
int tkl_kws_disable(void){disable_calls++;kws_enabled=false;return 0;}
int tkl_vad_init(TKL_VAD_CONFIG_T *cfg){vad_init_calls++;vad_config=*cfg;return vad_rc;}
int tkl_vad_start(void){vad_start_calls++;return start_rc;}
int tkl_vad_stop(void){vad_stop_calls++;return 0;}
TKL_VAD_STATUS_T tkl_vad_get_status(void){vad_reads++;return vad_status;}
#include "../../firmware/src/homehub_voice.c"

static void action(void *context,const HH_Action *a){(void)context;if(a->type<=HH_ACT_CHAT_STOP)actions[a->type]++;}
static void reset(void){
    memset(&voice,0,sizeof(voice));memset(actions,0,sizeof(actions));
    clock_ms=mutex_depth=vendor_calls=kws_init_calls=reg_calls=0;
    enable_calls=disable_calls=vad_init_calls=vad_start_calls=vad_stop_calls=vad_reads=0;
    mutex_rc=kws_rc=reg_rc=vad_rc=start_rc=enable_rc=0;kws_enabled=false;wake_cb=NULL;vad_status=TKL_VAD_STATUS_NONE;
    keys[0]=keys[1]=true;buttons[0]=buttons[1]=key_held=false;mic_sequence=mic_ms=0;
    HH_Config cfg=hh_default_config();hh_init(&core,&cfg,action,NULL,0);hh_chat_set_availability(&core,HH_CHAT_READY);
}
static void tick(uint32_t now){clock_ms=now;hh_sample_with_chat(&core,now,keys,buttons,false,key_held);hh_voice_tick(&core,now,true,mic_sequence,mic_ms);}
static void frame(uint32_t now,TKL_VAD_STATUS_T status){mic_sequence++;mic_ms=now;vad_status=status;tick(now);}
static void detect(uint32_t now,TKL_KWS_WAKEUP_WORD_E word){clock_ms=now;if(wake_cb)wake_cb(word);}
static void arm(void){tick(0);tick(1500);}
static void accept_quiet(uint32_t now){detect(now,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(now);frame(now+20,TKL_VAD_STATUS_NONE);}

static void setup_once_uses_exact_t5_vad_without_duplicate_initialization(void){
    reset();CHECK(hh_voice_init());CHECK(hh_voice_init());
    CHECK(vendor_calls==1&&kws_init_calls==1&&reg_calls==1&&vad_init_calls==1&&vad_start_calls==1&&vad_stop_calls==0);
    CHECK(vad_config.sample_rate==16000&&vad_config.channel_num==1&&vad_config.speech_min_ms==200&&vad_config.noise_min_ms==1000&&vad_config.frame_duration_ms==10&&vad_config.scale==1.0f);
    CHECK(!enable_calls&&wake_cb&&mutex_depth==0);
}
static void accepted_word_latches_only_and_waits_for_fresh_quiet(void){
    reset();CHECK(hh_voice_init());arm();CHECK(kws_enabled&&enable_calls==1);
    detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);CHECK(actions[HH_ACT_CHAT_START]==0&&disable_calls==0);
    tick(1501);CHECK(!kws_enabled&&core.audio_state==HH_AUDIO_IDLE);tick(1510);CHECK(core.audio_state==HH_AUDIO_IDLE);
    frame(1521,TKL_VAD_STATUS_SPEECH);CHECK(core.audio_state==HH_AUDIO_IDLE);
    frame(1600,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE&&core.chat_from_voice&&actions[HH_ACT_CHAT_START]==1);
    CHECK(vad_start_calls==1&&vad_stop_calls==0);
}
static void second_builtin_word_works_but_unknown_word_never_latches(void){
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_SMARTLIFE);tick(1501);frame(1521,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_IDLE);
    detect(1522,TKL_KWS_WAKEUP_XIAOZHI_TONGXUE);tick(1522);frame(1542,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
}
static void delayed_callback_and_disabled_epoch_cannot_start_old_wake(void){
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(2502);frame(2522,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_IDLE);
    core.audio_state=HH_AUDIO_PLAYING;tick(2600);detect(2601,TKL_KWS_WAKEUP_NIHAO_TUYA);
    core.audio_state=HH_AUDIO_IDLE;tick(2700);tick(4199);CHECK(!kws_enabled);tick(4200);frame(4220,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_IDLE&&actions[HH_ACT_CHAT_START]==0);
}
static void pending_wake_loses_to_raw_key_or_recording_button(void){
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(1501);
    buttons[0]=true;frame(1521,TKL_VAD_STATUS_NONE);CHECK(!actions[HH_ACT_CHAT_START]);tick(1581);CHECK(core.audio_state==HH_AUDIO_RECORDING);
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(1501);key_held=true;frame(1521,TKL_VAD_STATUS_NONE);tick(1581);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE&&!core.chat_from_voice&&actions[HH_ACT_CHAT_START]==1);
}
static void offline_or_local_work_cancels_pending_and_rearms_after_tail(void){
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(1501);
    hh_chat_set_availability(&core,HH_CHAT_OFFLINE);frame(1521,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_IDLE);
    hh_chat_set_availability(&core,HH_CHAT_READY);tick(1600);tick(3100);CHECK(kws_enabled);detect(3101,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(3101);
    core.audio_state=HH_AUDIO_SAVING;frame(3121,TKL_VAD_STATUS_NONE);core.audio_state=HH_AUDIO_IDLE;tick(3200);tick(4699);CHECK(!kws_enabled);tick(4700);CHECK(kws_enabled&&actions[HH_ACT_CHAT_START]==0);
}
static void wakeword_speech_must_end_and_pending_quiet_expires_at_three_seconds(void){
    reset();CHECK(hh_voice_init());arm();detect(1501,TKL_KWS_WAKEUP_NIHAO_TUYA);tick(1501);
    for(unsigned now=1521;now<4501;now+=20)frame(now,TKL_VAD_STATUS_SPEECH);
    frame(4501,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_IDLE&&!actions[HH_ACT_CHAT_START]&&!kws_enabled);
}
static void frozen_pcm_never_turns_old_vad_none_into_submission(void){
    reset();CHECK(hh_voice_init());arm();accept_quiet(1501);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    hh_chat_report(&core,core.chat_turn,HH_CHAT_STARTED,true,1522);frame(1800,TKL_VAD_STATUS_SPEECH);CHECK(core.chat_voice_speech);
    unsigned reads=vad_reads;vad_status=TKL_VAD_STATUS_NONE;tick(3000);CHECK(vad_reads==reads&&core.audio_state==HH_AUDIO_CHAT_CAPTURE&&!actions[HH_ACT_CHAT_SUBMIT]);
    mic_sequence++;mic_ms=2000;tick(3100);CHECK(vad_reads==reads&&core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    frame(3200,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_CHAT_WAIT&&actions[HH_ACT_CHAT_SUBMIT]==1);
}
static void wakeword_alone_times_out_without_submitting_and_does_not_rearm_before_quiescence(void){
    reset();CHECK(hh_voice_init());arm();accept_quiet(1501);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    uint32_t turn=core.chat_turn;hh_chat_report(&core,turn,HH_CHAT_STARTED,true,1522);
    frame(9521,TKL_VAD_STATUS_NONE);CHECK(core.audio_state==HH_AUDIO_CHAT_STOPPING&&!actions[HH_ACT_CHAT_SUBMIT]);tick(20000);CHECK(!kws_enabled);
    hh_chat_report(&core,turn,HH_CHAT_QUIESCED,true,20001);tick(20001);tick(21500);CHECK(!kws_enabled);tick(21501);CHECK(kws_enabled);
}
static void microphone_clock_and_sequence_wrap_keep_new_frame_detection(void){
    reset();CHECK(hh_voice_init());arm();mic_sequence=UINT32_MAX-1u;accept_quiet(1501);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    hh_chat_report(&core,core.chat_turn,HH_CHAT_STARTED,true,1522);mic_sequence=0;mic_ms=1800;vad_status=TKL_VAD_STATUS_SPEECH;tick(1800);CHECK(core.chat_voice_speech);
    mic_sequence=1;mic_ms=2000;vad_status=TKL_VAD_STATUS_NONE;tick(2000);CHECK(actions[HH_ACT_CHAT_SUBMIT]==1);
    reset();CHECK(hh_voice_init());tick(UINT32_MAX-2000u);tick(UINT32_MAX-500u);CHECK(kws_enabled);
    accept_quiet(UINT32_MAX-300u);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    hh_chat_report(&core,core.chat_turn,HH_CHAT_STARTED,true,UINT32_MAX-279u);frame(100,TKL_VAD_STATUS_SPEECH);CHECK(core.chat_voice_speech);
    frame(200,TKL_VAD_STATUS_NONE);CHECK(actions[HH_ACT_CHAT_SUBMIT]==1);
}
static void frame_newer_than_tick_is_not_consumed_before_clock_catches_up(void){
    reset();CHECK(hh_voice_init());arm();accept_quiet(1501);
    CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    mic_sequence++;mic_ms=1801;vad_status=TKL_VAD_STATUS_SPEECH;
    tick(1800);CHECK(!core.chat_voice_speech);
    tick(1802);CHECK(core.chat_voice_speech);
}
static void unavailable_model_degrades_without_retrying_or_breaking_key_chat(void){
    reset();kws_rc=-1;CHECK(!hh_voice_init());CHECK(!hh_voice_init());CHECK(kws_init_calls==1&&vad_init_calls==0);
    key_held=true;tick(0);tick(60);CHECK(core.audio_state==HH_AUDIO_IDLE);key_held=false;tick(100);tick(160);key_held=true;tick(200);tick(260);CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE&&!core.chat_from_voice);
}
int main(void){
    RUN(setup_once_uses_exact_t5_vad_without_duplicate_initialization);RUN(accepted_word_latches_only_and_waits_for_fresh_quiet);
    RUN(second_builtin_word_works_but_unknown_word_never_latches);RUN(delayed_callback_and_disabled_epoch_cannot_start_old_wake);
    RUN(pending_wake_loses_to_raw_key_or_recording_button);RUN(offline_or_local_work_cancels_pending_and_rearms_after_tail);
    RUN(wakeword_speech_must_end_and_pending_quiet_expires_at_three_seconds);RUN(frozen_pcm_never_turns_old_vad_none_into_submission);
    RUN(wakeword_alone_times_out_without_submitting_and_does_not_rearm_before_quiescence);RUN(microphone_clock_and_sequence_wrap_keep_new_frame_detection);
    RUN(frame_newer_than_tick_is_not_consumed_before_clock_catches_up);
    RUN(unavailable_model_degrades_without_retrying_or_breaking_key_chat);printf("%u failure(s)\n",failures);return failures?1:0;
}
