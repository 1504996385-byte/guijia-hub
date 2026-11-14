/* Uses actual storage C and verbatim receipt functions sliced from app.c by
 * run_tickets.py. Core implementation is linked unchanged by the host runner. */
#define HH_TEST_EVENT_EXTRA_TYPES ,EV_ACTION,EV_REMOTE_MESSAGE,EV_SET_TRIGGER,EV_NETWORK_INFO,EV_CHAT_REPORT,EV_CHAT_TEXT,EV_CHAT_EMOTION
#define HH_TEST_EVENT_EXTRA_FIELDS HH_Message message; int trigger; char text[192]; HH_ChatEvent chat_event; bool chat_is_user_text; uint8_t emotion;
#include "../../firmware/core/homehub_emotion.h"
#define main storage_suite_unused_main
#include "test_storage.c"
#undef main
#define PR_NOTICE(...) ((void)0)

static HH_Core core;
static HH_Action network_actions[32],last_played_action;
static unsigned network_count,ui_errors,core_play_count;
static unsigned cloud_download_receipts,cloud_key_actions,cloud_light_actions;
static char cloud_download_id[HH_ID_MAX+1];
static void hh_supabase_downloaded(const HH_Message *m){cloud_download_receipts++;snprintf(cloud_download_id,sizeof(cloud_download_id),"%s",m->id);}
static void hh_supabase_action(const HH_Action *a){(void)a;cloud_key_actions++;}
static void hh_tuya_light_action(const HH_Action *a){(void)a;cloud_light_actions++;}
static QUEUE_HANDLE hh_audio_jobs=(QUEUE_HANDLE)(uintptr_t)2;
static HH_Action app_audio_actions[8];
static unsigned app_audio_count,app_random_calls,app_ready_calls;
static bool app_queue_reject,app_ready_success;
static const char *app_ready_path;
static uint32_t app_cancel_turn;
static uint32_t app_now;
static unsigned app_network_ui_count,app_emotion_calls;
static uint32_t app_emotion_turn;static HH_Emotion app_emotion;
static bool capture_core_actions;
static HH_Action core_action_history[128];
static unsigned core_action_head,core_action_count;
static void hh_net_action(const HH_Action *a){if(network_count<32)network_actions[network_count]=*a;network_count++;}
static void hh_net_downloaded(const HH_Message *m){(void)m;}
static void hh_ui_feedback(const char *text){(void)text;ui_errors++;}
static void hh_ui_wake(void){}
static void hh_ui_network(const char *text){(void)text;app_network_ui_count++;}
static void hh_ui_chat_text(bool user,const char *text){(void)user;(void)text;}
static void hh_ui_chat_emotion(uint32_t turn,HH_Emotion emotion){app_emotion_calls++;app_emotion_turn=turn;app_emotion=emotion;}
static void hh_ui_update(const HH_Action *a,HH_AudioState state){(void)a;(void)state;}
static void hh_audio_cancel(void){}
static uint32_t tal_system_get_random(uint32_t upper){(void)upper;app_random_calls++;return 2;}
static uint32_t tal_system_get_millisecond(void){return app_now;}
static void hh_chat_cloud_cancel(uint32_t turn){app_cancel_turn=turn;}
static HH_ChatAvailability hh_chat_cloud_availability(void){return HH_CHAT_READY;}
static void hh_chat_cloud_status(char *out,size_t capacity){snprintf(out,capacity,"聊天可用");}
static int app_queue_post(QUEUE_HANDLE queue,void *item,unsigned wait){
    (void)wait;if(queue!=hh_audio_jobs||app_queue_reject||app_audio_count>=8)return -1;
    app_audio_actions[app_audio_count++]=*(HH_Action*)item;return OPRT_OK;
}
static HH_Result app_greeting_ready(HH_Core *c,const char *path,bool success){
    app_ready_calls++;app_ready_success=success;app_ready_path=path;return hh_greeting_ready(c,path,success);
}
#ifndef HH_RECEIPT_SOURCE
#error "run_tickets.py must provide the verbatim app receipt source slice"
#endif
#define tal_queue_post app_queue_post
#define hh_greeting_ready app_greeting_ready
#include HH_RECEIPT_SOURCE
#undef hh_greeting_ready
#undef tal_queue_post

