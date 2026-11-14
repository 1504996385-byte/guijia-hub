#include "homehub_voice.h"
#include "tal_api.h"
#include "tkl_kws.h"
#include "tkl_vad.h"
#define VOICE_TAIL_MS 1500u
#define VOICE_DETECTION_MAX_AGE_MS 1000u
#define VOICE_QUIET_WAIT_MS 3000u
#define VOICE_PCM_MAX_AGE_MS 200u
extern void tkl_ai_disable_vendor_vad(void);

static struct {
    MUTEX_HANDLE lock;
    bool attempted,ok,enabled,armed,latched;
    uint32_t epoch,detected_epoch,detected_ms;
    bool idle_tracking,waiting_quiet;
    uint32_t idle_since,quiet_since,last_sequence,capture_turn,submit_logged_turn;
} voice;

static void next_epoch_locked(void){voice.epoch++;if(!voice.epoch)voice.epoch++;}
/* Runs in the SDK KWS task. It must not enqueue reliable work, call the core,
 * change audio ownership, or wait for networking/files/UI. */
static void detected(TKL_KWS_WAKEUP_WORD_E word){
    if(word!=TKL_KWS_WAKEUP_NIHAO_TUYA && word!=TKL_KWS_WAKEUP_XIAOZHI_TONGXUE)return;
    if(!voice.lock)return;
    uint32_t now=(uint32_t)tal_system_get_millisecond();
    tal_mutex_lock(voice.lock);
    if(voice.ok && voice.armed && voice.enabled && !voice.latched){
        voice.detected_epoch=voice.epoch;voice.detected_ms=now;voice.latched=true;
    }
    tal_mutex_unlock(voice.lock);
}
bool hh_voice_init(void){
    if(voice.attempted)return voice.ok;
    voice.attempted=true;
    /* The vendor gate can stop PCM delivery while quiet. Disable it before
     * the existing sole codec is opened, as required by the T5 adapter. */
    tkl_ai_disable_vendor_vad();
    OPERATE_RET rc=tal_mutex_create_init(&voice.lock);
    if(rc!=OPRT_OK){PR_NOTICE("HH voice init stage=mutex rc=%d",(int)rc);return false;}
    rc=tkl_kws_init();
    if(rc==OPRT_OK)rc=tkl_kws_reg_wakeup_cb(detected);
    if(rc!=OPRT_OK){PR_NOTICE("HH voice init stage=kws rc=%d",(int)rc);return false;}
    TKL_VAD_CONFIG_T cfg={.sample_rate=16000,.channel_num=1,.speech_min_ms=200,
        .noise_min_ms=1000,.frame_duration_ms=10,.scale=1.0f};
    rc=tkl_vad_init(&cfg);
    if(rc==OPRT_OK)rc=tkl_vad_start();
    if(rc!=OPRT_OK){PR_NOTICE("HH voice init stage=vad rc=%d",(int)rc);return false;}
    tal_mutex_lock(voice.lock);voice.ok=true;tal_mutex_unlock(voice.lock);
    /* KWS init launches an asynchronous model task. This is a request result,
     * never evidence that a real phrase has been acoustically recognized. */
    PR_NOTICE("HH voice init request_ok=1 model_async=1");return true;
}
static void disarm(void){
    tal_mutex_lock(voice.lock);bool enabled=voice.enabled;
    if(voice.armed || voice.enabled || voice.latched)next_epoch_locked();
    voice.enabled=voice.armed=voice.latched=false;
    tal_mutex_unlock(voice.lock);
    if(enabled)(void)tkl_kws_disable();
}
static bool fresh_pcm(uint32_t now,uint32_t sequence,uint32_t last_ms){
    if(sequence==voice.last_sequence)return false;
    /* The microphone can advance after the caller captured its clock. Keep
     * that frame available for the next tick instead of consuming it as old. */
    if((int32_t)(now-last_ms)<0)return false;
    voice.last_sequence=sequence;
    return (uint32_t)(now-last_ms)<=VOICE_PCM_MAX_AGE_MS;
}
static bool idle_allowed(const HH_Core *c,bool audio_ready){
    return audio_ready && c->audio_state==HH_AUDIO_IDLE && c->chat_availability==HH_CHAT_READY &&
        !c->queue_count && c->chat_local_recipient<0 && !c->chat_key_requires_release &&
        !c->chat_button.candidate && !c->chat_button.stable &&
        !c->buttons[0].candidate && !c->buttons[0].stable &&
        !c->buttons[1].candidate && !c->buttons[1].stable;
}
void hh_voice_tick(HH_Core *core,uint32_t now,bool audio_ready,uint32_t sequence,uint32_t last_ms){
    if(!voice.ok || !core)return;
    if(core->chat_from_voice && core->audio_state==HH_AUDIO_CHAT_WAIT && voice.submit_logged_turn!=core->chat_turn){
        voice.submit_logged_turn=core->chat_turn;PR_NOTICE("HH voice submit turn=%u",(unsigned)core->chat_turn);
    }
    bool idle=idle_allowed(core,audio_ready);
    if(voice.waiting_quiet){
        /* Keep the wakeword itself out of the utterance. Wait for a newly
         * observed quiet frame; stale NONE or frozen PCM cannot start it. */
        if(!idle || (uint32_t)(now-voice.quiet_since)>=VOICE_QUIET_WAIT_MS){
            if(idle)PR_NOTICE("HH voice quiet_timeout=1");
            voice.waiting_quiet=false;voice.idle_tracking=false;disarm();return;
        }
        if(fresh_pcm(now,sequence,last_ms) && tkl_vad_get_status()==TKL_VAD_STATUS_NONE){
            voice.waiting_quiet=false;voice.idle_tracking=false;
            if(hh_chat_voice_wake(core,now)==HH_OK){
                voice.capture_turn=core->chat_turn;voice.last_sequence=sequence;voice.submit_logged_turn=0;
                PR_NOTICE("HH voice capture turn=%u",(unsigned)core->chat_turn);
            }
        }
        return;
    }
    if(!idle){
        disarm();voice.idle_tracking=false;
        if(audio_ready && core->audio_state==HH_AUDIO_CHAT_CAPTURE && core->chat_from_voice){
            if(voice.capture_turn!=core->chat_turn){voice.capture_turn=core->chat_turn;voice.last_sequence=sequence;}
            else if(fresh_pcm(now,sequence,last_ms)){
                bool spoke=core->chat_voice_speech;
                (void)hh_chat_voice_activity(core,core->chat_turn,tkl_vad_get_status()==TKL_VAD_STATUS_SPEECH,now);
                if(!spoke && core->chat_voice_speech)PR_NOTICE("HH voice speech turn=%u",(unsigned)core->chat_turn);
                if(core->audio_state==HH_AUDIO_CHAT_WAIT && voice.submit_logged_turn!=core->chat_turn){
                    voice.submit_logged_turn=core->chat_turn;PR_NOTICE("HH voice submit turn=%u",(unsigned)core->chat_turn);
                }
            }
        }else voice.capture_turn=0;
        return;
    }
    voice.capture_turn=0;
    if(!voice.idle_tracking){voice.idle_tracking=true;voice.idle_since=now;}
    if((uint32_t)(now-voice.idle_since)<VOICE_TAIL_MS)return;
    tal_mutex_lock(voice.lock);bool enabled=voice.enabled;tal_mutex_unlock(voice.lock);
    if(!enabled){
        OPERATE_RET rc=tkl_kws_enable();
        if(rc!=OPRT_OK){
            disarm();tal_mutex_lock(voice.lock);voice.ok=false;tal_mutex_unlock(voice.lock);
            PR_NOTICE("HH voice enable rc=%d",(int)rc);return;
        }
        tal_mutex_lock(voice.lock);next_epoch_locked();voice.latched=false;voice.enabled=voice.armed=true;tal_mutex_unlock(voice.lock);
    }
    tal_mutex_lock(voice.lock);
    bool wake=voice.latched && voice.detected_epoch==voice.epoch &&
        (uint32_t)(now-voice.detected_ms)<=VOICE_DETECTION_MAX_AGE_MS;
    voice.latched=false;tal_mutex_unlock(voice.lock);
    if(wake){
        disarm();voice.waiting_quiet=true;voice.quiet_since=now;
        voice.last_sequence=sequence;voice.idle_tracking=false;
        PR_NOTICE("HH voice detected=1");
    }
}
