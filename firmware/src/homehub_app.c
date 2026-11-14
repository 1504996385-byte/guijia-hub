#include "homehub_platform.h"
#include "homehub_seed.h"
#include "homehub_chat.h"
#include "homehub_voice.h"
#include "homehub_supabase.h"
#include "homehub_tuya_light.h"
#include "homehub_cloud_http.h"
#include "homehub_event_queue.h"
#include "board_com_api.h"
#include "tkl_gpio.h"
#include "tkl_output.h"
#include "tkl_memory.h"
#include "tal_memory.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

uint32_t hh_boot_id;
static HH_Core core;
static QUEUE_HANDLE core_actions;
static HH_Action ui_state;
static bool pending_greeting;
static uint8_t greeting_recipient;
static HH_Trigger greeting_trigger;
static bool greeting_busy_reported;
static bool network_greeting_pending,use_network_greeting;
static HH_Action network_greeting;
static uint32_t network_greeting_deadline;
typedef struct { bool used; bool downloaded; uint32_t ticket; HH_Action action; } DeferredReceipt;
static DeferredReceipt receipts[HH_MAX_MESSAGES];
static uint32_t next_ticket,last_receipt_retry;

static uint32_t receipt_snapshot(void)
{
    HH_Snapshot *snapshot=tkl_system_psram_malloc(sizeof(*snapshot));
    if(!snapshot)return 0;
    uint32_t ticket=++next_ticket;if(!ticket)ticket=++next_ticket;
    hh_export(&core,snapshot);
    bool accepted=hh_save_snapshot_tagged(snapshot,ticket);
    tkl_system_psram_free(snapshot);return accepted?ticket:0;
}
static bool supabase_id(const char *id)
{
    if(!id||(strncmp(id,"sb_",3)&&strncmp(id,"px_",3))||!id[3])return false;
    for(const char *p=id+3;*p;p++)if(*p<'0'||*p>'9')return false;
    return true;
}
static void defer_receipt_phase(const HH_Action *action,bool downloaded)
{
    if(action->source!=HH_REMOTE)return;
    DeferredReceipt *slot=NULL;
    for(unsigned i=0;i<HH_MAX_MESSAGES;i++){
        if(receipts[i].used&&receipts[i].downloaded==downloaded&&!strcmp(receipts[i].action.id,action->id))return;
        if(!receipts[i].used&&!slot)slot=&receipts[i];
    }
    if(!slot){hh_ui_feedback("保存失败");return;}
    slot->used=true;slot->downloaded=downloaded;slot->action=*action;slot->ticket=receipt_snapshot();
    if(!slot->ticket)hh_ui_feedback("保存失败");
}
static void defer_receipt(const HH_Action *action){defer_receipt_phase(action,false);}
static void defer_download(const HH_Message *message)
{
    HH_Action action={.source=HH_REMOTE,.recipient=message->recipient,.trigger=(HH_Trigger)message->trigger};
    snprintf(action.id,sizeof(action.id),"%s",message->id);
    snprintf(action.audio_path,sizeof(action.audio_path),"%s",message->audio_path);
    defer_receipt_phase(&action,true);
}
static void receipt_commit(uint32_t ticket,bool success)
{
    if(!ticket)return;
    for(unsigned i=0;i<HH_MAX_MESSAGES;i++)if(receipts[i].used&&receipts[i].ticket==ticket){
        if(success){
            HH_Action *a=&receipts[i].action;
            if(receipts[i].downloaded){
                HH_Message m={.source=HH_REMOTE,.recipient=a->recipient,.trigger=(uint8_t)a->trigger};
                snprintf(m.id,sizeof(m.id),"%s",a->id);snprintf(m.audio_path,sizeof(m.audio_path),"%s",a->audio_path);
                hh_supabase_downloaded(&m);
                PR_NOTICE("HH cloud mail durable=1");
            }else if(supabase_id(a->id))hh_supabase_action(a);
            else hh_net_action(a);
            receipts[i].used=false;
        }
        else receipts[i].ticket=0;
    }
}
static void retry_receipts(uint32_t now)
{
    if(now-last_receipt_retry<1000)return;
    last_receipt_retry=now;bool waiting=false;
    for(unsigned i=0;i<HH_MAX_MESSAGES;i++)if(receipts[i].used&&!receipts[i].ticket)waiting=true;
    if(!waiting)return;
    uint32_t ticket=receipt_snapshot();
    if(!ticket){hh_ui_feedback("保存失败");return;}
    for(unsigned i=0;i<HH_MAX_MESSAGES;i++)if(receipts[i].used&&!receipts[i].ticket)receipts[i].ticket=ticket;
}

