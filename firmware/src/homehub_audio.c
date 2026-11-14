#include "homehub_platform.h"
#include "homehub_chat.h"
#include "board_com_api.h"
#include "tdl_audio_manage.h"
#include "tkl_memory.h"
#include <stdio.h>
#include <string.h>

QUEUE_HANDLE hh_audio_jobs;
static TDL_AUDIO_HANDLE_T audio;
static MUTEX_HANDLE audio_mutex;
static uint8_t *capture;
static uint32_t captured;
static uint32_t recording_frames,recording_bad_frames;
static bool recording;
static bool audio_ready;
static uint32_t microphone_sequence,microphone_last_ms;
static char recording_id[HH_ID_MAX+1];
static MUTEX_HANDLE chat_sink_mutex;
static uint32_t chat_owner_turn,chat_captured_bytes,chat_failure_turn;
static bool chat_capture_open,chat_sink_open,chat_submitted,chat_failed,chat_stopping,local_playing;
static uint32_t chat_sink_deadline_ms;
static bool chat_sink_has_output;

static void chat_report(uint32_t turn,HH_ChatEvent report,bool ok)
{
    HH_Event event={.type=EV_CHAT_REPORT,.chat_event=report,.ok=ok};
    event.action.chat_turn=turn;hh_post(&event);
}
static void chat_fail_locked(void)
{
    chat_capture_open=false;chat_sink_open=false;chat_stopping=true;
    if(!chat_failed){chat_failed=true;chat_failure_turn=chat_owner_turn;}
}
/* Caller holds the sink mutex, never the microphone mutex. Serializing this
 * with quiesce keeps a deferred FAILED ahead of the backend's QUIESCED report. */
static void flush_chat_failure_locked(void)
{
    uint32_t turn;
    tal_mutex_lock(audio_mutex);turn=chat_failure_turn;chat_failure_turn=0;tal_mutex_unlock(audio_mutex);
    if(turn)chat_report(turn,HH_CHAT_FAILED,false);
}
static void flush_chat_failure(void)
{
    if(!audio_mutex||!chat_sink_mutex)return;
    tal_mutex_lock(chat_sink_mutex);flush_chat_failure_locked();tal_mutex_unlock(chat_sink_mutex);
}

