#include "homehub_core.h"
#include <stdio.h>
#include <string.h>

typedef struct { HH_Action items[1024]; size_t count; } Log;
typedef struct { HH_Core core; Log log; bool keys[2], buttons[2], pir, chat; uint32_t now; } Fixture;
static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL %s:%d: %s\n", __func__, __LINE__, #expr); failures++; return; } } while (0)
static void capture(void *ctx, const HH_Action *a) {
    Log *log = (Log *)ctx;
    if (log->count < sizeof(log->items)/sizeof(log->items[0])) log->items[log->count++] = *a;
}
static void sample(Fixture *f, uint32_t elapsed) {
    f->now += elapsed; hh_sample(&f->core, f->now, f->keys, f->buttons, f->pir);
}
static void init(Fixture *f, bool key0, bool key1, bool pir) {
    HH_Config config = hh_default_config(); memset(f, 0, sizeof(*f));
    config.boot_id = 123; config.pir_enabled = pir;
    f->keys[0] = key0; f->keys[1] = key1;
    hh_init(&f->core, &config, capture, &f->log, 0); sample(f, 0);
}
static size_t count(const Fixture *f, HH_ActionType type) {
    size_t n = 0, i; for (i=0;i<f->log.count;i++) if (f->log.items[i].type == type) n++; return n;
}
static const HH_Action *last(const Fixture *f, HH_ActionType type) {
    size_t i = f->log.count; while (i) { i--; if (f->log.items[i].type == type) return &f->log.items[i]; } return NULL;
}
static void settle(Fixture *f) { sample(f,0); sample(f,60); }
static HH_Result add(Fixture *f, const char *id, int recipient, HH_Trigger trigger) {
    HH_Message m; memset(&m,0,sizeof(m));
    snprintf(m.id,sizeof(m.id),"%s",id); snprintf(m.audio_path,sizeof(m.audio_path),"/sdcard/%s.wav",id);
    m.recipient=(uint8_t)recipient; m.trigger=(uint8_t)trigger;
    return hh_add_message(&f->core,&m);
}

