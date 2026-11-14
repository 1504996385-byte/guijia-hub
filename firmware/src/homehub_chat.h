#ifndef HOMEHUB_CHAT_H
#define HOMEHUB_CHAT_H
#include "homehub_core.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Backend: all command entrypoints below return promptly; the microphone path
 * must only copy bytes, never wait on cloud, filesystem or speaker drain. */
void hh_chat_cloud_start(void);
HH_ChatAvailability hh_chat_cloud_availability(void);
void hh_chat_cloud_status(char *out,size_t capacity);
bool hh_chat_network_ready(void);
bool hh_chat_cloud_begin(uint32_t turn);
bool hh_chat_cloud_pcm(uint32_t turn,const uint8_t *pcm,uint32_t bytes);
bool hh_chat_cloud_submit(uint32_t turn);
void hh_chat_cloud_cancel(uint32_t turn);

/* Called by the existing audio worker for CHAT actions; it alone opens the
 * codec. A backend cancellation must never unlock the next local job before
 * its player is stopped and this owner has quiesced. */
void hh_audio_chat_action(const HH_Action *action);
bool hh_audio_chat_sink_begin(uint32_t turn);
bool hh_audio_chat_sink_write(uint32_t turn,const uint8_t *pcm,uint32_t bytes);
void hh_audio_chat_sink_stop(uint32_t turn);
bool hh_audio_chat_quiesce(uint32_t turn,bool natural_end);

/* Snapshot real PCM progress for VAD freshness; false until valid PCM arrives,
 * or while local recording/playback still owns the audio hardware. */
bool hh_audio_voice_progress(uint32_t *sequence,uint32_t *last_frame_ms);

#endif
