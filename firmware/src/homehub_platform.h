#ifndef HOMEHUB_PLATFORM_H
#define HOMEHUB_PLATFORM_H
#include "homehub_core.h"
#include "homehub_emotion.h"
#include "tal_api.h"
#include "tkl_fs.h"
#define HH_ROOT "/sdcard/homehub"
#define HH_WAV_MAX (960000u + 44u)
typedef struct {
    char wifi_ssid[33], wifi_password[65];
    char server_url[192], device_token[129];
    char tuya_product_id[33];
    bool chat_enabled;
    bool pir_enabled;
    uint8_t volume;
} HH_DeviceConfig;
typedef enum { EV_ACTION, EV_RECORD_STARTED, EV_RECORD_SAVED, EV_PLAY_FINISHED,
               EV_REMOTE_MESSAGE, EV_SET_TRIGGER, EV_SNAPSHOT_SAVED, EV_GREETING_READY, EV_NETWORK_INFO,
               EV_CHAT_REPORT, EV_CHAT_TEXT, EV_CHAT_EMOTION } HH_EventType;
typedef struct {
    HH_EventType type;
    /* EV_REMOTE_MESSAGE carries a message; all other events carry an action.
     * Sharing these mutually exclusive payloads saves 15 KiB in the 64-slot
     * native RTOS queue. Text and status stay separate: network greetings
     * and chat replies need them alongside the action. */
    union {
        HH_Action action;
        HH_Message message;
    };
    bool ok;
    int trigger;
    HH_ChatEvent chat_event;
    bool chat_is_user_text;
    uint8_t emotion;
    char text[192];
} HH_Event;
extern HH_DeviceConfig hh_device;
extern QUEUE_HANDLE hh_audio_jobs;
extern MUTEX_HANDLE hh_fs_mutex;
extern bool hh_storage_ready;
extern volatile bool hh_network_ready;
extern uint32_t hh_boot_id;
void hh_post(const HH_Event *event);
bool hh_storage_init(void);
bool hh_load_config(void);
bool hh_load_snapshot(HH_Snapshot *snapshot);
bool hh_save_snapshot_async(const HH_Snapshot *snapshot);
bool hh_save_snapshot_tagged(const HH_Snapshot *snapshot,uint32_t ticket);
bool hh_file_read(const char *path, uint8_t **data, uint32_t *size, uint32_t max_size);
bool hh_file_write_atomic(const char *path, const uint8_t *data, uint32_t size);
bool hh_wav_validate(const uint8_t *data, uint32_t size, uint32_t *pcm_size);
void hh_wav_header(uint8_t header[44], uint32_t pcm_size);
void hh_audio_worker(void *arg);
bool hh_audio_init(void);
void hh_ui_init(void);
void hh_ui_update(const HH_Action *action, HH_AudioState state);
void hh_ui_feedback(const char *text);
void hh_ui_tick(HH_AudioState state, uint32_t now);
void hh_ui_network(const char *text);
void hh_ui_chat_text(bool user_text,const char *text);
void hh_ui_chat_emotion(uint32_t turn,HH_Emotion emotion);
void hh_ui_wake(void);
void hh_audio_cancel(void);
void hh_net_start(void);
void hh_net_action(const HH_Action *action);
void hh_net_downloaded(const HH_Message *message);
bool hh_valid_id(const char *id);
uint32_t hh_checksum(const void *data, size_t len);
#endif