static void initial_inputs_do_not_invent_edges(void) {
    Fixture f; init(&f,true,true,false); sample(&f,1000);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==0);
    CHECK(f.core.keys[0].initialized && f.core.keys[0].stable);
}
static void both_edges_wait_for_stability(void) {
    Fixture f; init(&f,false,false,false);
    f.keys[0]=true; sample(&f,0); f.keys[0]=false; sample(&f,10);
    f.keys[0]=true; sample(&f,10); sample(&f,59);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==0); sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==1); CHECK(last(&f,HH_ACT_KEY_EVENT)->trigger==HH_ARRIVE);
    f.keys[0]=false; settle(&f);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==2); CHECK(last(&f,HH_ACT_KEY_EVENT)->trigger==HH_LEAVE);
}
static void explicit_self_reminder_waits_until_evening(void) {
    Fixture f; char id[HH_ID_MAX+1]; init(&f,true,false,false);
    f.buttons[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_RECORD_START)==1);
    CHECK(last(&f,HH_ACT_RECORD_START)->trigger==HH_LEAVE);
    snprintf(id,sizeof(id),"%s",last(&f,HH_ACT_RECORD_START)->id);
    hh_recording_started(&f.core,true);
    CHECK(hh_set_record_trigger(&f.core,HH_ARRIVE)==HH_OK);
    sample(&f,300); f.buttons[0]=false; settle(&f);
    CHECK(count(&f,HH_ACT_RECORD_STOP_SAVE)==1);
    CHECK(last(&f,HH_ACT_RECORD_STOP_SAVE)->trigger==HH_ARRIVE);
    CHECK(hh_recording_saved(&f.core,id,"/sdcard/local.wav",true)==HH_OK);
    f.keys[0]=false; settle(&f); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    f.keys[0]=true; settle(&f); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,id)==0);
    CHECK(hh_playback_finished(&f.core,id,true)==HH_OK);
    CHECK(hh_find_message(&f.core,id)->state==HH_PLAYED);
}
static void short_press_cancels_and_long_hold_stops_at_30s(void) {
    Fixture f; init(&f,false,false,false); f.buttons[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_RECORD_START)==1);
    f.buttons[0]=false; settle(&f); CHECK(count(&f,HH_ACT_RECORD_CANCEL)==1);
    CHECK(count(&f,HH_ACT_RECORD_STOP_SAVE)==0);
    f.buttons[0]=true; settle(&f); sample(&f,30000);
    CHECK(count(&f,HH_ACT_RECORD_STOP_SAVE)==1);
    CHECK(f.core.audio_state==HH_AUDIO_SAVING);
    sample(&f,30000); CHECK(count(&f,HH_ACT_RECORD_STOP_SAVE)==1);
}
static void unfinished_recording_cannot_trigger_itself(void) {
    Fixture f; char id[HH_ID_MAX+1]; init(&f,false,false,false);
    f.buttons[0]=true; settle(&f); CHECK(last(&f,HH_ACT_RECORD_START)!=NULL);
    snprintf(id,sizeof(id),"%s",last(&f,HH_ACT_RECORD_START)->id);
    sample(&f,300); f.keys[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    f.buttons[0]=false; settle(&f);
    CHECK(hh_recording_saved(&f.core,id,"/sdcard/final.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    f.keys[0]=false; settle(&f); f.keys[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
}
static void simultaneous_slots_serialize_and_failure_stays_unread(void) {
    Fixture f; init(&f,false,false,false);
    CHECK(add(&f,"a",0,HH_ARRIVE)==HH_OK); CHECK(add(&f,"b",1,HH_ARRIVE)==HH_OK);
    f.keys[0]=true; f.keys[1]=true; settle(&f);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1); CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"a")==0);
    CHECK(hh_playback_finished(&f.core,"wrong",true)==HH_NOT_FOUND);
    CHECK(hh_playback_finished(&f.core,"a",false)==HH_OK);
    CHECK(hh_find_message(&f.core,"a")->state==HH_PENDING);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==2); CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"b")==0);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==0);
    CHECK(hh_playback_finished(&f.core,"b",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==1);
    sample(&f,1000); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==2);
    f.keys[0]=false; settle(&f); f.keys[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==3);
}
static void events_during_recording_queue_without_audio_overlap(void) {
    Fixture f; char id[HH_ID_MAX+1]; init(&f,false,false,false);
    CHECK(add(&f,"remote",1,HH_ARRIVE)==HH_OK);
    f.buttons[0]=true; settle(&f); CHECK(last(&f,HH_ACT_RECORD_START)!=NULL);
    snprintf(id,sizeof(id),"%s",last(&f,HH_ACT_RECORD_START)->id);
    f.keys[1]=true; settle(&f); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    sample(&f,300); f.buttons[0]=false; settle(&f); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    CHECK(hh_recording_saved(&f.core,id,"/sdcard/saved.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1); CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"remote")==0);
}
static void snapshot_restore_rearms_only_new_edges(void) {
    Fixture f,g; HH_Snapshot snapshot; init(&f,false,false,false);
    CHECK(add(&f,"persisted",0,HH_ARRIVE)==HH_OK);
    f.keys[0]=true; settle(&f); CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
    hh_export(&f.core,&snapshot); init(&g,true,false,false);
    CHECK(hh_restore(&g.core,&snapshot)==HH_OK); sample(&g,1000);
    CHECK(count(&g,HH_ACT_PLAY_AUDIO)==0); CHECK(hh_pending_count(&g.core,0)==1);
    CHECK(add(&g,"persisted",0,HH_ARRIVE)==HH_DUPLICATE);
    g.keys[0]=false; settle(&g); g.keys[0]=true; settle(&g);
    CHECK(count(&g,HH_ACT_PLAY_AUDIO)==1);
    snapshot.version=999; CHECK(hh_restore(&g.core,&snapshot)==HH_INVALID);
    CHECK(hh_find_message(&g.core,"persisted")!=NULL);
}
static void bounded_queue_and_id_validation(void) {
    Fixture f; int i; char id[20]; init(&f,false,false,false);
    CHECK(add(&f,"../bad",0,HH_ARRIVE)==HH_INVALID);
    CHECK(add(&f,"bad",2,HH_ARRIVE)==HH_INVALID);
    for(i=0;i<HH_MAX_MESSAGES;i++){snprintf(id,sizeof(id),"m%d",i); CHECK(add(&f,id,i%2,HH_ARRIVE)==HH_OK);}
    CHECK(add(&f,"overflow",0,HH_ARRIVE)==HH_FULL); CHECK(add(&f,"m0",0,HH_ARRIVE)==HH_DUPLICATE);
    CHECK(hh_pending_count(&f.core,0)+hh_pending_count(&f.core,1)==HH_MAX_MESSAGES);
    f.keys[0]=true; settle(&f); CHECK(hh_playback_finished(&f.core,"m0",true)==HH_OK);
    CHECK(add(&f,"new",0,HH_LEAVE)==HH_OK); CHECK(add(&f,"m0",0,HH_ARRIVE)==HH_DUPLICATE);
}
static void pir_warmup_throttle_and_missing_assets_are_explicit(void) {
    Fixture f; init(&f,true,true,true); f.pir=true;
    sample(&f,9999); CHECK(count(&f,HH_ACT_WAKE_SCREEN)==0);
    sample(&f,1); CHECK(count(&f,HH_ACT_WAKE_SCREEN)==1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(count(&f,HH_ACT_GREETING_REQUEST)==0);
    CHECK(hh_greeting_ready(&f.core,NULL,false)==HH_MISSING_ASSET);
    CHECK(last(&f,HH_ACT_FEEDBACK)->result==HH_MISSING_ASSET);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
    f.pir=false; sample(&f,1); f.pir=true; sample(&f,29998);
    CHECK(count(&f,HH_ACT_WAKE_SCREEN)==2);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    f.pir=false; sample(&f,0); f.pir=true; sample(&f,1);
    CHECK(count(&f,HH_ACT_WAKE_SCREEN)==3);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);
}
static void pir_reminds_when_either_single_key_is_still_present(void) {
    Fixture f;
    for(unsigned recipient=0;recipient<2;recipient++){
        init(&f,recipient==0,recipient==1,true);sample(&f,60000);f.pir=true;sample(&f,1);
        CHECK(count(&f,HH_ACT_WAKE_SCREEN)==1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
        CHECK(count(&f,HH_ACT_GREETING_REQUEST)==0);CHECK(count(&f,HH_ACT_KEY_EVENT)==0);
    }
}
static void recorder_failure_never_saves_or_marks_played(void) {
    Fixture f; init(&f,false,false,false); f.buttons[0]=true; settle(&f);
    CHECK(count(&f,HH_ACT_RECORD_START)==1); hh_recording_started(&f.core,false);
    CHECK(f.core.audio_state==HH_AUDIO_IDLE); CHECK(last(&f,HH_ACT_FEEDBACK)->result==HH_IO_ERROR);
    sample(&f,1000); f.buttons[0]=false; settle(&f);
    CHECK(count(&f,HH_ACT_RECORD_STOP_SAVE)==0); CHECK(hh_pending_count(&f.core,0)==0);
}
static void elapsed_time_handles_uint32_wrap(void) {
    Fixture f; init(&f,false,false,false); f.now=UINT32_MAX-20u;
    f.keys[0]=true; sample(&f,0); sample(&f,60);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==1);
}
static void invalid_trigger_override_is_rejected(void) {
    Fixture f; init(&f,false,false,false); f.buttons[0]=true; settle(&f);
    CHECK(hh_set_record_trigger(&f.core,(HH_Trigger)-1)==HH_INVALID);
    CHECK(f.core.record_trigger==HH_ARRIVE);
}
static void repeated_remote_played_message_retries_receipt(void) {
    Fixture f; HH_Message remote; init(&f,false,false,false);
    memset(&remote,0,sizeof(remote)); snprintf(remote.id,sizeof(remote.id),"remote_receipt");
    snprintf(remote.audio_path,sizeof(remote.audio_path),"/sdcard/remote.wav"); remote.source=HH_REMOTE;
    CHECK(hh_add_message(&f.core,&remote)==HH_OK);
    f.keys[0]=true; settle(&f); CHECK(hh_playback_finished(&f.core,remote.id,true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==1);
    CHECK(hh_add_message(&f.core,&remote)==HH_DUPLICATE);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==2);
    CHECK(last(&f,HH_ACT_PLAYED_RECEIPT)->source==HH_REMOTE);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
}
static void recording_reserves_capacity_and_save_failure_stays_unread(void) {
    Fixture f; int i; char id[20],record_id[HH_ID_MAX+1]; init(&f,false,false,false);
    for(i=0;i<HH_MAX_MESSAGES-1;i++){snprintf(id,sizeof(id),"before%d",i); CHECK(add(&f,id,i%2,HH_LEAVE)==HH_OK);}
    f.buttons[0]=true; settle(&f); CHECK(last(&f,HH_ACT_RECORD_START)!=NULL);
    snprintf(record_id,sizeof(record_id),"%s",last(&f,HH_ACT_RECORD_START)->id);
    CHECK(add(&f,"steal_reserved_slot",0,HH_ARRIVE)==HH_FULL);
    sample(&f,300); f.buttons[0]=false; settle(&f);
    CHECK(hh_recording_saved(&f.core,record_id,NULL,false)==HH_IO_ERROR);
    CHECK(hh_find_message(&f.core,record_id)==NULL);
    CHECK(hh_pending_count(&f.core,0)+hh_pending_count(&f.core,1)==HH_MAX_MESSAGES-1);
    CHECK(add(&f,"slot_freed",0,HH_ARRIVE)==HH_OK);
}
static void one_shot_trigger_can_be_chosen_before_holding_button(void) {
    Fixture f; init(&f,true,false,false);
    CHECK(hh_set_next_trigger(&f.core,0,HH_ARRIVE)==HH_OK);
    f.buttons[0]=true; settle(&f);
    CHECK(last(&f,HH_ACT_RECORD_START)->trigger==HH_ARRIVE);
    f.buttons[0]=false; settle(&f); /* short recording cancelled, selection consumed */
    f.buttons[0]=true; settle(&f);
    CHECK(last(&f,HH_ACT_RECORD_START)->trigger==HH_LEAVE);
    f.buttons[0]=false; settle(&f);
    CHECK(hh_set_next_trigger(&f.core,0,HH_ARRIVE)==HH_OK);
    CHECK(hh_set_next_trigger(&f.core,0,HH_TRIGGER_AUTO)==HH_OK);
    f.buttons[0]=true; settle(&f);
    CHECK(last(&f,HH_ACT_RECORD_START)->trigger==HH_LEAVE);
    CHECK(hh_set_next_trigger(&f.core,2,HH_ARRIVE)==HH_INVALID);
    CHECK(hh_set_next_trigger(&f.core,0,42)==HH_INVALID);
}
static void key_greeting_preserves_context_and_yields_to_mail(void) {
    Fixture f; size_t requests; init(&f,false,false,false);
    CHECK(hh_request_greeting(&f.core,1,HH_LEAVE)==HH_OK);
    CHECK(last(&f,HH_ACT_GREETING_REQUEST)->recipient==1);
    CHECK(last(&f,HH_ACT_GREETING_REQUEST)->trigger==HH_LEAVE);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(last(&f,HH_ACT_PLAY_AUDIO)->recipient==1);
    CHECK(last(&f,HH_ACT_PLAY_AUDIO)->trigger==HH_LEAVE);
    CHECK(add(&f,"urgent_mail",0,HH_ARRIVE)==HH_OK);
    f.keys[0]=true; settle(&f); requests=count(&f,HH_ACT_GREETING_REQUEST);
    CHECK(hh_request_greeting(&f.core,0,HH_ARRIVE)==HH_BUSY);
    CHECK(count(&f,HH_ACT_GREETING_REQUEST)==requests);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"urgent_mail")==0);
    CHECK(hh_request_greeting(&f.core,0,HH_ARRIVE)==HH_BUSY);
    CHECK(hh_playback_finished(&f.core,"urgent_mail",true)==HH_OK);
    CHECK(hh_request_greeting(&f.core,0,HH_ARRIVE)==HH_OK);
    CHECK(hh_greeting_ready(&f.core,NULL,false)==HH_MISSING_ASSET);
    CHECK(hh_request_greeting(&f.core,2,HH_ARRIVE)==HH_INVALID);
}
static void pir_with_no_keys_only_wakes_screen(void) {
    Fixture f;init(&f,false,false,true);sample(&f,60000);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_WAKE_SCREEN)==1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(count(&f,HH_ACT_GREETING_REQUEST)==0);CHECK(f.core.audio_state==HH_AUDIO_IDLE);
}
static void disabled_pir_does_not_wake_or_remind(void) {
    Fixture f;init(&f,true,true,false);sample(&f,60000);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_WAKE_SCREEN)==0&&count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
}
static void continuous_pir_rearms_at_quiet_boundary_without_wake_spam(void) {
    Fixture f;init(&f,true,false,true);sample(&f,f.core.config.pir_warmup_ms);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,f.core.config.pir_quiet_ms-1u);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2&&count(&f,HH_ACT_WAKE_SCREEN)==1);
    for(unsigned i=0;i<100;i++)sample(&f,10);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2&&count(&f,HH_ACT_WAKE_SCREEN)==1);
}
static void pir_high_edge_inside_quiet_is_not_lost_at_rearm(void) {
    Fixture f;init(&f,true,false,true);sample(&f,10000);f.pir=true;sample(&f,1);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);
    f.pir=false;sample(&f,1000);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);sample(&f,28998);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2&&count(&f,HH_ACT_WAKE_SCREEN)==2);
}
static void high_pir_with_no_keys_waits_for_a_key_without_fake_edges(void) {
    Fixture f;init(&f,false,false,true);sample(&f,10000);f.pir=true;sample(&f,1);
    sample(&f,60000);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(count(&f,HH_ACT_WAKE_SCREEN)==1);
    f.keys[0]=true;settle(&f);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1&&count(&f,HH_ACT_WAKE_SCREEN)==1);
}
static void expired_high_pir_wait_stays_cancelled_until_a_real_low(void) {
    Fixture f;init(&f,true,false,true);sample(&f,10000);
    CHECK(hh_request_greeting(&f.core,0,HH_LEAVE)==HH_OK);
    f.pir=true;sample(&f,1);sample(&f,10000);
    CHECK(!f.core.key_reminder_pending);
    sample(&f,30000);CHECK(!f.core.key_reminder_pending);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    f.pir=false;sample(&f,1);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
}
static void pir_never_consumes_or_acknowledges_personal_mail(void) {
    Fixture f;init(&f,true,true,true);
    CHECK(add(&f,"me_leave",0,HH_LEAVE)==HH_OK);CHECK(add(&f,"me_arrive",0,HH_ARRIVE)==HH_OK);
    CHECK(add(&f,"ta_leave",1,HH_LEAVE)==HH_OK);CHECK(add(&f,"ta_arrive",1,HH_ARRIVE)==HH_OK);
    sample(&f,60000);f.pir=true;sample(&f,1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(count(&f,HH_ACT_KEY_EVENT)==0&&count(&f,HH_ACT_GREETING_REQUEST)==0);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"greeting")==0);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);
    CHECK(hh_pending_count(&f.core,0)==2&&hh_pending_count(&f.core,1)==2);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==0);
    CHECK(hh_find_message(&f.core,"me_leave")->state==HH_PENDING&&hh_find_message(&f.core,"ta_arrive")->state==HH_PENDING);
}
static void busy_pir_edges_merge_then_dispatch_once_when_audio_is_idle(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);CHECK(hh_request_greeting(&f.core,1,HH_LEAVE)==HH_OK);
    f.pir=true;sample(&f,1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    f.pir=false;sample(&f,1000);f.pir=true;sample(&f,1);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);sample(&f,1000);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);sample(&f,0);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
}
static void busy_pir_merge_does_not_extend_the_ten_second_deadline(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);CHECK(hh_request_greeting(&f.core,0,HH_LEAVE)==HH_OK);
    f.pir=true;sample(&f,1);f.pir=false;sample(&f,9000);f.pir=true;sample(&f,1);sample(&f,999);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    f.pir=false;sample(&f,1);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1); /* expired wait did not consume speech quiet time */
}
static void all_keys_taken_cancels_a_busy_pending_reminder(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);CHECK(hh_request_greeting(&f.core,0,HH_LEAVE)==HH_OK);
    f.pir=true;sample(&f,1);f.keys[0]=false;settle(&f);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    f.keys[0]=true;settle(&f);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
}
static void prepared_reminder_rechecks_keys_before_playback(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);f.keys[0]=false;settle(&f);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0&&f.core.audio_state==HH_AUDIO_IDLE);
    CHECK(hh_request_greeting(&f.core,1,HH_LEAVE)==HH_OK);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1); /* ordinary greeting has no key-present requirement */
}
static void recording_save_and_queued_mail_run_before_pir_reminder(void) {
    Fixture f;char id[HH_ID_MAX+1];init(&f,true,false,true);CHECK(add(&f,"priority_mail",1,HH_ARRIVE)==HH_OK);
    sample(&f,60000);f.buttons[0]=true;settle(&f);CHECK(last(&f,HH_ACT_RECORD_START)!=NULL);
    snprintf(id,sizeof(id),"%s",last(&f,HH_ACT_RECORD_START)->id);
    f.pir=true;sample(&f,1);f.keys[1]=true;settle(&f);sample(&f,300);
    f.buttons[0]=false;settle(&f);CHECK(f.core.audio_state==HH_AUDIO_SAVING);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(hh_recording_saved(&f.core,id,"/sdcard/priority_record.wav",true)==HH_OK);sample(&f,0);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"priority_mail")==0&&count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(hh_playback_finished(&f.core,"priority_mail",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
}
static void busy_reminder_deadline_handles_timer_wrap(void) {
    Fixture f;init(&f,true,false,true);f.now=UINT32_MAX-5000u;
    CHECK(hh_request_greeting(&f.core,0,HH_LEAVE)==HH_OK);f.pir=true;sample(&f,0);sample(&f,10000);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
}
static void queued_mail_preempts_a_reminder_still_preparing(void) {
    Fixture f;init(&f,true,true,true);CHECK(add(&f,"leave_before_reminder",0,HH_LEAVE)==HH_OK);
    sample(&f,60000);f.pir=true;sample(&f,1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    f.keys[0]=false;settle(&f);CHECK(hh_find_message(&f.core,"leave_before_reminder")->state==HH_QUEUED);
    CHECK(f.core.audio_state==HH_AUDIO_GREETING);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"leave_before_reminder")==0);
    CHECK(hh_playback_finished(&f.core,"leave_before_reminder",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1&&f.core.audio_state==HH_AUDIO_IDLE);
}
static void taking_then_returning_all_keys_cannot_revive_old_preparation(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);f.pir=true;sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    f.keys[0]=false;settle(&f);f.keys[0]=true;settle(&f);
    CHECK(f.core.audio_state==HH_AUDIO_GREETING); /* retain reservation until its own ready arrives */
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0&&f.core.audio_state==HH_AUDIO_IDLE);
    f.keys[0]=false;settle(&f);CHECK(hh_request_greeting(&f.core,1,HH_LEAVE)==HH_OK);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1); /* ordinary request clears dedicated cancellation context */
}
static void prepared_reminder_expires_without_releasing_its_audio_reservation_early(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);f.pir=true;sample(&f,1);sample(&f,10000);
    CHECK(f.core.audio_state==HH_AUDIO_GREETING);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0&&f.core.audio_state==HH_AUDIO_IDLE);
}
static void preparation_keeps_the_original_busy_pending_deadline(void) {
    Fixture f;init(&f,true,false,true);sample(&f,60000);CHECK(hh_request_greeting(&f.core,0,HH_LEAVE)==HH_OK);
    f.pir=true;sample(&f,1);sample(&f,9000);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/leave.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);sample(&f,1000);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1&&f.core.audio_state==HH_AUDIO_IDLE);
}
static void fresh_activity_cannot_extend_an_old_preparation_deadline(void) {
    Fixture f;init(&f,true,false,true);sample(&f,f.core.config.pir_warmup_ms);f.pir=true;sample(&f,1);sample(&f,f.core.config.pir_quiet_ms-1u);
    f.pir=false;sample(&f,1);f.pir=true;sample(&f,1);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
}
static void raw_record_button_press_wins_over_same_frame_pir(void) {
    Fixture f;
    for(unsigned recipient=0;recipient<2;recipient++){
        init(&f,true,false,true);sample(&f,60000);
        f.buttons[recipient]=true;f.pir=true;sample(&f,0);
        CHECK(count(&f,HH_ACT_RECORD_START)==0); /* button is not debounced yet */
        CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
        sample(&f,60);
        CHECK(count(&f,HH_ACT_RECORD_START)==1);
        CHECK(last(&f,HH_ACT_RECORD_START)->recipient==recipient);
        CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0&&f.core.audio_state==HH_AUDIO_RECORDING);
    }
}
static void chat_sample(Fixture *f,uint32_t elapsed_ms) {
    f->now+=elapsed_ms;hh_sample_with_chat(&f->core,f->now,f->keys,f->buttons,f->pir,f->chat);
}
static void chat_init(Fixture *f,bool held,HH_ChatAvailability availability) {
    HH_Config config=hh_default_config();memset(f,0,sizeof(*f));config.boot_id=123;f->keys[0]=true;f->chat=held;
    hh_init(&f->core,&config,capture,&f->log,0);hh_chat_set_availability(&f->core,availability);chat_sample(f,0);
}
static void chat_press(Fixture *f){f->chat=true;chat_sample(f,0);chat_sample(f,60);}
static void chat_release(Fixture *f,uint32_t held_ms){chat_sample(f,held_ms);f->chat=false;chat_sample(f,0);chat_sample(f,60);}
static HH_Result chat_report(Fixture *f,uint32_t turn,HH_ChatEvent event,bool success){return hh_chat_report(&f->core,turn,event,success,f->now);}

