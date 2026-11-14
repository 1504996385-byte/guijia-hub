#include "homehub_core.h"
#include <stdio.h>
#include <string.h>

static size_t bounded_length(const char *s, size_t cap) {
    size_t i; if (!s) return cap;
    for (i=0;i<cap;i++) if (!s[i]) return i;
    return cap;
}
static bool valid_id(const char *id) {
    size_t i, n=bounded_length(id,HH_ID_MAX+1);
    if (!n || n>HH_ID_MAX) return false;
    for(i=0;i<n;i++) {
        unsigned char ch=(unsigned char)id[i];
        if (!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
              (ch>='0'&&ch<='9')||ch=='_'||ch=='-')) return false;
    }
    return true;
}
static bool valid_path(const char *path) {
    size_t n=bounded_length(path,HH_PATH_MAX+1);
    return n>0 && n<=HH_PATH_MAX;
}
static bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
    return (uint32_t)(now-since)>=interval;
}
size_t hh_pending_count(const HH_Core *c, unsigned recipient) {
    size_t i,n=0; if (!c || recipient>1) return 0;
    for(i=0;i<HH_MAX_MESSAGES;i++) if(c->store.used[i] &&
        c->store.messages[i].recipient==recipient && c->store.messages[i].state!=HH_PLAYED) n++;
    return n;
}
static int find_index(const HH_Core *c,const char *id) {
    int i; if(!c||!valid_id(id)) return -1;
    for(i=0;i<HH_MAX_MESSAGES;i++) if(c->store.used[i] && strcmp(c->store.messages[i].id,id)==0) return i;
    return -1;
}
const HH_Message *hh_find_message(const HH_Core *c,const char *id) {
    int i=find_index(c,id); return i<0?NULL:&c->store.messages[i];
}
static void emit(HH_Core *c,HH_ActionType type,unsigned recipient,HH_Trigger trigger,
                 HH_Result result,const HH_Message *message) {
    HH_Action a; memset(&a,0,sizeof(a)); a.type=type;
    a.recipient=(uint8_t)recipient; a.trigger=trigger; a.result=result; a.event_seq=c->event_seq;
    a.chat_turn=c->chat_turn;
    a.keys_present[0]=c->keys[0].initialized&&c->keys[0].stable;
    a.keys_present[1]=c->keys[1].initialized&&c->keys[1].stable;
    a.pending_count[0]=(uint8_t)hh_pending_count(c,0);
    a.pending_count[1]=(uint8_t)hh_pending_count(c,1);
    if(message) { memcpy(a.id,message->id,sizeof(a.id)); memcpy(a.audio_path,message->audio_path,sizeof(a.audio_path)); a.source=(HH_Source)message->source; }
    if(c->action) c->action(c->context,&a);
}
static void record_action(HH_Core *c,HH_ActionType type,HH_Result result) {
    HH_Message m; memset(&m,0,sizeof(m));
    memcpy(m.id,c->record_id,sizeof(m.id)); m.recipient=c->record_recipient;
    m.trigger=(uint8_t)c->record_trigger;
    emit(c,type,m.recipient,c->record_trigger,result,&m);
}
static void persist(HH_Core *c) { emit(c,HH_ACT_PERSIST,0,HH_ARRIVE,HH_OK,NULL); }
static void ui(HH_Core *c) { emit(c,HH_ACT_UI,0,HH_ARRIVE,HH_OK,NULL); }
static bool chat_owns_audio(const HH_Core *c) {
    return c->audio_state==HH_AUDIO_CHAT_CAPTURE || c->audio_state==HH_AUDIO_CHAT_WAIT ||
           c->audio_state==HH_AUDIO_CHAT_PLAY || c->audio_state==HH_AUDIO_CHAT_STOPPING;
}
static void request_chat_stop(HH_Core *c,HH_Result reason) {
    if(!chat_owns_audio(c)||c->audio_state==HH_AUDIO_CHAT_STOPPING) return;
    c->audio_state=HH_AUDIO_CHAT_STOPPING;
    c->chat_stop_since_ms=c->now_ms;
    c->chat_stop_notice_sent=false;
    emit(c,HH_ACT_CHAT_STOP,0,HH_ARRIVE,reason,NULL);
    if(reason!=HH_OK)emit(c,HH_ACT_FEEDBACK,0,HH_ARRIVE,reason,NULL);
    ui(c);
}
static void pump(HH_Core *c) {
    unsigned i; HH_Message *m;
    if(c->audio_state!=HH_AUDIO_IDLE) return;
    if(c->chat_local_recipient>=0) {
        if(c->buttons[(unsigned)c->chat_local_recipient].candidate)return;
        c->chat_local_recipient=-1;
    }
    while(c->queue_count) {
        i=c->play_queue[c->queue_head]; c->queue_head=(uint8_t)((c->queue_head+1)%HH_MAX_MESSAGES); c->queue_count--;
        if(i>=HH_MAX_MESSAGES || !c->store.used[i]) continue;
        m=&c->store.messages[i]; if(m->state!=HH_QUEUED) continue;
        m->state=HH_PLAYING; c->playing_index=(int)i; c->audio_state=HH_AUDIO_PLAYING;
        emit(c,HH_ACT_PLAY_AUDIO,m->recipient,(HH_Trigger)m->trigger,HH_OK,m);
        return;
    }
}
static int free_index(const HH_Core *c) {
    int i; for(i=0;i<HH_MAX_MESSAGES;i++) if(!c->store.used[i]) return i;
    for(i=0;i<HH_MAX_MESSAGES;i++) if(c->store.messages[i].state==HH_PLAYED) return i;
    return -1;
}
static bool seen_id(const HH_Core *c,const char *id) {
    unsigned i; if(find_index(c,id)>=0) return true;
    for(i=0;i<c->store.seen_count;i++) if(strcmp(c->store.seen_ids[i],id)==0) return true;
    return false;
}
static void remember_played(HH_Core *c,const char *id) {
    unsigned i; for(i=0;i<c->store.seen_count;i++) if(strcmp(c->store.seen_ids[i],id)==0) return;
    /* id may point into another subobject of c. memmove deliberately allows
     * aliasing; snprintf's restrict contract cannot make that guarantee. */
    memmove(c->store.seen_ids[c->store.seen_next],id,strlen(id)+1);
    c->store.seen_next=(uint16_t)((c->store.seen_next+1)%HH_SEEN_IDS);
    if(c->store.seen_count<HH_SEEN_IDS) c->store.seen_count++;
}
static bool valid_message_trigger(const HH_Message *message){
    return message->trigger<=HH_LEAVE || (message->trigger==HH_ANY&&message->source==HH_REMOTE);
}
static HH_Result add_message(HH_Core *c,const HH_Message *message,bool local_commit) {
    int i; size_t pending;
    if(!c || !message || !valid_id(message->id)||!valid_path(message->audio_path)||
       message->recipient>1||!valid_message_trigger(message)||message->source>HH_REMOTE) return HH_INVALID;
    if(seen_id(c,message->id)) {
        int existing=find_index(c,message->id);
        /* Poll repeats unacknowledged remote messages. Retry its played receipt
         * instead of replaying audio when an earlier HTTP receipt was lost. */
        if(message->source==HH_REMOTE && (existing<0||c->store.messages[existing].state==HH_PLAYED))
            emit(c,HH_ACT_PLAYED_RECEIPT,message->recipient,(HH_Trigger)message->trigger,HH_OK,message);
        return HH_DUPLICATE;
    }
    pending=hh_pending_count(c,0)+hh_pending_count(c,1);
    /* Reserve one slot while a new local recording is in flight. */
    if(!local_commit && (c->audio_state==HH_AUDIO_RECORDING||c->audio_state==HH_AUDIO_SAVING) && pending>=HH_MAX_MESSAGES-1) return HH_FULL;
    i=free_index(c); if(i<0) return HH_FULL;
    c->store.messages[i]=*message; c->store.messages[i].state=HH_PENDING; c->store.used[i]=1;
    persist(c); ui(c); return HH_OK;
}
HH_Result hh_add_message(HH_Core *c,const HH_Message *message) { return add_message(c,message,false); }

