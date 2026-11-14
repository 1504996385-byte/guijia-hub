#ifndef HOMEHUB_VOICE_H
#define HOMEHUB_VOICE_H
#include "homehub_core.h"

/* Request the pinned T5 offline KWS/RNN pipeline once, before codec open.
 * A successful request does not prove asynchronous model/acoustic readiness. */
bool hh_voice_init(void);
/* Application-thread only, after raw input sampling. PCM progress comes from
 * the existing sole microphone callback, never a second audio owner. */
void hh_voice_tick(HH_Core *core,uint32_t now,bool audio_ready,
                   uint32_t mic_sequence,uint32_t mic_last_ms);
#endif