static void unavailable_chat_never_starts_capture(void) {
    Fixture f;const HH_ChatAvailability states[]={HH_CHAT_DISABLED,HH_CHAT_UNAUTHORIZED,HH_CHAT_OFFLINE,HH_CHAT_CONNECTING};
    for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);i++){
        chat_init(&f,false,states[i]);chat_press(&f);chat_release(&f,300);
        CHECK(count(&f,HH_ACT_CHAT_START)==0&&count(&f,HH_ACT_CHAT_SUBMIT)==0&&count(&f,HH_ACT_CHAT_STOP)==0);
        CHECK(f.core.audio_state==HH_AUDIO_IDLE);
    }
}
static void chat_readiness_does_not_turn_an_existing_hold_into_capture(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_OFFLINE);chat_press(&f);hh_chat_set_availability(&f.core,HH_CHAT_READY);chat_sample(&f,1000);
    CHECK(count(&f,HH_ACT_CHAT_START)==0);chat_release(&f,0);chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==1);
}
static void chat_boot_held_requires_release_before_start(void) {
    Fixture f;chat_init(&f,true,HH_CHAT_READY);chat_sample(&f,1000);CHECK(count(&f,HH_ACT_CHAT_START)==0);
    chat_release(&f,0);chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==1);
}
static void chat_debounce_hold_and_release_submit_exactly_one_turn(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);f.chat=true;chat_sample(&f,0);f.chat=false;chat_sample(&f,10);f.chat=true;chat_sample(&f,10);chat_sample(&f,59);
    CHECK(count(&f,HH_ACT_CHAT_START)==0);chat_sample(&f,1);CHECK(count(&f,HH_ACT_CHAT_START)==1);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;CHECK(turn!=0&&f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    CHECK(chat_report(&f,turn,HH_CHAT_STARTED,true)==HH_OK);chat_release(&f,300);
    CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==1&&last(&f,HH_ACT_CHAT_SUBMIT)->chat_turn==turn);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_WAIT);chat_sample(&f,500);CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==1);
}
static void short_chat_hold_requests_stop_instead_of_submit(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,0);
    CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==0&&count(&f,HH_ACT_CHAT_STOP)==1);
    CHECK(last(&f,HH_ACT_CHAT_STOP)->chat_turn==turn&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);CHECK(f.core.audio_state==HH_AUDIO_IDLE);
}
static void chat_capture_limit_submits_once_and_release_does_not_cancel(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;CHECK(chat_report(&f,turn,HH_CHAT_STARTED,true)==HH_OK);
    chat_sample(&f,30000);CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==1&&f.core.audio_state==HH_AUDIO_CHAT_WAIT);
    chat_sample(&f,1000);chat_release(&f,0);CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==1&&count(&f,HH_ACT_CHAT_STOP)==0);
}
static void unanswered_chat_times_out_after_45_seconds_without_unlocking_audio(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);chat_release(&f,300);
    chat_sample(&f,44999);CHECK(count(&f,HH_ACT_CHAT_STOP)==0);chat_sample(&f,1);
    CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    chat_sample(&f,HH_CHAT_STOP_NOTICE_MS);CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    CHECK(last(&f,HH_ACT_FEEDBACK)!=NULL&&last(&f,HH_ACT_FEEDBACK)->result==HH_IO_ERROR);
    size_t feedback_count=count(&f,HH_ACT_FEEDBACK);chat_sample(&f,10000);CHECK(count(&f,HH_ACT_FEEDBACK)==feedback_count);
}
static void pressing_key_again_cancels_waiting_or_playing_chat(void) {
    Fixture f;
    for(unsigned playing=0;playing<2;playing++){
        chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
        uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,300);
        if(playing)CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_OK);
        chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
        CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
        CHECK(f.core.audio_state==HH_AUDIO_IDLE);chat_release(&f,0);CHECK(count(&f,HH_ACT_CHAT_START)==1);
    }
}
static void losing_availability_stops_each_active_chat_phase(void) {
    Fixture f;
    for(unsigned phase=0;phase<3;phase++){
        chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
        uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
        if(phase)chat_release(&f,300);if(phase==2)CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_OK);
        hh_chat_set_availability(&f.core,HH_CHAT_OFFLINE);
        CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
        hh_chat_set_availability(&f.core,HH_CHAT_CONNECTING);CHECK(count(&f,HH_ACT_CHAT_STOP)==1);
    }
}
static void raw_local_recording_wins_over_same_frame_chat_key(void) {
    Fixture f;
    for(unsigned recipient=0;recipient<2;recipient++){
        chat_init(&f,false,HH_CHAT_READY);f.buttons[recipient]=true;f.chat=true;chat_sample(&f,0);chat_sample(&f,60);
        CHECK(count(&f,HH_ACT_RECORD_START)==1&&last(&f,HH_ACT_RECORD_START)->recipient==recipient);
        CHECK(count(&f,HH_ACT_CHAT_START)==0&&f.core.audio_state==HH_AUDIO_RECORDING);
    }
}
static void raw_chat_key_blocks_pir_while_waiting_for_debounce(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);f.core.config.pir_enabled=true;chat_sample(&f,60000);
    f.chat=true;f.pir=true;chat_sample(&f,0);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    chat_sample(&f,60);CHECK(count(&f,HH_ACT_CHAT_START)==1&&count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
}
static void local_recording_takeover_waits_for_current_chat_quiescence(void) {
    Fixture f;
    for(unsigned recipient=0;recipient<2;recipient++){
        chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
        f.buttons[recipient]=true;chat_sample(&f,0);chat_sample(&f,60);
        CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&count(&f,HH_ACT_RECORD_START)==0);
        CHECK(chat_report(&f,turn+1,HH_CHAT_QUIESCED,true)==HH_NOT_FOUND);CHECK(count(&f,HH_ACT_RECORD_START)==0);
        CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
        CHECK(count(&f,HH_ACT_RECORD_START)==1&&last(&f,HH_ACT_RECORD_START)->recipient==recipient);
        CHECK(f.core.audio_state==HH_AUDIO_RECORDING);
    }
}
static void released_local_button_is_not_recorded_after_chat_stops(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    f.buttons[0]=true;chat_sample(&f,0);chat_sample(&f,60);f.buttons[0]=false;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(count(&f,HH_ACT_RECORD_START)==0&&f.core.audio_state==HH_AUDIO_IDLE);
}
static void mailbox_event_stops_chat_then_plays_after_quiesced(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(add(&f,"chat_priority_mail",0,HH_LEAVE)==HH_OK);chat_press(&f);
    CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    f.keys[0]=false;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(hh_find_message(&f.core,"chat_priority_mail")->state==HH_QUEUED);
    CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&count(&f,HH_ACT_PLAY_AUDIO)==0);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1&&strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"chat_priority_mail")==0);
}
static void old_turn_reports_cannot_change_a_new_chat(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t first=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,0);CHECK(chat_report(&f,first,HH_CHAT_QUIESCED,true)==HH_OK);
    chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==2);uint32_t second=last(&f,HH_ACT_CHAT_START)->chat_turn;CHECK(second&&second!=first);
    CHECK(chat_report(&f,first,HH_CHAT_QUIESCED,true)==HH_NOT_FOUND);CHECK(chat_report(&f,first,HH_CHAT_FAILED,false)==HH_NOT_FOUND);
    CHECK(chat_report(&f,first,HH_CHAT_TTS_STARTED,true)==HH_NOT_FOUND);CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    CHECK(count(&f,HH_ACT_CHAT_STOP)==1);
}
static void chat_reports_respect_capture_wait_and_play_states(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_BUSY);CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    CHECK(chat_report(&f,turn,HH_CHAT_STARTED,true)==HH_OK);chat_release(&f,300);
    CHECK(chat_report(&f,turn,HH_CHAT_STARTED,true)==HH_BUSY);CHECK(f.core.audio_state==HH_AUDIO_CHAT_WAIT);
    CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_OK);CHECK(f.core.audio_state==HH_AUDIO_CHAT_PLAY);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);CHECK(f.core.audio_state==HH_AUDIO_IDLE);
    CHECK(count(&f,HH_ACT_PLAYED_RECEIPT)==0);
}
static void chat_start_failure_requests_cleanup_without_unlocking(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    CHECK(chat_report(&f,turn,HH_CHAT_STARTED,false)==HH_IO_ERROR);
    CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
}
static void failed_quiescence_keeps_chat_owned_until_a_successful_confirmation(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    chat_release(&f,0);CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,false)==HH_IO_ERROR);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);chat_sample(&f,10000);CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);CHECK(f.core.audio_state==HH_AUDIO_IDLE);
}
static void queued_mail_wins_over_same_frame_new_chat(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(add(&f,"same_frame_mail",0,HH_LEAVE)==HH_OK);
    f.keys[0]=false;f.chat=true;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(count(&f,HH_ACT_CHAT_START)==0&&count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(strcmp(last(&f,HH_ACT_PLAY_AUDIO)->id,"same_frame_mail")==0);
}
static void chat_wait_timeout_handles_uint32_wrap(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);f.now=UINT32_MAX-1000u;chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    chat_release(&f,300);chat_sample(&f,45000);CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
}
static void pir_waits_for_chat_to_quiesce_before_reminding(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);f.core.config.pir_enabled=true;chat_sample(&f,60000);chat_press(&f);
    CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;f.pir=true;chat_sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);chat_release(&f,300);CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_OK);
    chat_sample(&f,1000);CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);chat_sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
}
static void unchanged_chat_availability_does_not_flood_ui_actions(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);size_t before=count(&f,HH_ACT_UI);
    for(unsigned i=0;i<20;i++)hh_chat_set_availability(&f.core,HH_CHAT_READY);
    CHECK(count(&f,HH_ACT_UI)==before);
    hh_chat_set_availability(&f.core,HH_CHAT_OFFLINE);CHECK(count(&f,HH_ACT_UI)==before+1);
}
static void tts_report_after_wait_deadline_cannot_revive_the_turn(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,300);f.now+=45000;
    CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_BUSY);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&count(&f,HH_ACT_CHAT_STOP)==1);
}
static void raw_local_takeover_at_quiescence_keeps_mail_waiting_for_debounce(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(add(&f,"wait_for_local_press",0,HH_LEAVE)==HH_OK);
    chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;
    f.keys[0]=false;chat_sample(&f,0);chat_sample(&f,60);
    f.buttons[1]=true;chat_sample(&f,0);CHECK(count(&f,HH_ACT_RECORD_START)==0);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0&&count(&f,HH_ACT_RECORD_START)==0);
    chat_sample(&f,60);CHECK(count(&f,HH_ACT_RECORD_START)==1&&last(&f,HH_ACT_RECORD_START)->recipient==1);
    CHECK(hh_find_message(&f.core,"wait_for_local_press")->state==HH_QUEUED);
}
static void old_chat_raw_key_candidate_cannot_restart_after_quiescence(void) {
    Fixture f;
    for(unsigned phase=0;phase<3;phase++){
        chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
        uint32_t old_turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,300);
        if(phase==1)CHECK(chat_report(&f,old_turn,HH_CHAT_TTS_STARTED,true)==HH_OK);
        if(phase==2)CHECK(chat_report(&f,old_turn,HH_CHAT_FAILED,false)==HH_IO_ERROR);
        f.chat=true;chat_sample(&f,0); /* candidate began during WAIT/PLAY/STOPPING */
        CHECK(count(&f,HH_ACT_CHAT_START)==1);
        CHECK(chat_report(&f,old_turn,HH_CHAT_QUIESCED,true)==HH_OK);chat_sample(&f,60);
        CHECK(f.core.audio_state==HH_AUDIO_IDLE&&count(&f,HH_ACT_CHAT_START)==1);
        chat_release(&f,0);chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==2);
        CHECK(last(&f,HH_ACT_CHAT_START)->chat_turn!=old_turn);
    }
}
static void old_chat_candidate_requires_a_stable_release_before_new_capture(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,300);f.chat=true;chat_sample(&f,0);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    f.chat=false;chat_sample(&f,10);f.chat=true;chat_sample(&f,10);chat_sample(&f,60);
    CHECK(f.core.audio_state==HH_AUDIO_IDLE&&count(&f,HH_ACT_CHAT_START)==1);
    chat_release(&f,0);chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==2);
}
static void partial_release_before_quiescence_does_not_rearm_old_candidate(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);chat_press(&f);CHECK(last(&f,HH_ACT_CHAT_START)!=NULL);
    uint32_t turn=last(&f,HH_ACT_CHAT_START)->chat_turn;chat_release(&f,300);
    f.chat=true;chat_sample(&f,0);f.chat=false;chat_sample(&f,10);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    f.chat=true;chat_sample(&f,10);chat_sample(&f,60);
    CHECK(f.core.audio_state==HH_AUDIO_IDLE&&count(&f,HH_ACT_CHAT_START)==1);
    chat_release(&f,0);chat_press(&f);CHECK(count(&f,HH_ACT_CHAT_START)==2);
}