void hh_post(const HH_Event *event)
{
    /* Workers can wait; the independent input/control task remains runnable. */
    while(!hh_event_try_post(event,1000))PR_ERR("HomeHub completion queue busy; retrying");
}
static void action_callback(void *unused,const HH_Action *action)
{
    (void)unused;
    /* Separate queue avoids a worker completion burst consuming action slots. */
    if(tal_queue_post(core_actions,(void*)action,0)!=OPRT_OK)PR_ERR("Core action invariant violated: queue full");
}
static void gpio_input(TUYA_GPIO_NUM_E pin,bool pullup)
{
    TUYA_GPIO_BASE_CFG_T cfg={.mode=pullup?TUYA_GPIO_PULLUP:TUYA_GPIO_PULLDOWN,
        .direct=TUYA_GPIO_INPUT,.level=TUYA_GPIO_LEVEL_HIGH};
    if(tkl_gpio_init(pin,&cfg)!=OPRT_OK)PR_ERR("Input GPIO init failed: %u",pin);
}
static bool input(TUYA_GPIO_NUM_E pin,bool active_low)
{
    TUYA_GPIO_LEVEL_E level=TUYA_GPIO_LEVEL_HIGH;
    if(tkl_gpio_read(pin,&level)!=OPRT_OK)return false;
    return active_low?level==TUYA_GPIO_LEVEL_LOW:level==TUYA_GPIO_LEVEL_HIGH;
}
static void greeting(const HH_Action *action)
{
    HH_Action job=*action;
    if(action->trigger==HH_LEAVE){
        snprintf(job.audio_path,sizeof(job.audio_path),HH_ROOT "/voices/leave.wav");
        use_network_greeting=false;
    }
    else if(use_network_greeting){snprintf(job.audio_path,sizeof(job.audio_path),"%s",network_greeting.audio_path);use_network_greeting=false;}
    else snprintf(job.audio_path,sizeof(job.audio_path),HH_ROOT "/voices/arrive.wav");
    if(tal_queue_post(hh_audio_jobs,&job,0)!=OPRT_OK)hh_greeting_ready(&core,NULL,false);
}
static const char *feedback(HH_Result result)
{
    switch(result){case HH_BUSY:return "忙碌 请稍后";case HH_TOO_SHORT:return "录音太短";
        case HH_MISSING_ASSET:return "音频缺失";case HH_FULL:return "存储卡未就绪";
        case HH_IO_ERROR:return "录音失败";default:return "请稍后";}
}
static void dispatch(const HH_Action *action)
{
    ui_state=*action;
    if(action->type!=HH_ACT_UI&&action->type!=HH_ACT_PERSIST)
        PR_NOTICE("HH action=%u recipient=%u trigger=%u result=%u turn=%u",(unsigned)action->type,
            action->recipient,(unsigned)action->trigger,(unsigned)action->result,(unsigned)action->chat_turn);
    switch(action->type){
    case HH_ACT_RECORD_START:case HH_ACT_RECORD_STOP_SAVE:case HH_ACT_RECORD_CANCEL:case HH_ACT_PLAY_AUDIO:
        hh_ui_wake();
        if(tal_queue_post(hh_audio_jobs,(void*)action,0)!=OPRT_OK){
            if(action->type==HH_ACT_RECORD_START)hh_recording_started(&core,false);
            else if(action->type==HH_ACT_PLAY_AUDIO)hh_playback_finished(&core,action->id,false);
            else if(action->type==HH_ACT_RECORD_STOP_SAVE)hh_recording_saved(&core,action->id,NULL,false);
            else if(action->type==HH_ACT_RECORD_CANCEL)hh_audio_cancel();
        }break;
    case HH_ACT_PERSIST:{
        HH_Snapshot *snapshot=tkl_system_psram_malloc(sizeof(*snapshot));
        if(snapshot){hh_export(&core,snapshot);if(!hh_save_snapshot_async(snapshot))hh_ui_feedback("保存失败");tkl_system_psram_free(snapshot);}
        else hh_ui_feedback("保存失败");
        break;
    }
    case HH_ACT_KEY_EVENT:
        hh_ui_wake();
        hh_net_action(action);
        hh_supabase_action(action);hh_tuya_light_action(action);
        /* The core drains addressed messages first; this deferred greeting
         * follows them without interrupting recording/chat or message audio. */
        if(action->trigger==HH_ARRIVE||action->trigger==HH_LEAVE){
            pending_greeting=true;greeting_recipient=action->recipient;
            greeting_trigger=action->trigger;greeting_busy_reported=false;
        }
        break;
    case HH_ACT_PLAYED_RECEIPT:defer_receipt(action);break;
    case HH_ACT_CHAT_START:case HH_ACT_CHAT_SUBMIT:case HH_ACT_CHAT_STOP:
        hh_ui_wake();
        if(tal_queue_post(hh_audio_jobs,(void*)action,0)!=OPRT_OK){
            hh_chat_cloud_cancel(action->chat_turn);
            hh_chat_report(&core,action->chat_turn,HH_CHAT_FAILED,false,(uint32_t)tal_system_get_millisecond());
        }
        break;
    case HH_ACT_GREETING_REQUEST:greeting(action);break;
    case HH_ACT_KEY_REMINDER_REQUEST:{
        HH_Action job=*action;
        snprintf(job.audio_path,sizeof(job.audio_path),HH_ROOT "/voices/take_key.wav");
        if(tal_queue_post(hh_audio_jobs,&job,0)!=OPRT_OK)hh_greeting_ready(&core,NULL,false);
        break;
    }
    case HH_ACT_WAKE_SCREEN:hh_ui_wake();break;
    case HH_ACT_FEEDBACK:
        if(core.audio_state>=HH_AUDIO_CHAT_CAPTURE&&core.audio_state<=HH_AUDIO_CHAT_STOPPING&&action->result==HH_IO_ERROR)
            hh_ui_feedback("聊天失败 正在停止");
        else if(core.audio_state==HH_AUDIO_IDLE&&action->result==HH_BUSY&&hh_chat_cloud_availability()!=HH_CHAT_READY){
            char status[96];hh_chat_cloud_status(status,sizeof(status));hh_ui_feedback(status);
        }
        else hh_ui_feedback(feedback(action->result));
        break;
    case HH_ACT_UI:ui_state=*action;break;
    default:break;
    }
    hh_ui_update(&ui_state,core.audio_state);
}
static void handle(const HH_Event *event)
{
    if(event->type==EV_RECORD_STARTED||event->type==EV_RECORD_SAVED||event->type==EV_PLAY_FINISHED||
       event->type==EV_GREETING_READY||event->type==EV_SNAPSHOT_SAVED||event->type==EV_CHAT_REPORT)
        PR_NOTICE("HH completion=%u ok=%u turn=%u",(unsigned)event->type,event->ok,(unsigned)event->action.chat_turn);
    switch(event->type){
    case EV_ACTION:dispatch(&event->action);break;
    case EV_RECORD_STARTED:hh_recording_started(&core,event->ok);if(!event->ok)hh_ui_feedback("录音失败");break;
    case EV_RECORD_SAVED:
        hh_recording_saved(&core,event->action.id,event->action.audio_path,event->ok);
        hh_ui_feedback(event->ok?"正在保存":"录音失败");break;
    case EV_PLAY_FINISHED:hh_playback_finished(&core,event->action.id,event->ok);if(!event->ok)hh_ui_feedback("播放失败");break;
    case EV_REMOTE_MESSAGE:{HH_Result result=hh_add_message(&core,&event->message);
        if(result==HH_OK||result==HH_DUPLICATE){
            if(supabase_id(event->message.id))defer_download(&event->message);
            else hh_net_downloaded(&event->message);
        }
        break;}
    case EV_SET_TRIGGER:
        if(core.audio_state==HH_AUDIO_RECORDING&&core.record_recipient==event->action.recipient&&event->trigger>=0)
            hh_set_record_trigger(&core,(HH_Trigger)event->trigger);
        else hh_set_next_trigger(&core,event->action.recipient,event->trigger);
        hh_ui_feedback("已选 点击切换时机");break;
    case EV_SNAPSHOT_SAVED:
        receipt_commit(event->action.event_seq,event->ok);
        if(!event->ok)hh_ui_feedback("保存失败");else hh_ui_feedback("已保存");break;
    case EV_GREETING_READY:hh_greeting_ready(&core,event->action.audio_path,event->ok);break;
    case EV_NETWORK_INFO:
        hh_ui_network(event->text);
        if(event->ok&&event->action.trigger==HH_ARRIVE){network_greeting=event->action;network_greeting_pending=true;
            network_greeting_deadline=(uint32_t)tal_system_get_millisecond()+60000;}
        break;
    case EV_CHAT_REPORT:
        hh_chat_report(&core,event->action.chat_turn,event->chat_event,event->ok,(uint32_t)tal_system_get_millisecond());
        break;
    case EV_CHAT_EMOTION:
        /* A queued reply must not repaint a canceled or newer conversation. */
        if(event->action.chat_turn && event->action.chat_turn==core.chat_turn &&
           (core.audio_state==HH_AUDIO_CHAT_WAIT || core.audio_state==HH_AUDIO_CHAT_PLAY) &&
           event->emotion<HH_EMOTION_COUNT){
            PR_NOTICE("HH chat emotion turn=%u value=%u",(unsigned)event->action.chat_turn,(unsigned)event->emotion);
            hh_ui_chat_emotion(event->action.chat_turn,(HH_Emotion)event->emotion);
        }
        break;
    case EV_CHAT_TEXT:
        if(event->action.chat_turn==core.chat_turn&&core.audio_state>=HH_AUDIO_CHAT_CAPTURE&&core.audio_state<=HH_AUDIO_CHAT_STOPPING)
            hh_ui_chat_text(event->chat_is_user_text,event->text);
        break;
    default:break;
    }
}
static void app_thread(void *unused)
{
    (void)unused;tal_log_init(TAL_LOG_LEVEL_NOTICE,1024,(TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("HomeHub 1.0.0 | board=%s | SDK=%s | build=%s %s",PLATFORM_BOARD,OPEN_VERSION,__DATE__,__TIME__);
    if(!hh_event_queue_init() || tal_queue_create_init(&core_actions,sizeof(HH_Action),64)!=OPRT_OK){
        PR_ERR("HomeHub event queue initialization failed");return;
    }
    PR_NOTICE("HH event queue bytes=%u slots=64",(unsigned)sizeof(HH_Event));
    PR_NOTICE("HH event pool psram_bytes=%u native_pointer_bytes=%u",(unsigned)(64u*sizeof(HH_Event)),(unsigned)(128u*sizeof(void*)));
    if(board_register_hardware()!=OPRT_OK)PR_ERR("Board registration failed");
    hh_storage_init();
    if(hh_storage_ready)hh_seed_voices();
    bool config_loaded=hh_load_config();
    /* Restore the owner's doorway detection mode; periodic speech is off. */
    hh_device.pir_enabled=true;
    PR_NOTICE("HH config loaded=%u sd=%u pir47=%u chat12=%u volume=%u",config_loaded,
        hh_storage_ready,hh_device.pir_enabled,hh_device.chat_enabled,hh_device.volume);
    PR_NOTICE("HH inputs: reed14/15, record16/17 and PIR47 HIGH active; PIR warmup=10s quiet=30s");
    PR_NOTICE("HH reminder policy: leave=messages-then-fixed-farewell; timed-reminder=off; key-reminder=PIR (keys required)");
    /* Confirmed on the assembled modules: magnet present drives D0 HIGH. */
    gpio_input(TUYA_GPIO_NUM_14,false);gpio_input(TUYA_GPIO_NUM_15,false);
    /* Real three-pin OUT modules: HIGH while held, LOW when released. */
    gpio_input(TUYA_GPIO_NUM_16,false);gpio_input(TUYA_GPIO_NUM_17,false);
    gpio_input(TUYA_GPIO_NUM_12,true);
    if(hh_device.pir_enabled)gpio_input(TUYA_GPIO_NUM_47,false);
    hh_ui_init();
    bool voice_requested=hh_device.chat_enabled&&hh_voice_init();
    PR_NOTICE("HH voice setup requested=%u (recognition requires live wake test)",(unsigned)voice_requested);
    bool audio_ok=hh_audio_init();if(!audio_ok)hh_ui_feedback("音频缺失");
    THREAD_HANDLE audio_thread=NULL;THREAD_CFG_T cfg={.stackDepth=6144,.priority=THREAD_PRIO_2,.thrdname="hh_audio"};
    tal_thread_create_and_start(&audio_thread,NULL,NULL,hh_audio_worker,NULL,&cfg);
    hh_boot_id=(uint32_t)tal_system_get_random(0xffffffffu);
    HH_Config settings=hh_default_config();settings.boot_id=hh_boot_id;settings.pir_enabled=hh_device.pir_enabled;
    settings.reminder_interval_ms=0u;
    hh_init(&core,&settings,action_callback,NULL,(uint32_t)tal_system_get_millisecond());
    HH_Snapshot *snapshot=tkl_system_psram_malloc(sizeof(*snapshot));
    if(snapshot){if(hh_load_snapshot(snapshot)&&hh_restore(&core,snapshot)!=HH_OK)PR_ERR("Snapshot rejected");tkl_system_psram_free(snapshot);}
    hh_chat_cloud_start();
    hh_net_start();
    bool cloud_started=false;
    unsigned last_inputs=0xffffffffu;
    HH_AudioState last_audio=HH_AUDIO_IDLE;
    HH_ChatAvailability last_chat=HH_CHAT_DISABLED;
    uint32_t heap_check_ms=0;
    int lowest_heap=0x7fffffff;
    while(1){
        uint32_t now=(uint32_t)tal_system_get_millisecond();
        bool keys[]={input(TUYA_GPIO_NUM_14,false),input(TUYA_GPIO_NUM_15,false)};
        bool buttons[]={input(TUYA_GPIO_NUM_16,false),input(TUYA_GPIO_NUM_17,false)};
        bool chat_key=input(TUYA_GPIO_NUM_12,true);
        bool pir=hh_device.pir_enabled&&input(TUYA_GPIO_NUM_47,false);
        HH_ChatAvailability chat=hh_chat_cloud_availability();
        hh_chat_set_availability(&core,chat);
        hh_sample_with_chat(&core,now,keys,buttons,pir,chat_key);
        uint32_t mic_sequence=0,mic_last_ms=0;
        bool mic_ready=hh_audio_voice_progress(&mic_sequence,&mic_last_ms);
        hh_voice_tick(&core,(uint32_t)tal_system_get_millisecond(),mic_ready,mic_sequence,mic_last_ms);
        hh_cloud_http_background_idle(core.audio_state==HH_AUDIO_IDLE&&!buttons[0]&&!buttons[1]&&!chat_key);
        if(!cloud_started&&core.keys[0].initialized&&core.keys[1].initialized&&
           core.keys[0].stable==core.keys[0].candidate&&core.keys[1].stable==core.keys[1].candidate&&
           (uint32_t)(now-core.keys[0].candidate_since)>=core.config.debounce_ms&&
           (uint32_t)(now-core.keys[1].candidate_since)>=core.config.debounce_ms){
            hh_tuya_light_start(core.keys[0].stable,core.keys[1].stable);
            hh_supabase_start();cloud_started=true;
        }
        unsigned inputs=core.keys[0].stable|(core.keys[1].stable<<1)|(core.buttons[0].stable<<2)|
            (core.buttons[1].stable<<3)|(pir<<4)|(core.chat_button.stable<<5);
        if(inputs!=last_inputs){
            PR_NOTICE("HH inputs reed14=%u reed15=%u rec16=%u rec17=%u pir47=%u chat12=%u (1=active)",
                core.keys[0].stable,core.keys[1].stable,core.buttons[0].stable,core.buttons[1].stable,pir,core.chat_button.stable);
            last_inputs=inputs;
        }
        if(core.audio_state!=last_audio){PR_NOTICE("HH audio=%u",(unsigned)core.audio_state);last_audio=core.audio_state;}
        if(chat!=last_chat){PR_NOTICE("HH chat availability=%u",(unsigned)chat);last_chat=chat;}
        if(now-heap_check_ms>=5000u){
            heap_check_ms=now;int free_heap=tal_system_get_free_heap_size();
            if(free_heap<lowest_heap){lowest_heap=free_heap;PR_NOTICE("HH heap native=%d",free_heap);}
        }
        HH_Action action;
        for(unsigned i=0;i<64&&tal_queue_fetch(core_actions,&action,0)==OPRT_OK;i++)dispatch(&action);
        HH_Event event;
        for(unsigned i=0;i<24&&hh_event_fetch(&event,0);i++){
            handle(&event);
            for(unsigned j=0;j<32&&tal_queue_fetch(core_actions,&action,0)==OPRT_OK;j++)dispatch(&action);
        }
        if(pending_greeting&&core.audio_state==HH_AUDIO_IDLE&&!buttons[0]&&!buttons[1]&&!chat_key){
            /* This is a local key action. Only the independent network branch
             * below may select cached network audio instead of the fixed WAV. */
            use_network_greeting=false;
            if(hh_request_greeting(&core,greeting_recipient,greeting_trigger)==HH_OK)pending_greeting=false;
            else if(!greeting_busy_reported)greeting_busy_reported=true;
        }
        if(network_greeting_pending&&(int32_t)(now-network_greeting_deadline)>=0)network_greeting_pending=false;
        if(!pending_greeting&&network_greeting_pending&&core.audio_state==HH_AUDIO_IDLE&&!buttons[0]&&!buttons[1]&&!chat_key){
            use_network_greeting=true;
            if(hh_request_greeting(&core,network_greeting.recipient,network_greeting.trigger)==HH_OK)network_greeting_pending=false;
            else use_network_greeting=false;
        }
        retry_receipts(now);
        hh_cloud_http_background_idle(core.audio_state==HH_AUDIO_IDLE&&!buttons[0]&&!buttons[1]&&!chat_key);
        hh_ui_tick(core.audio_state,now);tal_system_sleep(10);
    }
}
void tuya_app_main(void)
{
    /* Install once before app/worker threads or JSON objects exist. SDK AI
     * prints JSON then frees it through tal_memory.h's matching heap. */
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn=tal_psram_malloc,.free_fn=tal_psram_free});
#else
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn=tal_malloc,.free_fn=tal_free});
#endif
    THREAD_HANDLE thread=NULL;THREAD_CFG_T cfg={.stackDepth=8192,.priority=THREAD_PRIO_2,.thrdname="homehub"};
    tal_thread_create_and_start(&thread,NULL,NULL,app_thread,NULL,&cfg);
}