static void core_action(void *context,const HH_Action *a){
    (void)context;if(a->type==HH_ACT_PLAYED_RECEIPT)last_played_action=*a;if(a->type==HH_ACT_PLAY_AUDIO)core_play_count++;
    if(capture_core_actions&&core_action_count<128)core_action_history[core_action_count++]=*a;
}
static void reset_receipts(void){reset();memset(receipts,0,sizeof(receipts));next_ticket=last_receipt_retry=network_count=ui_errors=core_play_count=0;memset(&last_played_action,0,sizeof(last_played_action));hh_init(&core,NULL,core_action,NULL,0);}
static HH_Action played(const char*id){HH_Action a;memset(&a,0,sizeof(a));a.type=HH_ACT_PLAYED_RECEIPT;a.source=HH_REMOTE;snprintf(a.id,sizeof(a.id),"%s",id);return a;}
static void unrelated_and_zero_acks_do_not_release_receipts(void){
    HH_Action a=played("first");reset_receipts();defer_receipt(&a);uint32_t ticket=receipts[0].ticket;
    CHECK(ticket!=0&&network_count==0);receipt_commit(0,true);receipt_commit(ticket+99,true);CHECK(network_count==0);
    run_worker();CHECK(latest_event.ok&&latest_event.action.event_seq==ticket);
    receipt_commit(latest_event.action.event_seq,latest_event.ok);CHECK(network_count==1);
    CHECK(strcmp(network_actions[0].id,"first")==0);receipt_commit(ticket,true);CHECK(network_count==1);
    a.source=HH_LOCAL;defer_receipt(&a);CHECK(network_count==1&&!receipts[0].used);
}
static void failed_ack_retains_receipt_and_requires_new_success_ticket(void){
    HH_Action a=played("retry");reset_receipts();defer_receipt(&a);uint32_t old_ticket=receipts[0].ticket;
    rename_failures=1;run_worker();CHECK(!latest_event.ok);receipt_commit(latest_event.action.event_seq,false);
    CHECK(receipts[0].used&&receipts[0].ticket==0&&network_count==0);
    receipt_commit(old_ticket,true);CHECK(network_count==0);retry_receipts(999);CHECK(receipts[0].ticket==0);
    retry_receipts(1000);uint32_t new_ticket=receipts[0].ticket;CHECK(new_ticket&&new_ticket!=old_ticket);
    receipt_commit(old_ticket,true);CHECK(network_count==0);run_worker();CHECK(latest_event.ok);
    receipt_commit(new_ticket,true);CHECK(network_count==1&&!receipts[0].used);
}
static void queue_rejection_cannot_release_on_ordinary_snapshot_acks(void){
    HH_Action a=played("full_queue");HH_Snapshot s=snapshot("ordinary");reset_receipts();
    for(unsigned i=0;i<8;i++)CHECK(hh_save_snapshot_async(&s));
    defer_receipt(&a);CHECK(receipts[0].used&&receipts[0].ticket==0&&ui_errors==1&&network_count==0);
    run_worker();for(unsigned i=0;i<event_count;i++)receipt_commit(event_history[i].action.event_seq,event_history[i].ok);
    CHECK(network_count==0);retry_receipts(1000);CHECK(receipts[0].ticket!=0);run_worker();
    receipt_commit(latest_event.action.event_seq,latest_event.ok);CHECK(network_count==1);
}
static void shared_retry_snapshot_releases_only_its_waiting_receipts(void){
    HH_Action a=played("shared_a"),b=played("shared_b");reset_receipts();allocation_failure=true;defer_receipt(&a);
    allocation_failure=true;defer_receipt(&b);CHECK(receipts[0].used&&receipts[1].used);
    CHECK(receipts[0].ticket==0&&receipts[1].ticket==0);retry_receipts(1000);
    CHECK(receipts[0].ticket&&receipts[0].ticket==receipts[1].ticket);
    CHECK(network_count==0);run_worker();receipt_commit(latest_event.action.event_seq,latest_event.ok);
    CHECK(network_count==2&&!receipts[0].used&&!receipts[1].used);
    CHECK(strcmp(network_actions[0].id,"shared_a")==0&&strcmp(network_actions[1].id,"shared_b")==0);
}
static void restored_played_poll_waits_for_durable_ack_without_replaying(void){
    HH_Message m;HH_Snapshot s=snapshot("already_played");reset_receipts();CHECK(hh_restore(&core,&s)==HH_OK);
    memset(&m,0,sizeof(m));snprintf(m.id,sizeof(m.id),"already_played");snprintf(m.audio_path,sizeof(m.audio_path),"/sdcard/played.wav");m.source=HH_REMOTE;
    CHECK(hh_add_message(&core,&m)==HH_DUPLICATE);CHECK(last_played_action.type==HH_ACT_PLAYED_RECEIPT);
    defer_receipt(&last_played_action);CHECK(network_count==0&&core_play_count==0);
    run_worker();receipt_commit(latest_event.action.event_seq,latest_event.ok);
    CHECK(network_count==1&&core_play_count==0);
}
static void reset_dispatch(void){
    reset_receipts();memset(app_audio_actions,0,sizeof(app_audio_actions));app_audio_count=app_random_calls=app_ready_calls=0;
    app_queue_reject=app_ready_success=false;app_ready_path=NULL;pending_greeting=false;use_network_greeting=false;
    app_cancel_turn=app_now=app_network_ui_count=app_emotion_calls=app_emotion_turn=0;
    network_greeting_pending=false;network_greeting_deadline=0;greeting_busy_reported=false;
    capture_core_actions=false;core_action_head=core_action_count=0;
    memset(&network_greeting,0,sizeof(network_greeting));
}
static void drain_app_actions(void){
    while(core_action_head<core_action_count){HH_Action a=core_action_history[core_action_head++];dispatch(&a);}
}
static void run_app_pending(uint32_t now){
    const bool buttons[2]={false,false};app_now=now;
    app_pending_greetings_under_test(buttons,false,now);drain_app_actions();
}
static void initialize_magnet_integration(bool first_present,bool second_present){
    const bool keys[2]={first_present,second_present},buttons[2]={false,false};
    reset_dispatch();capture_core_actions=true;
    hh_sample_with_chat(&core,0,keys,buttons,false,false);drain_app_actions();
}
static void remove_first_magnet(void){
    const bool keys[2]={false,false},buttons[2]={false,false};
    hh_sample_with_chat(&core,1,keys,buttons,false,false);
    hh_sample_with_chat(&core,61,keys,buttons,false,false);drain_app_actions();
}
static void leave_key_event_logs_and_schedules_the_farewell(void){
    HH_Action a={0};reset_dispatch();a.type=HH_ACT_KEY_EVENT;a.trigger=HH_LEAVE;a.recipient=1;
    dispatch(&a);
    CHECK(network_count==1&&network_actions[0].type==HH_ACT_KEY_EVENT);
    CHECK(network_actions[0].trigger==HH_LEAVE&&network_actions[0].recipient==1);
    CHECK(pending_greeting&&!network_greeting_pending&&app_audio_count==0);
    CHECK(greeting_trigger==HH_LEAVE&&greeting_recipient==1);
}
static void leaving_without_a_message_prepares_the_fixed_offline_farewell(void){
    initialize_magnet_integration(true,false);remove_first_magnet();run_app_pending(62);
    CHECK(network_count==1&&network_actions[0].trigger==HH_LEAVE);
    CHECK(core_play_count==0&&app_audio_count==1&&core.audio_state==HH_AUDIO_GREETING);
    CHECK(app_audio_actions[0].type==HH_ACT_GREETING_REQUEST);
    CHECK(!strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/leave.wav")&&app_random_calls==0);
}
static void leaving_plays_its_message_before_the_farewell(void){
    HH_Message m={0};initialize_magnet_integration(true,false);
    snprintf(m.id,sizeof(m.id),"my_leave");snprintf(m.audio_path,sizeof(m.audio_path),HH_ROOT"/messages/my_leave.wav");
    m.recipient=0;m.trigger=HH_LEAVE;m.source=HH_LOCAL;CHECK(hh_add_message(&core,&m)==HH_OK);
    snprintf(m.id,sizeof(m.id),"their_leave");snprintf(m.audio_path,sizeof(m.audio_path),HH_ROOT"/messages/their_leave.wav");
    m.recipient=1;CHECK(hh_add_message(&core,&m)==HH_OK);drain_app_actions();
    remove_first_magnet();
    CHECK(app_audio_count==1&&app_audio_actions[0].type==HH_ACT_PLAY_AUDIO);
    CHECK(strcmp(app_audio_actions[0].id,"my_leave")==0);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/messages/my_leave.wav")==0);
    CHECK(hh_pending_count(&core,1)==1);
    CHECK(hh_playback_finished(&core,"my_leave",true)==HH_OK);drain_app_actions();run_app_pending(62);
    CHECK(app_audio_count==2&&core_play_count==1&&core.audio_state==HH_AUDIO_GREETING);
    CHECK(!strcmp(app_audio_actions[1].audio_path,HH_ROOT"/voices/leave.wav"));
    CHECK(hh_pending_count(&core,1)==1);
}
static void arrival_schedules_the_fixed_local_welcome(void){
    HH_Action a={0};reset_dispatch();capture_core_actions=true;
    a.type=HH_ACT_KEY_EVENT;a.trigger=HH_ARRIVE;a.recipient=1;dispatch(&a);run_app_pending(100);
    CHECK(network_count==1&&network_actions[0].trigger==HH_ARRIVE);
    CHECK(app_audio_count==1&&app_audio_actions[0].type==HH_ACT_GREETING_REQUEST);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/arrive.wav")==0&&app_random_calls==0);
}
static void magnet_arrival_uses_fixed_welcome_despite_cached_network_voice(void){
    const bool keys[2]={true,false},buttons[2]={false,false};
    initialize_magnet_integration(false,false);
    HH_Event e={.type=EV_NETWORK_INFO,.ok=true};e.action.trigger=HH_ARRIVE;e.action.recipient=0;
    snprintf(e.action.audio_path,sizeof(e.action.audio_path),HH_ROOT"/voices/network_cached.wav");handle(&e);
    /* A stale selection must not promote cached network audio over this local
     * magnet action; only the independent network scheduler may select it. */
    use_network_greeting=true;
    hh_sample_with_chat(&core,1,keys,buttons,false,false);
    hh_sample_with_chat(&core,61,keys,buttons,false,false);drain_app_actions();run_app_pending(62);
    CHECK(network_count==1&&network_actions[0].trigger==HH_ARRIVE);
    CHECK(app_audio_count==1&&!strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/arrive.wav"));
    CHECK(app_random_calls==0&&!use_network_greeting&&network_greeting_pending);
}
static void arriving_plays_its_message_before_the_fixed_welcome(void){
    const bool keys[2]={true,false},buttons[2]={false,false};HH_Message m={0};
    initialize_magnet_integration(false,false);
    snprintf(m.id,sizeof(m.id),"my_arrival");snprintf(m.audio_path,sizeof(m.audio_path),HH_ROOT"/messages/my_arrival.wav");
    m.recipient=0;m.trigger=HH_ARRIVE;m.source=HH_LOCAL;CHECK(hh_add_message(&core,&m)==HH_OK);drain_app_actions();
    hh_sample_with_chat(&core,1,keys,buttons,false,false);
    hh_sample_with_chat(&core,61,keys,buttons,false,false);drain_app_actions();run_app_pending(62);
    CHECK(app_audio_count==1&&app_audio_actions[0].type==HH_ACT_PLAY_AUDIO&&!strcmp(app_audio_actions[0].id,"my_arrival"));
    CHECK(pending_greeting&&core.audio_state==HH_AUDIO_PLAYING);
    CHECK(hh_playback_finished(&core,"my_arrival",true)==HH_OK);drain_app_actions();run_app_pending(63);
    CHECK(app_audio_count==2&&app_audio_actions[1].type==HH_ACT_GREETING_REQUEST);
    CHECK(!strcmp(app_audio_actions[1].audio_path,HH_ROOT"/voices/arrive.wav")&&app_random_calls==0);
}
static void completed_leave_network_voice_is_not_scheduled_or_played(void){
    HH_Event e={0};reset_dispatch();capture_core_actions=true;
    e.type=EV_NETWORK_INFO;e.ok=true;e.action.type=HH_ACT_KEY_EVENT;e.action.trigger=HH_LEAVE;
    snprintf(e.action.audio_path,sizeof(e.action.audio_path),HH_ROOT"/voices/net0_1.wav");
    snprintf(e.text,sizeof(e.text),"network leave greeting");handle(&e);run_app_pending(100);
    CHECK(app_network_ui_count==1);
    CHECK(!network_greeting_pending&&app_audio_count==0&&core.audio_state==HH_AUDIO_IDLE);
}
static void arrival_network_voice_remains_available(void){
    HH_Event e={0};reset_dispatch();capture_core_actions=true;
    e.type=EV_NETWORK_INFO;e.ok=true;e.action.type=HH_ACT_KEY_EVENT;e.action.trigger=HH_ARRIVE;
    snprintf(e.action.audio_path,sizeof(e.action.audio_path),HH_ROOT"/voices/net0_0.wav");handle(&e);run_app_pending(100);
    CHECK(app_network_ui_count==1&&app_audio_count==1);
    CHECK(app_audio_actions[0].type==HH_ACT_GREETING_REQUEST);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/net0_0.wav")==0);
}
static void pir_still_requests_the_dedicated_take_key_audio(void){
    const bool keys[2]={true,false},buttons[2]={false,false};HH_Config cfg=hh_default_config();
    reset_dispatch();capture_core_actions=true;cfg.pir_enabled=true;
    hh_init(&core,&cfg,core_action,NULL,0);
    hh_sample_with_chat(&core,0,keys,buttons,false,false);
    hh_sample_with_chat(&core,60000,keys,buttons,true,false);drain_app_actions();
    CHECK(app_audio_count==1&&app_audio_actions[0].type==HH_ACT_KEY_REMINDER_REQUEST);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/take_key.wav")==0);
    CHECK(network_count==0&&!pending_greeting&&app_random_calls==0);
}
static void reminder_dispatch_keeps_type_and_fixed_path_despite_cached_network_voice(void){
    HH_Action a={0};reset_dispatch();a.type=HH_ACT_KEY_REMINDER_REQUEST;a.trigger=HH_LEAVE;a.event_seq=44;
    use_network_greeting=true;snprintf(network_greeting.audio_path,sizeof(network_greeting.audio_path),"/sdcard/network.wav");
    dispatch(&a);CHECK(app_audio_count==1);CHECK(app_audio_actions[0].type==HH_ACT_KEY_REMINDER_REQUEST);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/take_key.wav")==0);
    CHECK(app_audio_actions[0].event_seq==44&&app_random_calls==0&&network_count==0&&!pending_greeting);
    CHECK(use_network_greeting);
}
static void rejected_reminder_dispatch_reports_preparation_failure(void){
    HH_Action a={0};reset_dispatch();a.type=HH_ACT_KEY_REMINDER_REQUEST;app_queue_reject=true;
    dispatch(&a);CHECK(app_audio_count==0&&app_ready_calls==1&&!app_ready_success&&app_ready_path==NULL);
}
static void ordinary_greeting_dispatch_uses_fixed_welcome_and_explicit_network_path(void){
    HH_Action a={0};reset_dispatch();a.type=HH_ACT_GREETING_REQUEST;a.trigger=HH_ARRIVE;
    dispatch(&a);CHECK(app_audio_count==1&&app_audio_actions[0].type==HH_ACT_GREETING_REQUEST);
    CHECK(strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/arrive.wav")==0&&app_random_calls==0);
    use_network_greeting=true;snprintf(network_greeting.audio_path,sizeof(network_greeting.audio_path),"/sdcard/network.wav");
    dispatch(&a);CHECK(app_audio_count==2&&!use_network_greeting);
    CHECK(strcmp(app_audio_actions[1].audio_path,"/sdcard/network.wav")==0&&app_random_calls==0);
}
static void farewell_ignores_cached_network_audio_and_random_variants(void){
    HH_Action a={.type=HH_ACT_GREETING_REQUEST,.trigger=HH_LEAVE};reset_dispatch();
    use_network_greeting=true;snprintf(network_greeting.audio_path,sizeof(network_greeting.audio_path),"/sdcard/network.wav");
    dispatch(&a);CHECK(app_audio_count==1&&app_random_calls==0);
    CHECK(!strcmp(app_audio_actions[0].audio_path,HH_ROOT"/voices/leave.wav"));
}
static void chat_dispatch_preserves_commands_and_turns(void){
    HH_Action a={0};reset_dispatch();a.chat_turn=71;
    HH_ActionType commands[]={HH_ACT_CHAT_START,HH_ACT_CHAT_SUBMIT,HH_ACT_CHAT_STOP};
    for(unsigned i=0;i<3;i++){a.type=commands[i];dispatch(&a);}
    CHECK(app_audio_count==3&&app_cancel_turn==0);
    for(unsigned i=0;i<3;i++)CHECK(app_audio_actions[i].type==commands[i]&&app_audio_actions[i].chat_turn==71);
}
static void full_audio_queue_cancels_chat_without_pretending_it_quiesced(void){
    bool keys[2]={false,false},buttons[2]={false,false};reset_dispatch();
    hh_chat_set_availability(&core,HH_CHAT_READY);
    hh_sample_with_chat(&core,0,keys,buttons,false,false);
    hh_sample_with_chat(&core,1,keys,buttons,false,true);
    hh_sample_with_chat(&core,61,keys,buttons,false,true);
    CHECK(core.audio_state==HH_AUDIO_CHAT_CAPTURE&&core.chat_turn!=0);
    HH_Action a={0};a.type=HH_ACT_CHAT_START;a.chat_turn=core.chat_turn;app_queue_reject=true;dispatch(&a);
    CHECK(app_audio_count==0&&app_cancel_turn==a.chat_turn);
    CHECK(core.audio_state==HH_AUDIO_CHAT_STOPPING);
}
static void emotion_routing_requires_current_active_reply_turn(void){
    reset_dispatch();HH_Event e={.type=EV_CHAT_EMOTION,.emotion=HH_EMOTION_HAPPY};e.action.chat_turn=700;
    core.chat_turn=700;core.audio_state=HH_AUDIO_CHAT_WAIT;handle(&e);
    CHECK(app_emotion_calls==1&&app_emotion_turn==700&&app_emotion==HH_EMOTION_HAPPY);
    core.audio_state=HH_AUDIO_CHAT_PLAY;e.emotion=HH_EMOTION_SAD;handle(&e);CHECK(app_emotion_calls==2&&app_emotion==HH_EMOTION_SAD);
    e.action.chat_turn=699;handle(&e);CHECK(app_emotion_calls==2);
    e.action.chat_turn=700;core.audio_state=HH_AUDIO_CHAT_STOPPING;handle(&e);CHECK(app_emotion_calls==2);
    core.audio_state=HH_AUDIO_IDLE;handle(&e);CHECK(app_emotion_calls==2);
    core.audio_state=HH_AUDIO_CHAT_CAPTURE;handle(&e);CHECK(app_emotion_calls==2);
    core.audio_state=HH_AUDIO_CHAT_PLAY;e.emotion=HH_EMOTION_COUNT;handle(&e);CHECK(app_emotion_calls==2);
    core.chat_turn=e.action.chat_turn=0;e.emotion=HH_EMOTION_HAPPY;handle(&e);CHECK(app_emotion_calls==2);
}
static void supabase_download_waits_for_matching_durable_snapshot_ack(void){
    const char *ids[]={"sb_42","px_42"};
    for(unsigned i=0;i<2;i++){
    const char *id=ids[i];
    reset_receipts();cloud_download_receipts=0;cloud_download_id[0]=0;
    HH_Event e={.type=EV_REMOTE_MESSAGE};snprintf(e.message.id,sizeof(e.message.id),"%s",id);
    snprintf(e.message.audio_path,sizeof(e.message.audio_path),"/sdcard/homehub/messages/%s.wav",id);
    e.message.source=HH_REMOTE;e.message.recipient=0;e.message.trigger=HH_ANY;
    handle(&e);CHECK(hh_find_message(&core,id)!=NULL);CHECK(cloud_download_receipts==0);
    CHECK(receipts[0].used&&receipts[0].ticket!=0);uint32_t ticket=receipts[0].ticket;
    handle(&e);CHECK(cloud_download_receipts==0);
    receipt_commit(ticket+77,true);receipt_commit(0,true);CHECK(cloud_download_receipts==0);
    receipt_commit(ticket,false);CHECK(cloud_download_receipts==0&&receipts[0].used);
    retry_receipts(1000);ticket=receipts[0].ticket;CHECK(ticket!=0);
    receipt_commit(ticket,true);CHECK(cloud_download_receipts==1&&!strcmp(cloud_download_id,id));
    receipt_commit(ticket,true);CHECK(cloud_download_receipts==1);
    }
}
static void cloud_download_and_played_receipts_do_not_suppress_each_other(void){
    reset_receipts();cloud_download_receipts=0;
    HH_Event e={.type=EV_REMOTE_MESSAGE};snprintf(e.message.id,sizeof(e.message.id),"sb_43");
    snprintf(e.message.audio_path,sizeof(e.message.audio_path),"/sdcard/homehub/messages/sb_43.wav");
    e.message.source=HH_REMOTE;e.message.trigger=HH_ANY;handle(&e);
    HH_Action p=played("sb_43");defer_receipt(&p);
    CHECK(receipts[0].used&&receipts[1].used);CHECK(receipts[0].ticket!=receipts[1].ticket);
    receipt_commit(receipts[1].ticket,true);CHECK(cloud_download_receipts==0);
    receipt_commit(receipts[0].ticket,true);CHECK(cloud_download_receipts==1);
}
int main(int argc,char**argv){if(argc!=2)return 2;snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(supabase_download_waits_for_matching_durable_snapshot_ack);
    RUN(cloud_download_and_played_receipts_do_not_suppress_each_other);
    RUN(emotion_routing_requires_current_active_reply_turn);
    RUN(tagged_snapshot_ack_keeps_success_and_failure_tickets);
    RUN(unrelated_and_zero_acks_do_not_release_receipts);
    RUN(failed_ack_retains_receipt_and_requires_new_success_ticket);
    RUN(queue_rejection_cannot_release_on_ordinary_snapshot_acks);
    RUN(shared_retry_snapshot_releases_only_its_waiting_receipts);
    RUN(restored_played_poll_waits_for_durable_ack_without_replaying);
    RUN(leave_key_event_logs_and_schedules_the_farewell);
    RUN(leaving_without_a_message_prepares_the_fixed_offline_farewell);
    RUN(leaving_plays_its_message_before_the_farewell);
    RUN(arrival_schedules_the_fixed_local_welcome);
    RUN(magnet_arrival_uses_fixed_welcome_despite_cached_network_voice);
    RUN(arriving_plays_its_message_before_the_fixed_welcome);
    RUN(completed_leave_network_voice_is_not_scheduled_or_played);
    RUN(arrival_network_voice_remains_available);
    RUN(pir_still_requests_the_dedicated_take_key_audio);
    RUN(reminder_dispatch_keeps_type_and_fixed_path_despite_cached_network_voice);
    RUN(rejected_reminder_dispatch_reports_preparation_failure);
    RUN(ordinary_greeting_dispatch_uses_fixed_welcome_and_explicit_network_path);
    RUN(farewell_ignores_cached_network_audio_and_random_variants);
    RUN(chat_dispatch_preserves_commands_and_turns);
    RUN(full_audio_queue_cancels_chat_without_pretending_it_quiesced);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