static void voice_wake_starts_one_turn_without_key_edges(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);
    CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
    uint32_t turn=f.core.chat_turn;CHECK(turn!=0&&f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    CHECK(f.core.chat_from_voice&&!f.core.chat_voice_speech);
    CHECK(count(&f,HH_ACT_CHAT_START)==1&&last(&f,HH_ACT_CHAT_START)->chat_turn==turn);
    CHECK(!f.core.chat_button.stable&&!f.core.chat_button.candidate);
    size_t actions=f.log.count;CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
    CHECK(f.log.count==actions&&count(&f,HH_ACT_KEY_EVENT)==0);
    CHECK(hh_chat_voice_wake(NULL,0)==HH_INVALID);
}
static void unavailable_voice_wakes_are_quiet(void) {
    Fixture f;
    for(unsigned availability=HH_CHAT_DISABLED;availability<HH_CHAT_READY;availability++){
        chat_init(&f,false,(HH_ChatAvailability)availability);size_t actions=f.log.count;
        for(unsigned i=0;i<10;i++)CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
        CHECK(f.log.count==actions&&f.core.audio_state==HH_AUDIO_IDLE);
    }
}
static void voice_wake_yields_to_raw_record_buttons_and_key(void) {
    Fixture f;
    for(unsigned button=0;button<3;button++){
        chat_init(&f,false,HH_CHAT_READY);
        if(button<2)f.buttons[button]=true;else f.chat=true;
        chat_sample(&f,0);CHECK(f.core.audio_state==HH_AUDIO_IDLE);
        size_t actions=f.log.count;CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
        CHECK(f.log.count==actions&&count(&f,HH_ACT_CHAT_START)==0);
        chat_sample(&f,60);
        CHECK(f.core.audio_state==(button<2?HH_AUDIO_RECORDING:HH_AUDIO_CHAT_CAPTURE));
    }
    chat_init(&f,true,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
}
static void voice_wake_preserves_local_record_save_mail_and_pir_priority(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);f.buttons[0]=true;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
    chat_sample(&f,300);f.buttons[0]=false;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(f.core.audio_state==HH_AUDIO_SAVING&&hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
    chat_init(&f,false,HH_CHAT_READY);CHECK(add(&f,"voice_mail",0,HH_LEAVE)==HH_OK);
    f.keys[0]=false;chat_sample(&f,0);chat_sample(&f,60);
    CHECK(f.core.audio_state==HH_AUDIO_PLAYING&&hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
    CHECK(count(&f,HH_ACT_CHAT_START)==0);
    chat_init(&f,false,HH_CHAT_READY);f.core.config.pir_enabled=true;chat_sample(&f,60000);
    f.pir=true;chat_sample(&f,0);
    CHECK(f.core.audio_state==HH_AUDIO_GREETING&&hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1&&count(&f,HH_ACT_CHAT_START)==0);
}
static void voice_without_speech_cancels_at_eight_seconds_without_upload(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
    uint32_t turn=f.core.chat_turn;size_t feedback=count(&f,HH_ACT_FEEDBACK);
    CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);
    chat_sample(&f,7999);CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
    CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);
    chat_sample(&f,1);CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    CHECK(count(&f,HH_ACT_CHAT_STOP)==1&&last(&f,HH_ACT_CHAT_STOP)->result==HH_OK);
    CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==0&&count(&f,HH_ACT_FEEDBACK)==feedback);
    chat_sample(&f,1000);CHECK(count(&f,HH_ACT_CHAT_STOP)==1);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
    chat_sample(&f,1000);CHECK(f.core.audio_state==HH_AUDIO_IDLE&&count(&f,HH_ACT_CHAT_START)==1);
}
static void voice_speech_then_silence_submits_once_and_finishes_one_reply(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
    uint32_t turn=f.core.chat_turn;CHECK(hh_chat_voice_activity(&f.core,turn,true,f.now)==HH_OK);
    CHECK(f.core.chat_from_voice&&f.core.chat_voice_speech);
    chat_sample(&f,1000);CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_WAIT&&count(&f,HH_ACT_CHAT_SUBMIT)==1);
    CHECK(last(&f,HH_ACT_CHAT_SUBMIT)->chat_turn==turn);
    CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_BUSY);
    CHECK(chat_report(&f,turn,HH_CHAT_TTS_STARTED,true)==HH_OK);
    chat_sample(&f,45000);CHECK(f.core.audio_state==HH_AUDIO_CHAT_PLAY);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);chat_sample(&f,10000);
    CHECK(!f.core.chat_from_voice&&!f.core.chat_voice_speech);
    CHECK(f.core.audio_state==HH_AUDIO_IDLE&&count(&f,HH_ACT_CHAT_START)==1&&count(&f,HH_ACT_CHAT_SUBMIT)==1);
}
static void voice_activity_rejects_stale_turns_and_manual_capture(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);
    CHECK(hh_chat_voice_activity(NULL,1,true,0)==HH_INVALID);
    CHECK(hh_chat_voice_activity(&f.core,0,true,f.now)==HH_NOT_FOUND);
    chat_press(&f);uint32_t manual=f.core.chat_turn;
    CHECK(hh_chat_voice_activity(&f.core,manual,true,f.now)==HH_BUSY);
    chat_release(&f,300);CHECK(chat_report(&f,manual,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);uint32_t current=f.core.chat_turn;
    CHECK(current!=manual&&hh_chat_voice_activity(&f.core,manual,true,f.now)==HH_NOT_FOUND);
    CHECK(hh_chat_voice_activity(&f.core,current+1,false,f.now)==HH_NOT_FOUND);
    chat_sample(&f,8000);CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    CHECK(hh_chat_voice_activity(&f.core,current,true,f.now)==HH_BUSY);
    CHECK(chat_report(&f,current,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);uint32_t fresh=f.core.chat_turn;
    CHECK(fresh!=current&&!f.core.chat_voice_speech);
    CHECK(hh_chat_voice_activity(&f.core,current,true,f.now)==HH_NOT_FOUND);
    CHECK(hh_chat_voice_activity(&f.core,current,false,f.now)==HH_NOT_FOUND);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE&&!f.core.chat_voice_speech);
    f.now+=8000;CHECK(hh_chat_voice_activity(&f.core,fresh,true,f.now)==HH_BUSY);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&count(&f,HH_ACT_CHAT_SUBMIT)==1);
}
static void voice_capture_preserves_minimum_length(void) {
    Fixture f;
    for(unsigned ms=249;ms<=250;ms++){
        chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
        uint32_t turn=f.core.chat_turn;CHECK(hh_chat_voice_activity(&f.core,turn,true,f.now)==HH_OK);
        chat_sample(&f,ms);CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);
        CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==(ms==250?1u:0u));
        if(ms==249)CHECK(last(&f,HH_ACT_CHAT_STOP)->result==HH_TOO_SHORT);
    }
}
static void voice_capture_limit_never_submits_an_empty_turn(void) {
    Fixture f;
    for(unsigned speech=0;speech<2;speech++){
        chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
        uint32_t turn=f.core.chat_turn;
        if(speech)CHECK(hh_chat_voice_activity(&f.core,turn,true,f.now)==HH_OK);
        chat_sample(&f,29999);
        if(speech)CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);
        else CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
        chat_sample(&f,1);CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==speech);
        CHECK(f.core.audio_state==(speech?HH_AUDIO_CHAT_WAIT:HH_AUDIO_CHAT_STOPPING));
        chat_sample(&f,1000);CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==speech);
    }
}
static void physical_key_cancels_voice_and_requires_release_before_fallback(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
    uint32_t voice=f.core.chat_turn;chat_sample(&f,300);chat_press(&f);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&count(&f,HH_ACT_CHAT_SUBMIT)==0);
    CHECK(f.core.chat_key_requires_release&&last(&f,HH_ACT_CHAT_STOP)->result==HH_OK);
    CHECK(chat_report(&f,voice,HH_CHAT_QUIESCED,true)==HH_OK);
    CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_BUSY);chat_sample(&f,500);
    CHECK(count(&f,HH_ACT_CHAT_START)==1);chat_release(&f,0);
    CHECK(count(&f,HH_ACT_CHAT_SUBMIT)==0);chat_press(&f);chat_release(&f,300);
    CHECK(count(&f,HH_ACT_CHAT_START)==2&&count(&f,HH_ACT_CHAT_SUBMIT)==1);
}
static void raw_voice_cancel_key_cannot_cross_quiescence_into_a_new_turn(void) {
    Fixture f;chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
    uint32_t turn=f.core.chat_turn;chat_sample(&f,7999);f.chat=true;chat_sample(&f,1);
    CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&f.core.chat_key_requires_release);
    CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);chat_sample(&f,60);
    CHECK(count(&f,HH_ACT_CHAT_START)==1&&f.core.audio_state==HH_AUDIO_IDLE);
    chat_release(&f,0);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
}
static void local_record_and_mail_takeover_wait_for_voice_quiescence(void) {
    Fixture f;
    for(unsigned mail=0;mail<2;mail++){
        chat_init(&f,false,HH_CHAT_READY);CHECK(add(&f,"voice_takeover",0,HH_LEAVE)==HH_OK);
        CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);uint32_t turn=f.core.chat_turn;
        if(mail)f.keys[0]=false;else f.buttons[1]=true;
        chat_sample(&f,0);chat_sample(&f,60);
        CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&count(&f,HH_ACT_CHAT_SUBMIT)==0);
        CHECK(count(&f,HH_ACT_RECORD_START)==0&&count(&f,HH_ACT_PLAY_AUDIO)==0);
        CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,true)==HH_OK);
        CHECK(f.core.audio_state==(mail?HH_AUDIO_PLAYING:HH_AUDIO_RECORDING));
    }
}
static void voice_network_loss_and_reply_timeout_keep_cleanup_ownership(void) {
    Fixture f;
    for(unsigned waiting=0;waiting<2;waiting++){
        chat_init(&f,false,HH_CHAT_READY);CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);
        uint32_t turn=f.core.chat_turn;
        if(waiting){CHECK(hh_chat_voice_activity(&f.core,turn,true,f.now)==HH_OK);chat_sample(&f,300);
            CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);chat_sample(&f,45000);}
        else hh_chat_set_availability(&f.core,HH_CHAT_OFFLINE);
        CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&last(&f,HH_ACT_CHAT_STOP)->result==HH_IO_ERROR);
        CHECK(chat_report(&f,turn,HH_CHAT_QUIESCED,false)==HH_IO_ERROR);
        CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING);
    }
}
static void voice_timeouts_and_silence_handle_uint32_wrap(void) {
    Fixture f;
    for(unsigned speech=0;speech<2;speech++){
        chat_init(&f,false,HH_CHAT_READY);f.now=UINT32_MAX-100u;
        CHECK(hh_chat_voice_wake(&f.core,f.now)==HH_OK);uint32_t turn=f.core.chat_turn;
        if(speech){CHECK(hh_chat_voice_activity(&f.core,turn,true,f.now)==HH_OK);chat_sample(&f,300);
            CHECK(hh_chat_voice_activity(&f.core,turn,false,f.now)==HH_OK);CHECK(f.core.audio_state==HH_AUDIO_CHAT_WAIT);
            chat_sample(&f,45000);}
        else {chat_sample(&f,7999);CHECK(f.core.audio_state==HH_AUDIO_CHAT_CAPTURE);chat_sample(&f,1);}
        CHECK(f.core.audio_state==HH_AUDIO_CHAT_STOPPING&&count(&f,HH_ACT_CHAT_STOP)==1);
    }
}

