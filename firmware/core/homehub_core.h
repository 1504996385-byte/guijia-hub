#ifndef HOMEHUB_CORE_H
#define HOMEHUB_CORE_H

/* Portable C99 state machine. Call only from one application task. The callback
 * must enqueue work; it must not call back into this core. All input booleans
 * are logical (true = key present / button held / PIR active), not pin levels. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HH_MAX_MESSAGES 32
#define HH_ID_MAX 48
#define HH_PATH_MAX 191
#define HH_SEEN_IDS 64
#define HH_SNAPSHOT_VERSION 1u
#define HH_TRIGGER_AUTO (-1)
#define HH_KEY_REMINDER_WAIT_MS 10000u
#define HH_CHAT_REPLY_WAIT_MS 45000u
#define HH_CHAT_STOP_NOTICE_MS 3000u
#define HH_CHAT_CAPTURE_MIN_MS 250u
#define HH_CHAT_CAPTURE_MAX_MS 30000u
#define HH_CHAT_VOICE_NO_SPEECH_MS 8000u

/* ANY is reserved for remote messages, played at the next edge of that slot. */
typedef enum { HH_ARRIVE = 0, HH_LEAVE = 1, HH_ANY = 2 } HH_Trigger;
typedef enum { HH_LOCAL = 0, HH_REMOTE = 1 } HH_Source;
typedef enum { HH_PENDING = 0, HH_QUEUED, HH_PLAYING, HH_PLAYED } HH_MessageState;
typedef enum { HH_AUDIO_IDLE = 0, HH_AUDIO_RECORDING, HH_AUDIO_SAVING,
               HH_AUDIO_PLAYING, HH_AUDIO_GREETING, HH_AUDIO_CHAT_CAPTURE,
               HH_AUDIO_CHAT_WAIT, HH_AUDIO_CHAT_PLAY, HH_AUDIO_CHAT_STOPPING } HH_AudioState;
typedef enum { HH_CHAT_DISABLED = 0, HH_CHAT_UNAUTHORIZED, HH_CHAT_OFFLINE,
               HH_CHAT_CONNECTING, HH_CHAT_READY } HH_ChatAvailability;
typedef enum { HH_CHAT_STARTED = 0, HH_CHAT_TTS_STARTED, HH_CHAT_FAILED,
               HH_CHAT_QUIESCED } HH_ChatEvent;
typedef enum { HH_OK = 0, HH_DUPLICATE, HH_FULL, HH_INVALID, HH_BUSY,
               HH_NOT_FOUND, HH_IO_ERROR, HH_TOO_SHORT, HH_MISSING_ASSET } HH_Result;

typedef struct {
    char id[HH_ID_MAX + 1];
    char audio_path[HH_PATH_MAX + 1];
    uint8_t recipient;
    uint8_t trigger;
    uint8_t state;
    uint8_t source;
} HH_Message;

/* Persist this versioned data in a checksummed/atomic platform envelope. Core
 * export demotes queued/playing to pending; restore never invents a key edge. */
typedef struct {
    uint32_t version;
    HH_Message messages[HH_MAX_MESSAGES];
    uint8_t used[HH_MAX_MESSAGES];
    char seen_ids[HH_SEEN_IDS][HH_ID_MAX + 1];
    uint16_t seen_next;
    uint16_t seen_count;
} HH_Snapshot;

typedef enum {
    HH_ACT_UI = 0,
    HH_ACT_KEY_EVENT,
    HH_ACT_RECORD_START,
    HH_ACT_RECORD_STOP_SAVE,
    HH_ACT_RECORD_CANCEL,
    HH_ACT_PLAY_AUDIO,
    HH_ACT_PERSIST,
    HH_ACT_PLAYED_RECEIPT,
    HH_ACT_WAKE_SCREEN,
    HH_ACT_GREETING_REQUEST,
    HH_ACT_FEEDBACK,
    /* Anonymous PIR/timer reminder: resolve voices/take_key.wav, never mail. */
    HH_ACT_KEY_REMINDER_REQUEST,
    HH_ACT_CHAT_START,
    HH_ACT_CHAT_SUBMIT,
    HH_ACT_CHAT_STOP
} HH_ActionType;

typedef struct {
    HH_ActionType type;
    uint8_t recipient;
    HH_Source source;
    HH_Trigger trigger;
    HH_Result result;
    uint32_t event_seq;
    uint32_t chat_turn;
    char id[HH_ID_MAX + 1];
    char audio_path[HH_PATH_MAX + 1];
    bool keys_present[2];
    uint8_t pending_count[2];
} HH_Action;

typedef void (*HH_ActionFn)(void *context, const HH_Action *action);
typedef struct {
    uint32_t debounce_ms;       /* default 60 */
    uint32_t minimum_record_ms; /* default 250 */
    uint32_t maximum_record_ms; /* default 30000, clamped to 30000 */
    uint32_t boot_id;           /* platform supplies a unique per-boot value */
    bool pir_enabled;          /* default false */
    uint32_t pir_warmup_ms;     /* default 10000 */
    uint32_t pir_quiet_ms;      /* default 30000 */
    uint32_t reminder_interval_ms; /* 0 disables periodic anonymous reminders */
} HH_Config;

