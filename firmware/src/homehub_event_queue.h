#ifndef HOMEHUB_EVENT_QUEUE_H
#define HOMEHUB_EVENT_QUEUE_H
#include "homehub_platform.h"

/* Initialize once before event producers/consumer start. A failed attempt
 * releases every resource and may be retried; successful init is idempotent.
 * TASK CONTEXT ONLY: these copy complete events and must never run in an ISR. */
bool hh_event_queue_init(void);
/* timeout applies to acquiring an available slot / pending event. Event data
 * is copied into an owned preallocated slot; no per-event allocation occurs. */
bool hh_event_try_post(const HH_Event *event,uint32_t timeout);
/* Copies the entire event into caller storage before returning its pool slot. */
bool hh_event_fetch(HH_Event *event,uint32_t timeout);
#endif