#define RUN(test) do { int before=failures; test(); if (before==failures) printf("PASS %s\n",#test); } while(0)
static HH_Result add_any_remote(Fixture *f,const char *id,unsigned recipient){
    HH_Message m={0};snprintf(m.id,sizeof(m.id),"%s",id);
    snprintf(m.audio_path,sizeof(m.audio_path),"/sdcard/homehub/messages/%s.wav",id);
    m.recipient=(uint8_t)recipient;m.trigger=HH_ANY;m.source=HH_REMOTE;
    return hh_add_message(&f->core,&m);
}
static void remote_any_plays_on_next_arrive_or_leave_once(void){
    for(unsigned present=0;present<2;present++){
        Fixture f;init(&f,present!=0,true,false);CHECK(add_any_remote(&f,"sb_123",0)==HH_OK);
        f.keys[1]=false;settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==0);
        f.keys[0]=!f.keys[0];settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
        f.keys[0]=!f.keys[0];settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
        CHECK(hh_playback_finished(&f.core,"sb_123",true)==HH_OK);
        f.keys[0]=!f.keys[0];settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
        CHECK(add_any_remote(&f,"sb_123",0)==HH_DUPLICATE);
    }
}
static void remote_any_survives_reboot_with_changed_initial_key_position(void){
    Fixture f,g;HH_Snapshot saved;init(&f,true,false,false);
    CHECK(add_any_remote(&f,"sb_456",0)==HH_OK);hh_export(&f.core,&saved);
    init(&g,false,false,false);CHECK(hh_restore(&g.core,&saved)==HH_OK);
    CHECK(count(&g,HH_ACT_PLAY_AUDIO)==0);g.keys[0]=true;settle(&g);
    CHECK(count(&g,HH_ACT_PLAY_AUDIO)==1);
    CHECK(!strcmp(last(&g,HH_ACT_PLAY_AUDIO)->id,"sb_456"));
}
static void failed_remote_any_retries_next_edge_and_local_any_is_rejected(void){
    Fixture f;init(&f,true,false,false);CHECK(add_any_remote(&f,"sb_789",0)==HH_OK);
    f.keys[0]=false;settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(hh_playback_finished(&f.core,"sb_789",false)==HH_OK);
    f.keys[0]=true;settle(&f);CHECK(count(&f,HH_ACT_PLAY_AUDIO)==2);
    CHECK(add(&f,"local_bad",1,HH_ANY)==HH_INVALID);
    CHECK(hh_set_next_trigger(&f.core,1,HH_ANY)==HH_INVALID);
    CHECK(hh_set_record_trigger(&f.core,HH_ANY)==HH_INVALID);
}
static void timed_init(Fixture *f,uint32_t boot) {
    HH_Config config=hh_default_config();memset(f,0,sizeof(*f));
    config.reminder_interval_ms=60000;config.pir_enabled=false;f->now=boot;
    hh_init(&f->core,&config,capture,&f->log,boot);sample(f,0);
}
static void timer_reminder_runs_without_pir_or_keys_at_sixty_seconds(void) {
    Fixture f;timed_init(&f,0);sample(&f,59999);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(f.core.audio_state==HH_AUDIO_GREETING);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/homehub/voices/take_key.wav",true)==HH_OK);
    CHECK(f.core.audio_state==HH_AUDIO_PLAYING&&count(&f,HH_ACT_PLAY_AUDIO)==1);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);
    sample(&f,59999);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    sample(&f,1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2);
}
static void timer_reminder_defers_busy_and_coalesces_without_a_catchup_burst(void) {
    Fixture f;timed_init(&f,0);
    CHECK(hh_request_greeting(&f.core,0,HH_ARRIVE)==HH_OK);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/arrive.wav",true)==HH_OK);
    sample(&f,90000);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    CHECK(f.core.audio_state==HH_AUDIO_PLAYING);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(hh_greeting_ready(&f.core,"/sdcard/take_key.wav",true)==HH_OK);
    CHECK(hh_playback_finished(&f.core,"greeting",true)==HH_OK);sample(&f,0);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    sample(&f,59999);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    sample(&f,1);CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2);
}
static void timer_reminder_yields_to_raw_record_and_chat_presses(void) {
    Fixture f;
    for(unsigned chat=0;chat<2;chat++) {
        timed_init(&f,0);if(chat)f.chat=true;else f.buttons[0]=true;
        f.now=60000;hh_sample_with_chat(&f.core,f.now,f.keys,f.buttons,false,f.chat);
        CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);
    }
}
static void timer_reminder_handles_wrap_and_missing_audio_without_stalling(void) {
    Fixture f;timed_init(&f,UINT32_MAX-100u);sample(&f,59999);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==0);sample(&f,1);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==1);
    CHECK(hh_greeting_ready(&f.core,NULL,false)==HH_MISSING_ASSET);
    CHECK(f.core.audio_state==HH_AUDIO_IDLE);sample(&f,60000);
    CHECK(count(&f,HH_ACT_KEY_REMINDER_REQUEST)==2);
}
int main(void) {
    RUN(timer_reminder_runs_without_pir_or_keys_at_sixty_seconds);
    RUN(timer_reminder_defers_busy_and_coalesces_without_a_catchup_burst);
    RUN(timer_reminder_yields_to_raw_record_and_chat_presses);
    RUN(timer_reminder_handles_wrap_and_missing_audio_without_stalling);
    RUN(remote_any_plays_on_next_arrive_or_leave_once);
    RUN(remote_any_survives_reboot_with_changed_initial_key_position);
    RUN(failed_remote_any_retries_next_edge_and_local_any_is_rejected);
    RUN(initial_inputs_do_not_invent_edges); RUN(both_edges_wait_for_stability);
    RUN(explicit_self_reminder_waits_until_evening); RUN(short_press_cancels_and_long_hold_stops_at_30s);
    RUN(unfinished_recording_cannot_trigger_itself); RUN(simultaneous_slots_serialize_and_failure_stays_unread);
    RUN(events_during_recording_queue_without_audio_overlap); RUN(snapshot_restore_rearms_only_new_edges);
    RUN(bounded_queue_and_id_validation); RUN(pir_warmup_throttle_and_missing_assets_are_explicit);
    RUN(pir_reminds_when_either_single_key_is_still_present); RUN(recorder_failure_never_saves_or_marks_played);
    RUN(elapsed_time_handles_uint32_wrap);
    RUN(invalid_trigger_override_is_rejected); RUN(repeated_remote_played_message_retries_receipt);
    RUN(recording_reserves_capacity_and_save_failure_stays_unread);
    RUN(one_shot_trigger_can_be_chosen_before_holding_button);
    RUN(key_greeting_preserves_context_and_yields_to_mail);
    RUN(pir_with_no_keys_only_wakes_screen);RUN(disabled_pir_does_not_wake_or_remind);
    RUN(continuous_pir_rearms_at_quiet_boundary_without_wake_spam);
    RUN(pir_high_edge_inside_quiet_is_not_lost_at_rearm);
    RUN(high_pir_with_no_keys_waits_for_a_key_without_fake_edges);
    RUN(expired_high_pir_wait_stays_cancelled_until_a_real_low);
    RUN(pir_never_consumes_or_acknowledges_personal_mail);
    RUN(busy_pir_edges_merge_then_dispatch_once_when_audio_is_idle);RUN(busy_pir_merge_does_not_extend_the_ten_second_deadline);
    RUN(all_keys_taken_cancels_a_busy_pending_reminder);RUN(prepared_reminder_rechecks_keys_before_playback);
    RUN(recording_save_and_queued_mail_run_before_pir_reminder);RUN(busy_reminder_deadline_handles_timer_wrap);
    RUN(queued_mail_preempts_a_reminder_still_preparing);RUN(taking_then_returning_all_keys_cannot_revive_old_preparation);
    RUN(prepared_reminder_expires_without_releasing_its_audio_reservation_early);
    RUN(preparation_keeps_the_original_busy_pending_deadline);RUN(fresh_activity_cannot_extend_an_old_preparation_deadline);
    RUN(raw_record_button_press_wins_over_same_frame_pir);
    RUN(unavailable_chat_never_starts_capture);RUN(chat_readiness_does_not_turn_an_existing_hold_into_capture);
    RUN(chat_boot_held_requires_release_before_start);RUN(chat_debounce_hold_and_release_submit_exactly_one_turn);
    RUN(short_chat_hold_requests_stop_instead_of_submit);RUN(chat_capture_limit_submits_once_and_release_does_not_cancel);
    RUN(unanswered_chat_times_out_after_45_seconds_without_unlocking_audio);RUN(pressing_key_again_cancels_waiting_or_playing_chat);
    RUN(losing_availability_stops_each_active_chat_phase);RUN(raw_local_recording_wins_over_same_frame_chat_key);
    RUN(raw_chat_key_blocks_pir_while_waiting_for_debounce);RUN(local_recording_takeover_waits_for_current_chat_quiescence);
    RUN(released_local_button_is_not_recorded_after_chat_stops);RUN(mailbox_event_stops_chat_then_plays_after_quiesced);
    RUN(old_turn_reports_cannot_change_a_new_chat);RUN(chat_reports_respect_capture_wait_and_play_states);
    RUN(chat_start_failure_requests_cleanup_without_unlocking);RUN(failed_quiescence_keeps_chat_owned_until_a_successful_confirmation);
    RUN(queued_mail_wins_over_same_frame_new_chat);RUN(chat_wait_timeout_handles_uint32_wrap);
    RUN(pir_waits_for_chat_to_quiesce_before_reminding);
    RUN(unchanged_chat_availability_does_not_flood_ui_actions);RUN(tts_report_after_wait_deadline_cannot_revive_the_turn);
    RUN(raw_local_takeover_at_quiescence_keeps_mail_waiting_for_debounce);
    RUN(old_chat_raw_key_candidate_cannot_restart_after_quiescence);
    RUN(old_chat_candidate_requires_a_stable_release_before_new_capture);
    RUN(partial_release_before_quiescence_does_not_rearm_old_candidate);
    RUN(voice_wake_starts_one_turn_without_key_edges);RUN(unavailable_voice_wakes_are_quiet);
    RUN(voice_wake_yields_to_raw_record_buttons_and_key);RUN(voice_wake_preserves_local_record_save_mail_and_pir_priority);
    RUN(voice_without_speech_cancels_at_eight_seconds_without_upload);
    RUN(voice_speech_then_silence_submits_once_and_finishes_one_reply);
    RUN(voice_activity_rejects_stale_turns_and_manual_capture);RUN(voice_capture_preserves_minimum_length);
    RUN(voice_capture_limit_never_submits_an_empty_turn);
    RUN(physical_key_cancels_voice_and_requires_release_before_fallback);
    RUN(raw_voice_cancel_key_cannot_cross_quiescence_into_a_new_turn);
    RUN(local_record_and_mail_takeover_wait_for_voice_quiescence);
    RUN(voice_network_loss_and_reply_timeout_keep_cleanup_ownership);RUN(voice_timeouts_and_silence_handle_uint32_wrap);
    printf("%d failure(s)\n",failures); return failures?1:0;
}