typedef struct {
    bool initialized, stable, candidate;
    uint32_t candidate_since;
} HH_Debouncer;

typedef struct {
    HH_Config config;
    HH_ActionFn action;
    void *context;
    HH_Snapshot store;
    HH_Debouncer keys[2], buttons[2];
    HH_Debouncer chat_button;
    HH_ChatAvailability chat_availability;
    HH_AudioState audio_state;
    uint32_t boot_ms, now_ms, event_seq, recording_seq, record_start_ms;
    uint32_t last_pir_ms;
    uint32_t key_reminder_since_ms;
    uint32_t key_reminder_prepared_since_ms;
    uint32_t timed_reminder_last_ms;
    bool pir_announced, pir_previous, record_button_released, greeting_announced;
    bool key_reminder_pending, greeting_is_key_reminder;
    bool key_reminder_preparation_cancelled;
    bool pir_wait_expired; /* A cancelled busy HIGH episode needs a real LOW. */
    bool greeting_is_timed_reminder;
    int8_t next_trigger[2];
    uint8_t record_recipient;
    HH_Trigger record_trigger;
    uint8_t greeting_recipient;
    HH_Trigger greeting_trigger;
    char record_id[HH_ID_MAX + 1];
    int playing_index;
    uint8_t play_queue[HH_MAX_MESSAGES];
    uint8_t queue_head, queue_count;
    uint32_t chat_turn, chat_sequence, chat_started_ms, chat_wait_since_ms, chat_stop_since_ms;
    int8_t chat_local_recipient;
    bool chat_stop_notice_sent, chat_start_confirmed, chat_key_requires_release;
    bool chat_from_voice, chat_voice_speech;
} HH_Core;

HH_Config hh_default_config(void);
void hh_init(HH_Core *core, const HH_Config *config, HH_ActionFn action,
             void *context, uint32_t now_ms);
/* Invoke regularly, e.g. every 10 ms. Handles timeout without blocking. */
void hh_sample(HH_Core *core, uint32_t now_ms, const bool keys_present[2],
               const bool buttons_held[2], bool pir_active);
/* Atomic input arbitration: local raw buttons take precedence over board KEY.
 * All methods, including reports from workers, run only in the application task. */
void hh_sample_with_chat(HH_Core *core, uint32_t now_ms, const bool keys_present[2],
                         const bool buttons_held[2], bool pir_active, bool chat_key_held);
void hh_chat_set_availability(HH_Core *core, HH_ChatAvailability availability);
/* Call after sampling real inputs. A busy/unavailable wake is rejected quietly;
 * it never changes physical KEY state or preempts local audio/mail. */
HH_Result hh_chat_voice_wake(HH_Core *core, uint32_t now_ms);
/* Fresh PCM activity only, tagged with its original turn. The platform VAD
 * debounces silence; false after speech finishes this voice capture once. */
HH_Result hh_chat_voice_activity(HH_Core *core, uint32_t turn, bool speech,
                                 uint32_t now_ms);
/* Every asynchronous report must carry the original nonzero turn. Failed
 * quiescence retains CHAT_STOPPING; timeout never pretends audio is released. */
HH_Result hh_chat_report(HH_Core *core, uint32_t turn, HH_ChatEvent event,
                         bool success, uint32_t now_ms);
HH_Result hh_set_record_trigger(HH_Core *core, HH_Trigger trigger);
/* One-shot preselection for the next recording of this recipient. AUTO restores
 * slot-derived default. Consumed when recording starts, not on a busy press. */
HH_Result hh_set_next_trigger(HH_Core *core, unsigned recipient, int trigger);
/* Call after the recorder opened successfully or failed; false aborts safely. */
void hh_recording_started(HH_Core *core, bool success);
/* Path must identify a fully saved/validated WAV. Only accepted in SAVING. */
HH_Result hh_recording_saved(HH_Core *core, const char *id,
                             const char *audio_path, bool success);
/* Only add after durable audio download and format/hash validation. */
HH_Result hh_add_message(HH_Core *core, const HH_Message *message);
/* success=true only on natural complete playback; stopped/error=false. */
HH_Result hh_playback_finished(HH_Core *core, const char *id, bool success);
/* Low-priority greeting requested by a board key-event handler. Does not alter
 * PIR speech quiet time. Returns BUSY without emitting when audio/queue active. */
HH_Result hh_request_greeting(HH_Core *core, unsigned recipient, HH_Trigger trigger);
/* Resolve either greeting request using its original action type. PIR key
 * reminders recheck stable keys; periodic reminders do not require keys.
 * available=false reports missing material; playback uses id="greeting". */
HH_Result hh_greeting_ready(HH_Core *core, const char *audio_path, bool available);
void hh_export(const HH_Core *core, HH_Snapshot *snapshot);
HH_Result hh_restore(HH_Core *core, const HH_Snapshot *snapshot);
size_t hh_pending_count(const HH_Core *core, unsigned recipient);
const HH_Message *hh_find_message(const HH_Core *core, const char *id);

#endif