static void microphone(TDL_AUDIO_FRAME_FORMAT_E format,TDL_AUDIO_STATUS_E status,uint8_t *data,uint32_t len)
{
    (void)status;
    if(!audio_mutex||!len)return;
    tal_mutex_lock(audio_mutex);
    if(format==TDL_AUDIO_FRAME_FORMAT_PCM&&data&&!(len&1u)){
        if(!++microphone_sequence)++microphone_sequence;
        microphone_last_ms=(uint32_t)tal_system_get_millisecond();
    }
    if(chat_owner_turn&&chat_capture_open){
        uint32_t turn=chat_owner_turn;
        if(format!=TDL_AUDIO_FRAME_FORMAT_PCM||!data||(len&1u)){
            chat_fail_locked();hh_chat_cloud_cancel(turn);
        }else{
            uint32_t room=HH_WAV_MAX-44-chat_captured_bytes;
            if(len>room)len=room;
            for(uint32_t offset=0;offset<len;){
                uint32_t count=len-offset;if(count>640u)count=640u;
                if(!hh_chat_cloud_pcm(turn,data+offset,count)){
                    chat_fail_locked();hh_chat_cloud_cancel(turn);break;
                }
                chat_captured_bytes+=count;offset+=count;
            }
            if(chat_captured_bytes==HH_WAV_MAX-44)chat_capture_open=false;
        }
    }else if(recording&&capture){
        if(format==TDL_AUDIO_FRAME_FORMAT_PCM&&data){
            uint32_t room=HH_WAV_MAX-44-captured;
            if(len>room)len=room;
            memcpy(capture+44+captured,data,len);captured+=len;recording_frames++;
        }else recording_bad_frames++;
    }
    tal_mutex_unlock(audio_mutex);
}
bool hh_audio_init(void)
{
    audio_ready=false;
    microphone_sequence=microphone_last_ms=0;
    if(tal_mutex_create_init(&audio_mutex)!=OPRT_OK||tal_mutex_create_init(&chat_sink_mutex)!=OPRT_OK)return false;
    tal_queue_create_init(&hh_audio_jobs,sizeof(HH_Action),8);
    if(tdl_audio_find(AUDIO_CODEC_NAME,&audio)!=OPRT_OK || tdl_audio_open(audio,microphone)!=OPRT_OK)return false;
    TDL_AUDIO_INFO_T info={0};
    if(tdl_audio_get_info(audio,&info)!=OPRT_OK || info.sample_rate!=16000 || info.sample_bits!=16 || info.sample_ch_num!=1)return false;
    audio_ready=tdl_audio_volume_set(audio,hh_device.volume)==OPRT_OK;
    return audio_ready;
}
bool hh_audio_voice_progress(uint32_t *sequence,uint32_t *last_frame_ms)
{
    if(!sequence||!last_frame_ms)return false;
    *sequence=*last_frame_ms=0;
    if(!audio_mutex)return false;
    tal_mutex_lock(audio_mutex);
    *sequence=microphone_sequence;*last_frame_ms=microphone_last_ms;
    bool ready=audio_ready&&microphone_sequence&&!recording&&!capture&&!local_playing;
    tal_mutex_unlock(audio_mutex);return ready;
}
static void start_record(const HH_Action *action)
{
    HH_Event event={.type=EV_RECORD_STARTED,.action=*action};
    if(audio_ready && hh_storage_ready){
        uint8_t *buffer=tkl_system_psram_malloc(HH_WAV_MAX);
        if(buffer){
            tal_mutex_lock(audio_mutex);
            if(!chat_owner_turn&&!local_playing&&!recording&&!capture){
                capture=buffer;captured=recording_frames=recording_bad_frames=0;recording=true;buffer=NULL;
                snprintf(recording_id,sizeof(recording_id),"%s",action->id);event.ok=true;
            }
            tal_mutex_unlock(audio_mutex);
            if(buffer)tkl_system_psram_free(buffer);
        }
    }
    PR_NOTICE("HH record-start ok=%u sd=%u audio=%u",(unsigned)event.ok,(unsigned)hh_storage_ready,(unsigned)audio_ready);
    hh_post(&event);
}
static void stop_record(const HH_Action *action,bool cancel)
{
    uint8_t *buffer;uint32_t bytes,frames,bad_frames;
    tal_mutex_lock(audio_mutex);recording=false;buffer=capture;capture=NULL;bytes=captured;
    frames=recording_frames;bad_frames=recording_bad_frames;
    tal_mutex_unlock(audio_mutex);
    HH_Event event={.type=EV_RECORD_SAVED,.action=*action};
    bool id_ok=hh_valid_id(recording_id),wav_ok=false;
    /* Reaching the 30-second allocation is a successful bounded recording;
       additional callback frames are ignored rather than discarding it. */
    if(!cancel && buffer && bytes>=8000u && id_ok){
        bytes&=~1u;hh_wav_header(buffer,bytes);
        snprintf(event.action.audio_path,sizeof(event.action.audio_path),HH_ROOT "/messages/%s.wav",recording_id);
        wav_ok=hh_wav_validate(buffer,bytes+44,NULL);
        if(wav_ok)event.ok=hh_file_write_atomic(event.action.audio_path,buffer,bytes+44);
    }
    const char *reason=cancel?"cancel":!buffer?"no_buffer":bytes<8000u?"short_pcm":!id_ok?"invalid_id":!wav_ok?"invalid_wav":event.ok?"saved":"storage";
    PR_NOTICE("HH record-stop cancel=%u pcm_bytes=%u frames=%u bad_frames=%u valid_id=%u wav=%u write=%u reason=%s",
        (unsigned)cancel,(unsigned)bytes,(unsigned)frames,(unsigned)bad_frames,(unsigned)id_ok,(unsigned)wav_ok,(unsigned)event.ok,reason);
    if(buffer)tkl_system_psram_free(buffer);
    if(!cancel)hh_post(&event);
}
void hh_audio_cancel(void)
{
    HH_Action action={0};stop_record(&action,true);
}
static bool speaker_quiet(bool had_output,bool cancel)
{
    /* T5's clear-buffer is a no-op and its writer starts only after 1280
     * buffered bytes. Even cancellation must pad a short accepted fragment;
     * waiting alone would leave it queued for the next audio owner. */
    if(cancel&&tdl_audio_play_stop(audio)!=OPRT_OK)return false;
    bool padded=true;
    if(had_output){
        uint8_t silence[640]={0};
        for(unsigned i=0;i<3;i++){
            if(tdl_audio_play(audio,silence,sizeof(silence))!=OPRT_OK){padded=false;break;}
            tal_system_sleep(20);
        }
        tal_system_sleep(1200);
    }
    bool stopped=cancel||tdl_audio_play_stop(audio)==OPRT_OK;
    return padded&&stopped;
}
static bool play(const char *path)
{
    uint8_t *wav=NULL;uint32_t size=0,pcm=0;
    if(!audio_ready)return false;
    tal_mutex_lock(audio_mutex);
    bool allowed=!chat_owner_turn&&!local_playing&&!recording&&!capture;
    if(allowed)local_playing=true;
    tal_mutex_unlock(audio_mutex);
    if(!allowed)return false;
    if(!hh_file_read(path,&wav,&size,HH_WAV_MAX)||!hh_wav_validate(wav,size,&pcm)){
        if(wav)tkl_system_psram_free(wav);
        tal_mutex_lock(audio_mutex);local_playing=false;tal_mutex_unlock(audio_mutex);return false;
    }
    bool ok=true;uint32_t sent=0;uint32_t started=(uint32_t)tal_system_get_millisecond();
    /* The SDK reports queue acceptance, not DAC completion. Pace at 32kB/s to
       bound backlog; add silence to pass its 1280-byte startup threshold and
       conservatively drain the 32768-byte writer FIFO. This is not an acoustic
       or DMA confirmation; real hardware acceptance must check the final word. */
    while(sent<pcm){
        uint32_t count=pcm-sent;if(count>640)count=640;
        if(tdl_audio_play(audio,wav+44+sent,count)!=OPRT_OK){ok=false;break;}
        sent+=count;
        uint32_t deadline=started+(sent/32u);
        uint32_t now=(uint32_t)tal_system_get_millisecond();
        if((int32_t)(deadline-now)>0)tal_system_sleep(deadline-now);
    }
    bool quiet=speaker_quiet(sent>0,!ok);ok=ok&&quiet;
    tkl_system_psram_free(wav);
    /* A failed drain keeps this owner reserved rather than contaminating the
     * next local recording or cloud turn with an unconfirmed old PCM tail. */
    tal_mutex_lock(audio_mutex);local_playing=!quiet;tal_mutex_unlock(audio_mutex);return ok;
}
void hh_audio_chat_action(const HH_Action *action)
{
    uint32_t turn=action->chat_turn;bool started=false,failed=false;
    if(!turn)return;
    if(action->type==HH_ACT_CHAT_STOP){
        if(audio_mutex){
            tal_mutex_lock(audio_mutex);
            if(chat_owner_turn==turn){chat_capture_open=false;chat_sink_open=false;chat_stopping=true;}
            tal_mutex_unlock(audio_mutex);
        }
        /* Cancellation is an independent nonblocking latch, even if begin
         * never acquired an owner. Do not wait on the speaker mutex here. */
        hh_chat_cloud_cancel(turn);return;
    }
    if(!audio_mutex||!audio_ready){chat_report(turn,HH_CHAT_FAILED,false);return;}
    tal_mutex_lock(audio_mutex);
    if(action->type==HH_ACT_CHAT_START){
        if(chat_owner_turn==turn&&!chat_stopping){started=true;}
        else if(!chat_owner_turn&&!recording&&!capture&&!local_playing&&hh_chat_cloud_begin(turn)){
            chat_owner_turn=turn;chat_captured_bytes=0;chat_capture_open=true;
            chat_sink_has_output=false;
            chat_sink_open=chat_submitted=chat_failed=chat_stopping=false;started=true;
        }else failed=true;
    }else if(action->type==HH_ACT_CHAT_SUBMIT&&chat_owner_turn==turn){
        chat_capture_open=false;
        if(!chat_stopping&&!chat_submitted){
            chat_submitted=true;
            if(!hh_chat_cloud_submit(turn)){chat_fail_locked();hh_chat_cloud_cancel(turn);}
        }
    }
    tal_mutex_unlock(audio_mutex);
    if(started)chat_report(turn,HH_CHAT_STARTED,true);
    else if(failed)chat_report(turn,HH_CHAT_FAILED,false);
    flush_chat_failure();
}
bool hh_audio_chat_sink_begin(uint32_t turn)
{
    if(!turn||!audio_mutex||!chat_sink_mutex||!audio_ready)return false;
    tal_mutex_lock(chat_sink_mutex);tal_mutex_lock(audio_mutex);
    bool allowed=chat_owner_turn==turn&&chat_submitted&&!chat_capture_open&&!chat_stopping&&!chat_failed;
    if(allowed&&!chat_sink_open){
        chat_sink_open=true;
        chat_sink_deadline_ms=(uint32_t)tal_system_get_millisecond();
    }
    tal_mutex_unlock(audio_mutex);tal_mutex_unlock(chat_sink_mutex);return allowed;
}
static void chat_sink_failed(uint32_t turn)
{
    tal_mutex_lock(audio_mutex);
    if(chat_owner_turn==turn)chat_fail_locked();
    tal_mutex_unlock(audio_mutex);hh_chat_cloud_cancel(turn);
}
bool hh_audio_chat_sink_write(uint32_t turn,const uint8_t *pcm,uint32_t bytes)
{
    if(!turn||!audio_mutex||!chat_sink_mutex||!audio_ready)return false;
    bool ok=true;uint32_t sent=0;
    tal_mutex_lock(chat_sink_mutex);
    do{
        tal_mutex_lock(audio_mutex);
        bool allowed=chat_owner_turn==turn&&chat_sink_open&&!chat_stopping&&!chat_failed;
        tal_mutex_unlock(audio_mutex);
        if(!allowed){ok=false;break;}
        if((!pcm&&bytes)||(bytes&1u)){chat_sink_failed(turn);ok=false;break;}
        if(sent==bytes)break;
        uint32_t count=bytes-sent;if(count>640u)count=640u;
        uint32_t now=(uint32_t)tal_system_get_millisecond();
        /* A network pause must not permit an unpaced catch-up burst. */
        if((int32_t)(now-chat_sink_deadline_ms)>0)chat_sink_deadline_ms=now;
        if(tdl_audio_play(audio,(uint8_t*)pcm+sent,count)!=OPRT_OK){chat_sink_failed(turn);ok=false;break;}
        chat_sink_has_output=true;sent+=count;chat_sink_deadline_ms+=(count+31u)/32u;
        now=(uint32_t)tal_system_get_millisecond();
        if((int32_t)(chat_sink_deadline_ms-now)>0)tal_system_sleep(chat_sink_deadline_ms-now);
    }while(sent<bytes);
    flush_chat_failure_locked();tal_mutex_unlock(chat_sink_mutex);return ok;
}
void hh_audio_chat_sink_stop(uint32_t turn)
{
    if(!audio_mutex)return;
    tal_mutex_lock(audio_mutex);if(chat_owner_turn==turn)chat_sink_open=false;tal_mutex_unlock(audio_mutex);
}
bool hh_audio_chat_quiesce(uint32_t turn,bool natural_end)
{
    if(!audio_mutex||!chat_sink_mutex)return chat_owner_turn==0;
    tal_mutex_lock(chat_sink_mutex);tal_mutex_lock(audio_mutex);
    if(!chat_owner_turn){tal_mutex_unlock(audio_mutex);tal_mutex_unlock(chat_sink_mutex);return true;}
    if(chat_owner_turn!=turn){tal_mutex_unlock(audio_mutex);tal_mutex_unlock(chat_sink_mutex);return false;}
    chat_capture_open=false;chat_sink_open=false;chat_stopping=true;
    bool natural=natural_end&&!chat_failed;
    tal_mutex_unlock(audio_mutex);
    flush_chat_failure_locked();
    bool quiet=speaker_quiet(chat_sink_has_output,!natural);
    if(!quiet)chat_sink_failed(turn);
    flush_chat_failure_locked();
    if(quiet){
        tal_mutex_lock(audio_mutex);
        chat_owner_turn=0;chat_capture_open=chat_sink_open=chat_submitted=chat_stopping=false;
        chat_sink_has_output=false;
        tal_mutex_unlock(audio_mutex);
    }
    tal_mutex_unlock(chat_sink_mutex);return quiet;
}
static void prepare_prompt(const HH_Action *action)
{
    HH_Event event={.type=EV_GREETING_READY,.action=*action};
    uint8_t *data=NULL;uint32_t bytes=0;
    if(action->type==HH_ACT_KEY_REMINDER_REQUEST)
        snprintf(event.action.audio_path,sizeof(event.action.audio_path),HH_ROOT "/voices/take_key.wav");
    event.ok=hh_file_read(event.action.audio_path,&data,&bytes,HH_WAV_MAX)&&hh_wav_validate(data,bytes,NULL);
    if(data)tkl_system_psram_free(data);
    /* A doorway reminder has one meaning and one fixed recording. Only normal
     * greetings may fall back from their selected variant to an alias. */
    if(!event.ok&&action->type==HH_ACT_GREETING_REQUEST){
        data=NULL;
        snprintf(event.action.audio_path,sizeof(event.action.audio_path),HH_ROOT "/voices/%s.wav",action->trigger==HH_ARRIVE?"arrive":"leave");
        event.ok=hh_file_read(event.action.audio_path,&data,&bytes,HH_WAV_MAX)&&hh_wav_validate(data,bytes,NULL);
        if(data)tkl_system_psram_free(data);
    }
    hh_post(&event);
}
void hh_audio_worker(void *unused)
{
    (void)unused;HH_Action action;
    while(1){
      flush_chat_failure();
      if(tal_queue_fetch(hh_audio_jobs,&action,20)==OPRT_OK){
        switch(action.type){
        case HH_ACT_RECORD_START:start_record(&action);break;
        case HH_ACT_RECORD_STOP_SAVE:stop_record(&action,false);break;
        case HH_ACT_RECORD_CANCEL:stop_record(&action,true);break;
        case HH_ACT_CHAT_START:case HH_ACT_CHAT_SUBMIT:case HH_ACT_CHAT_STOP:hh_audio_chat_action(&action);break;
        case HH_ACT_GREETING_REQUEST:case HH_ACT_KEY_REMINDER_REQUEST:prepare_prompt(&action);break;
        case HH_ACT_PLAY_AUDIO:{
            HH_Event event={.type=EV_PLAY_FINISHED,.action=action};event.ok=play(action.audio_path);hh_post(&event);break;
        }
        default:break;
        }
      }
    }
}