HH_Config hh_default_config(void) {
    HH_Config config; memset(&config,0,sizeof(config));
    config.debounce_ms=60; config.minimum_record_ms=250; config.maximum_record_ms=30000;
    config.pir_warmup_ms=10000; config.pir_quiet_ms=30000; return config;
}
void hh_init(HH_Core *c,const HH_Config *config,HH_ActionFn action,void *context,uint32_t now_ms) {
    HH_Config defaults=hh_default_config(); if(!c) return;
    memset(c,0,sizeof(*c)); c->config=config?*config:defaults;
    if(!c->config.debounce_ms) c->config.debounce_ms=defaults.debounce_ms;
    if(!c->config.minimum_record_ms) c->config.minimum_record_ms=defaults.minimum_record_ms;
    if(!c->config.maximum_record_ms||c->config.maximum_record_ms>30000) c->config.maximum_record_ms=30000;
    if(c->config.minimum_record_ms>c->config.maximum_record_ms) c->config.minimum_record_ms=c->config.maximum_record_ms;
    c->action=action; c->context=context; c->boot_ms=now_ms; c->now_ms=now_ms;
    c->timed_reminder_last_ms=now_ms;
    c->store.version=HH_SNAPSHOT_VERSION; c->playing_index=-1;
    c->next_trigger[0]=HH_TRIGGER_AUTO; c->next_trigger[1]=HH_TRIGGER_AUTO;
    c->chat_local_recipient=-1;
}
/* Returns one only for a new debounced edge, never for the first observation. */
static bool debounce(HH_Debouncer *d,bool raw,uint32_t now,uint32_t interval) {
    if(!d->initialized) { d->initialized=true; d->stable=raw; d->candidate=raw; d->candidate_since=now; return false; }
    if(raw!=d->candidate) { d->candidate=raw; d->candidate_since=now; }
    if(d->candidate!=d->stable && elapsed(now,d->candidate_since,interval)) { d->stable=d->candidate; return true; }
    return false;
}
static void start_recording(HH_Core *c,unsigned recipient) {
    if(c->audio_state!=HH_AUDIO_IDLE) { emit(c,HH_ACT_FEEDBACK,recipient,HH_ARRIVE,HH_BUSY,NULL); return; }
    if(free_index(c)<0) { emit(c,HH_ACT_FEEDBACK,recipient,HH_ARRIVE,HH_FULL,NULL); return; }
    c->record_recipient=(uint8_t)recipient;
    c->record_trigger=c->next_trigger[recipient]==HH_TRIGGER_AUTO?
        (c->keys[recipient].stable?HH_LEAVE:HH_ARRIVE):(HH_Trigger)c->next_trigger[recipient];
    c->next_trigger[recipient]=HH_TRIGGER_AUTO;
    c->recording_seq++; snprintf(c->record_id,sizeof(c->record_id),"r_%lu_%lu",(unsigned long)c->config.boot_id,(unsigned long)c->recording_seq);
    c->record_start_ms=c->now_ms; c->audio_state=HH_AUDIO_RECORDING;
    record_action(c,HH_ACT_RECORD_START,HH_OK); ui(c);
}
static void stop_recording(HH_Core *c) {
    if(c->audio_state!=HH_AUDIO_RECORDING) return;
    if(!elapsed(c->now_ms,c->record_start_ms,c->config.minimum_record_ms)) {
        c->audio_state=HH_AUDIO_IDLE; record_action(c,HH_ACT_RECORD_CANCEL,HH_TOO_SHORT);
        record_action(c,HH_ACT_FEEDBACK,HH_TOO_SHORT); ui(c);
    } else { c->audio_state=HH_AUDIO_SAVING; record_action(c,HH_ACT_RECORD_STOP_SAVE,HH_OK); ui(c); }
}
static void key_event(HH_Core *c,unsigned recipient) {
    unsigned i,tail; HH_Trigger trigger=c->keys[recipient].stable?HH_ARRIVE:HH_LEAVE;
    c->event_seq++; emit(c,HH_ACT_KEY_EVENT,recipient,trigger,HH_OK,NULL);
    for(i=0;i<HH_MAX_MESSAGES;i++) {
        HH_Message *m=&c->store.messages[i];
        if(c->store.used[i] && m->state==HH_PENDING && m->recipient==recipient &&
           (m->trigger==(uint8_t)trigger || (m->trigger==HH_ANY&&m->source==HH_REMOTE))) {
            if(c->queue_count>=HH_MAX_MESSAGES) { emit(c,HH_ACT_FEEDBACK,recipient,trigger,HH_FULL,m); continue; }
            tail=(c->queue_head+c->queue_count)%HH_MAX_MESSAGES; c->play_queue[tail]=(uint8_t)i;
            c->queue_count++; m->state=HH_QUEUED;
        }
    }
    ui(c);
}
static bool any_key_present(const HH_Core *c) {
    return (c->keys[0].initialized && c->keys[0].stable) ||
           (c->keys[1].initialized && c->keys[1].stable);
}
static HH_Result begin_chat(HH_Core *c,const bool buttons_held[2],bool from_voice) {
    if(c->chat_availability!=HH_CHAT_READY || c->audio_state!=HH_AUDIO_IDLE || c->chat_key_requires_release ||
       c->queue_count || buttons_held[0] || buttons_held[1]) {
        if(!from_voice){emit(c,HH_ACT_FEEDBACK,0,HH_ARRIVE,HH_BUSY,NULL);ui(c);}
        return HH_BUSY;
    }
    c->chat_sequence++;
    if(!c->chat_sequence)c->chat_sequence++;
    c->chat_turn=c->chat_sequence;
    c->chat_started_ms=c->now_ms;
    c->chat_start_confirmed=false;
    c->chat_stop_notice_sent=false;
    c->chat_local_recipient=-1;
    c->chat_from_voice=from_voice;c->chat_voice_speech=false;
    c->audio_state=HH_AUDIO_CHAT_CAPTURE;
    emit(c,HH_ACT_CHAT_START,0,HH_ARRIVE,HH_OK,NULL);
    ui(c);
    return HH_OK;
}
static void finish_chat_capture(HH_Core *c) {
    if(c->audio_state!=HH_AUDIO_CHAT_CAPTURE)return;
    if(c->chat_from_voice&&!c->chat_voice_speech){request_chat_stop(c,HH_OK);return;}
    if(!elapsed(c->now_ms,c->chat_started_ms,HH_CHAT_CAPTURE_MIN_MS)) {
        request_chat_stop(c,HH_TOO_SHORT);
        return;
    }
    c->audio_state=HH_AUDIO_CHAT_WAIT;
    c->chat_wait_since_ms=c->now_ms;
    emit(c,HH_ACT_CHAT_SUBMIT,0,HH_ARRIVE,HH_OK,NULL);
    ui(c);
}
static bool chat_capture_expired(HH_Core *c) {
    if(c->audio_state!=HH_AUDIO_CHAT_CAPTURE)return false;
    if(c->chat_from_voice&&!c->chat_voice_speech&&
       elapsed(c->now_ms,c->chat_started_ms,HH_CHAT_VOICE_NO_SPEECH_MS)) {
        request_chat_stop(c,HH_OK);return true;
    }
    if(elapsed(c->now_ms,c->chat_started_ms,HH_CHAT_CAPTURE_MAX_MS)) {
        finish_chat_capture(c);return true;
    }
    return false;
}
HH_Result hh_chat_voice_wake(HH_Core *c,uint32_t now) {
    if(!c)return HH_INVALID;
    if(c->chat_button.candidate||c->chat_button.stable||c->buttons[0].stable||c->buttons[1].stable)
        return HH_BUSY;
    const bool held[]={c->buttons[0].candidate,c->buttons[1].candidate};
    c->now_ms=now;return begin_chat(c,held,true);
}
HH_Result hh_chat_voice_activity(HH_Core *c,uint32_t turn,bool speech,uint32_t now) {
    if(!c)return HH_INVALID;
    if(!turn||turn!=c->chat_turn)return HH_NOT_FOUND;
    if(c->audio_state!=HH_AUDIO_CHAT_CAPTURE||!c->chat_from_voice)return HH_BUSY;
    c->now_ms=now;
    if(chat_capture_expired(c))return HH_BUSY;
    if(speech)c->chat_voice_speech=true;
    else if(c->chat_voice_speech)finish_chat_capture(c);
    return HH_OK;
}
static void process_key_reminder(HH_Core *c,bool pir_active,const bool buttons_held[2],bool chat_key_held) {
    /* Keep the reservation until the in-flight preparation reports ready.
     * A transient all-keys-away state permanently cancels this lifecycle. */
    if (c->audio_state == HH_AUDIO_GREETING && c->greeting_is_key_reminder &&
        (!any_key_present(c) || elapsed(c->now_ms,c->key_reminder_prepared_since_ms,HH_KEY_REMINDER_WAIT_MS))) {
        c->key_reminder_preparation_cancelled = true;
        c->pir_wait_expired = true;
    }
    if (!pir_active || !c->pir_previous) c->pir_wait_expired = false;
    if (!c->config.pir_enabled) {
        c->key_reminder_pending = false;
        return;
    }
    bool awake=pir_active && elapsed(c->now_ms,c->boot_ms,c->config.pir_warmup_ms);
    bool fresh=awake && (!c->pir_announced || !c->pir_previous);
    if (fresh) {
        c->pir_announced = true;
        emit(c,HH_ACT_WAKE_SCREEN,0,HH_ARRIVE,HH_OK,NULL);
    }
    /* Retriggerable PIR modules can hold HIGH across the entire quiet period.
     * Recheck that level after quiet, without inventing repeated wake edges or
     * reviving an expired busy wait / an in-flight preparation. */
    if (fresh || (awake && !c->pir_wait_expired && !c->greeting_is_key_reminder)) {
        if (any_key_present(c) && !c->key_reminder_pending &&
            (!c->greeting_announced || elapsed(c->now_ms,c->last_pir_ms,c->config.pir_quiet_ms))) {
            c->key_reminder_pending = true;
            c->key_reminder_since_ms = c->now_ms;
        }
    }
    if (!c->key_reminder_pending) return;
    /* New edges coalesce before checking expiry, so activity while busy cannot
     * refresh the original ten-second deadline, including at its boundary. */
    if (!any_key_present(c) || elapsed(c->now_ms,c->key_reminder_since_ms,HH_KEY_REMINDER_WAIT_MS)) {
        c->key_reminder_pending = false;
        c->pir_wait_expired = true;
        return;
    }
    /* A raw press gets its debounce interval before a low-priority reminder
     * reserves audio. Keep the same pending deadline while waiting. */
    if (c->audio_state != HH_AUDIO_IDLE || c->queue_count || buttons_held[0] || buttons_held[1] || chat_key_held) return;
    c->key_reminder_pending = false;
    c->pir_wait_expired = false;
    c->greeting_is_key_reminder = true;
    c->key_reminder_preparation_cancelled = false;
    c->key_reminder_prepared_since_ms = c->key_reminder_since_ms;
    c->greeting_recipient = 0; /* unused by the anonymous dedicated action */
    c->greeting_trigger = HH_LEAVE;
    c->audio_state = HH_AUDIO_GREETING;
    c->greeting_announced = true;
    c->last_pir_ms = c->now_ms;
    emit(c,HH_ACT_KEY_REMINDER_REQUEST,0,HH_LEAVE,HH_OK,NULL);
}
static void process_timed_reminder(HH_Core *c,const bool buttons_held[2],bool chat_key_held) {
    if(!c->config.reminder_interval_ms ||
       !elapsed(c->now_ms,c->timed_reminder_last_ms,c->config.reminder_interval_ms)) return;
    /* Coalesce overdue ticks, and let raw presses complete debounce first. */
    if(c->audio_state!=HH_AUDIO_IDLE || c->queue_count ||
       buttons_held[0] || buttons_held[1] || chat_key_held) return;
    c->timed_reminder_last_ms=c->now_ms;
    c->greeting_is_key_reminder=false;
    c->greeting_is_timed_reminder=true;
    c->key_reminder_preparation_cancelled=false;
    c->key_reminder_prepared_since_ms=c->now_ms;
    c->greeting_recipient=0;c->greeting_trigger=HH_LEAVE;
    c->audio_state=HH_AUDIO_GREETING;
    emit(c,HH_ACT_KEY_REMINDER_REQUEST,0,HH_LEAVE,HH_OK,NULL);
}
void hh_sample_with_chat(HH_Core *c,uint32_t now_ms,const bool keys_present[2],const bool buttons_held[2],bool pir_active,bool chat_key_held) {
    bool key_changed[2],button_changed[2],chat_changed; unsigned i;
    if(!c||!keys_present||!buttons_held) return;
    c->now_ms=now_ms;
    for(i=0;i<2;i++) key_changed[i]=debounce(&c->keys[i],keys_present[i],now_ms,c->config.debounce_ms);
    for(i=0;i<2;i++) button_changed[i]=debounce(&c->buttons[i],buttons_held[i],now_ms,c->config.debounce_ms);
    chat_changed=debounce(&c->chat_button,chat_key_held,now_ms,c->config.debounce_ms);
    /* A press begun as cancellation belongs to that old turn, even if its
     * debounce edge arrives after QUIESCED. Only a stable release rearms KEY. */
    if(chat_key_held && (c->audio_state==HH_AUDIO_CHAT_WAIT || c->audio_state==HH_AUDIO_CHAT_PLAY ||
                        c->audio_state==HH_AUDIO_CHAT_STOPPING ||
                        (c->audio_state==HH_AUDIO_CHAT_CAPTURE&&c->chat_from_voice)))
        c->chat_key_requires_release=true;
    if(c->chat_key_requires_release && !chat_key_held && !c->chat_button.stable &&
       elapsed(now_ms,c->chat_button.candidate_since,c->config.debounce_ms))
        c->chat_key_requires_release=false;
    /* Record-button edges are handled before pumping any simultaneous key edges. */
    for(i=0;i<2;i++) if(button_changed[i]) {
        if(c->buttons[i].stable) {
            if(chat_owns_audio(c)) {
                if(c->chat_local_recipient<0 || !c->buttons[(unsigned)c->chat_local_recipient].candidate)
                    c->chat_local_recipient=(int8_t)i;
                request_chat_stop(c,HH_OK);
            } else {c->chat_local_recipient=-1;start_recording(c,i);}
        }
        else if(c->audio_state==HH_AUDIO_RECORDING&&c->record_recipient==i) stop_recording(c);
    }
    if(c->audio_state==HH_AUDIO_RECORDING && elapsed(now_ms,c->record_start_ms,c->config.maximum_record_ms)) stop_recording(c);
    for(i=0;i<2;i++) if(key_changed[i]) key_event(c,i);
    if(c->queue_count&&chat_owns_audio(c))request_chat_stop(c,HH_OK);
    pump(c);
    if(chat_changed) {
        if(c->chat_button.stable) {
            if(c->audio_state==HH_AUDIO_CHAT_WAIT||c->audio_state==HH_AUDIO_CHAT_PLAY||
               (c->audio_state==HH_AUDIO_CHAT_CAPTURE&&c->chat_from_voice))
                request_chat_stop(c,HH_OK);
            else if(c->audio_state==HH_AUDIO_CHAT_STOPPING)
                emit(c,HH_ACT_FEEDBACK,0,HH_ARRIVE,HH_BUSY,NULL);
            else begin_chat(c,buttons_held,false);
        } else if(c->audio_state==HH_AUDIO_CHAT_CAPTURE&&!c->chat_from_voice) finish_chat_capture(c);
    }
    chat_capture_expired(c);
    if(c->audio_state==HH_AUDIO_CHAT_WAIT&&elapsed(now_ms,c->chat_wait_since_ms,HH_CHAT_REPLY_WAIT_MS))
        request_chat_stop(c,HH_IO_ERROR);
    if(c->audio_state==HH_AUDIO_CHAT_STOPPING&&!c->chat_stop_notice_sent&&
       elapsed(now_ms,c->chat_stop_since_ms,HH_CHAT_STOP_NOTICE_MS)) {
        c->chat_stop_notice_sent=true;
        emit(c,HH_ACT_FEEDBACK,0,HH_ARRIVE,HH_IO_ERROR,NULL);
        ui(c);
    }
    process_key_reminder(c,pir_active,buttons_held,chat_key_held);
    process_timed_reminder(c,buttons_held,chat_key_held);
    c->pir_previous=pir_active;
}
void hh_sample(HH_Core *c,uint32_t now_ms,const bool keys_present[2],const bool buttons_held[2],bool pir_active) {
    hh_sample_with_chat(c,now_ms,keys_present,buttons_held,pir_active,false);
}
HH_Result hh_set_record_trigger(HH_Core *c,HH_Trigger trigger) {
    if(!c||trigger>HH_LEAVE) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_RECORDING) return HH_BUSY;
    c->record_trigger=trigger; ui(c); return HH_OK;
}
HH_Result hh_set_next_trigger(HH_Core *c,unsigned recipient,int trigger) {
    if(!c||recipient>1||(trigger!=HH_TRIGGER_AUTO&&trigger!=HH_ARRIVE&&trigger!=HH_LEAVE)) return HH_INVALID;
    c->next_trigger[recipient]=(int8_t)trigger; ui(c); return HH_OK;
}
void hh_recording_started(HH_Core *c,bool success) {
    if(!c||c->audio_state!=HH_AUDIO_RECORDING||success) return;
    c->audio_state=HH_AUDIO_IDLE; record_action(c,HH_ACT_RECORD_CANCEL,HH_IO_ERROR);
    record_action(c,HH_ACT_FEEDBACK,HH_IO_ERROR); ui(c); pump(c);
}
HH_Result hh_recording_saved(HH_Core *c,const char *id,const char *audio_path,bool success) {
    HH_Message m; HH_Result result;
    if(!c||!valid_id(id)) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_SAVING) return HH_BUSY;
    if(strcmp(id,c->record_id)!=0) return HH_NOT_FOUND;
    if(!success||!valid_path(audio_path)) {
        c->audio_state=HH_AUDIO_IDLE; record_action(c,HH_ACT_FEEDBACK,HH_IO_ERROR); ui(c); pump(c); return HH_IO_ERROR;
    }
    memset(&m,0,sizeof(m)); snprintf(m.id,sizeof(m.id),"%s",id); snprintf(m.audio_path,sizeof(m.audio_path),"%s",audio_path);
    m.recipient=c->record_recipient; m.trigger=(uint8_t)c->record_trigger; m.source=HH_LOCAL;
    result=add_message(c,&m,true); c->audio_state=HH_AUDIO_IDLE;
    if(result!=HH_OK) record_action(c,HH_ACT_FEEDBACK,result);
    ui(c); pump(c); return result;
}
HH_Result hh_playback_finished(HH_Core *c,const char *id,bool success) {
    HH_Message *m;
    if(!c||!valid_id(id)) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_PLAYING) return HH_BUSY;
    if(c->playing_index<0) {
        if(strcmp(id,"greeting")!=0) return HH_NOT_FOUND;
        c->audio_state=HH_AUDIO_IDLE;
        c->greeting_is_key_reminder=false;
        c->greeting_is_timed_reminder=false;
        c->key_reminder_preparation_cancelled=false;
        if(!success) emit(c,HH_ACT_FEEDBACK,c->greeting_recipient,c->greeting_trigger,HH_IO_ERROR,NULL);
        ui(c); pump(c); return HH_OK;
    }
    m=&c->store.messages[c->playing_index]; if(strcmp(m->id,id)!=0) return HH_NOT_FOUND;
    m->state=success?HH_PLAYED:HH_PENDING;
    if(success) { remember_played(c,m->id); persist(c); emit(c,HH_ACT_PLAYED_RECEIPT,m->recipient,(HH_Trigger)m->trigger,HH_OK,m); }
    else { persist(c); emit(c,HH_ACT_FEEDBACK,m->recipient,(HH_Trigger)m->trigger,HH_IO_ERROR,m); }
    c->playing_index=-1; c->audio_state=HH_AUDIO_IDLE; ui(c); pump(c); return HH_OK;
}
HH_Result hh_greeting_ready(HH_Core *c,const char *audio_path,bool available) {
    HH_Message m;
    if(!c) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_GREETING) return HH_BUSY;
    if((c->greeting_is_key_reminder || c->greeting_is_timed_reminder) &&
       (c->key_reminder_preparation_cancelled || (c->greeting_is_key_reminder&&!any_key_present(c)) || c->queue_count ||
        elapsed(c->now_ms,c->key_reminder_prepared_since_ms,HH_KEY_REMINDER_WAIT_MS))) {
        c->greeting_is_key_reminder=false;
        c->greeting_is_timed_reminder=false;
        c->key_reminder_preparation_cancelled=false;
        c->audio_state=HH_AUDIO_IDLE;
        ui(c);pump(c);return HH_OK;
    }
    if(!available||!valid_path(audio_path)) {
        c->greeting_is_key_reminder=false;
        c->greeting_is_timed_reminder=false;
        c->key_reminder_preparation_cancelled=false;
        c->audio_state=HH_AUDIO_IDLE; emit(c,HH_ACT_FEEDBACK,c->greeting_recipient,c->greeting_trigger,HH_MISSING_ASSET,NULL); ui(c); pump(c); return HH_MISSING_ASSET;
    }
    memset(&m,0,sizeof(m)); snprintf(m.id,sizeof(m.id),"greeting"); snprintf(m.audio_path,sizeof(m.audio_path),"%s",audio_path);
    m.recipient=c->greeting_recipient; m.trigger=(uint8_t)c->greeting_trigger;
    c->playing_index=-1; c->audio_state=HH_AUDIO_PLAYING; emit(c,HH_ACT_PLAY_AUDIO,m.recipient,c->greeting_trigger,HH_OK,&m); return HH_OK;
}
HH_Result hh_request_greeting(HH_Core *c,unsigned recipient,HH_Trigger trigger) {
    if(!c||recipient>1||(trigger!=HH_ARRIVE&&trigger!=HH_LEAVE)) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_IDLE||c->queue_count) return HH_BUSY;
    c->greeting_is_key_reminder=false;
    c->greeting_is_timed_reminder=false;
    c->key_reminder_preparation_cancelled=false;
    c->key_reminder_prepared_since_ms=0;
    c->greeting_recipient=(uint8_t)recipient; c->greeting_trigger=trigger;
    c->audio_state=HH_AUDIO_GREETING;
    emit(c,HH_ACT_GREETING_REQUEST,recipient,trigger,HH_OK,NULL); return HH_OK;
}
void hh_export(const HH_Core *c,HH_Snapshot *snapshot) {
    unsigned i; if(!c||!snapshot) return; *snapshot=c->store;
    for(i=0;i<HH_MAX_MESSAGES;i++) if(snapshot->used[i] && snapshot->messages[i].state!=HH_PLAYED) snapshot->messages[i].state=HH_PENDING;
}
HH_Result hh_restore(HH_Core *c,const HH_Snapshot *snapshot) {
    unsigned i,j;
    if(!c||!snapshot||snapshot->version!=HH_SNAPSHOT_VERSION||snapshot->seen_count>HH_SEEN_IDS||snapshot->seen_next>=HH_SEEN_IDS) return HH_INVALID;
    if(c->audio_state!=HH_AUDIO_IDLE) return HH_BUSY;
    for(i=0;i<HH_MAX_MESSAGES;i++) {
        const HH_Message *m=&snapshot->messages[i];
        if(snapshot->used[i]>1) return HH_INVALID;
        if(!snapshot->used[i]) continue;
        if(!valid_id(m->id)||!valid_path(m->audio_path)||m->recipient>1||!valid_message_trigger(m)||m->state>HH_PLAYED||m->source>HH_REMOTE) return HH_INVALID;
        for(j=0;j<i;j++) if(snapshot->used[j]&&strcmp(snapshot->messages[j].id,m->id)==0) return HH_INVALID;
    }
    for(i=0;i<snapshot->seen_count;i++) if(!valid_id(snapshot->seen_ids[i])) return HH_INVALID;
    c->store=*snapshot; c->queue_head=0; c->queue_count=0; c->playing_index=-1;
    for(i=0;i<HH_MAX_MESSAGES;i++) if(c->store.used[i]&&c->store.messages[i].state!=HH_PLAYED) c->store.messages[i].state=HH_PENDING;
    ui(c); return HH_OK;
}
void hh_chat_set_availability(HH_Core *c,HH_ChatAvailability availability) {
    if(!c||(unsigned)availability>(unsigned)HH_CHAT_READY||c->chat_availability==availability)return;
    c->chat_availability=availability;
    if(availability!=HH_CHAT_READY&&chat_owns_audio(c))request_chat_stop(c,HH_IO_ERROR);
    ui(c);
}
HH_Result hh_chat_report(HH_Core *c,uint32_t turn,HH_ChatEvent event,bool success,uint32_t now) {
    if(!c||(unsigned)event>(unsigned)HH_CHAT_QUIESCED)return HH_INVALID;
    if(!turn||turn!=c->chat_turn||!chat_owns_audio(c))return HH_NOT_FOUND;
    c->now_ms=now;
    if(c->audio_state==HH_AUDIO_CHAT_WAIT&&elapsed(now,c->chat_wait_since_ms,HH_CHAT_REPLY_WAIT_MS))
        request_chat_stop(c,HH_IO_ERROR);
    switch(event) {
    case HH_CHAT_STARTED:
        if(!success){request_chat_stop(c,HH_IO_ERROR);return HH_IO_ERROR;}
        if(c->audio_state!=HH_AUDIO_CHAT_CAPTURE)return HH_BUSY;
        c->chat_start_confirmed=true;ui(c);return HH_OK;
    case HH_CHAT_TTS_STARTED:
        if(!success){request_chat_stop(c,HH_IO_ERROR);return HH_IO_ERROR;}
        if(c->audio_state!=HH_AUDIO_CHAT_WAIT)return HH_BUSY;
        c->audio_state=HH_AUDIO_CHAT_PLAY;ui(c);return HH_OK;
    case HH_CHAT_FAILED:
        request_chat_stop(c,HH_IO_ERROR);return HH_IO_ERROR;
    case HH_CHAT_QUIESCED:
        if(!success) {
            if(c->audio_state!=HH_AUDIO_CHAT_STOPPING)request_chat_stop(c,HH_IO_ERROR);
            else emit(c,HH_ACT_FEEDBACK,0,HH_ARRIVE,HH_IO_ERROR,NULL);
            c->chat_stop_notice_sent=true;return HH_IO_ERROR;
        }
        if(c->chat_button.candidate)c->chat_key_requires_release=true;
        c->audio_state=HH_AUDIO_IDLE;
        c->chat_turn=0;c->chat_start_confirmed=false;c->chat_stop_notice_sent=false;
        c->chat_from_voice=c->chat_voice_speech=false;
        {
            int recipient=c->chat_local_recipient;c->chat_local_recipient=-1;
            if(recipient<0||!c->buttons[(unsigned)recipient].candidate) {
                recipient=-1;
                for(unsigned i=0;i<2;i++)if(c->buttons[i].candidate){recipient=(int)i;break;}
            }
            if(recipient>=0) {
                if(c->buttons[(unsigned)recipient].stable)start_recording(c,(unsigned)recipient);
                else c->chat_local_recipient=(int8_t)recipient; /* keep mail behind its debounce interval */
            }
        }
        ui(c);pump(c);return HH_OK;
    }
    return HH_INVALID;
}
