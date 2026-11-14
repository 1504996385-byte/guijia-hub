#include "homehub_event_queue.h"
#include "tal_queue.h"
#include "tkl_memory.h"
#include <string.h>

#define HH_EVENT_SLOTS 64u

static struct {
    HH_Event *pool;
    QUEUE_HANDLE free_slots, pending;
    bool ready;
} event_queue;

/* Only used during initialization, before any producers or consumer start.
 * Delete queues first: even a partially seeded queue still refers to the pool. */
static void init_cleanup(void)
{
    if (event_queue.pending) tal_queue_free(event_queue.pending);
    if (event_queue.free_slots) tal_queue_free(event_queue.free_slots);
    if (event_queue.pool) tkl_system_psram_free(event_queue.pool);
    memset(&event_queue, 0, sizeof(event_queue));
}

bool hh_event_queue_init(void)
{
    if (event_queue.ready) return true;
    event_queue.pool = tkl_system_psram_malloc(HH_EVENT_SLOTS * sizeof(HH_Event));
    if (!event_queue.pool) return false;
    if (tal_queue_create_init(&event_queue.free_slots, sizeof(HH_Event *), HH_EVENT_SLOTS) != OPRT_OK ||
        tal_queue_create_init(&event_queue.pending, sizeof(HH_Event *), HH_EVENT_SLOTS) != OPRT_OK) {
        init_cleanup();
        return false;
    }
    for (unsigned i = 0; i < HH_EVENT_SLOTS; ++i) {
        HH_Event *slot = &event_queue.pool[i];
        if (tal_queue_post(event_queue.free_slots, &slot, 0) != OPRT_OK) {
            init_cleanup();
            return false;
        }
    }
    event_queue.ready = true;
    PR_NOTICE("HH events slots=%u pool_bytes=%u pointer_queue_bytes=%u", HH_EVENT_SLOTS,
              (unsigned)(HH_EVENT_SLOTS * sizeof(HH_Event)),
              (unsigned)(2u * HH_EVENT_SLOTS * sizeof(HH_Event *)));
    return true;
}

static void return_slot(HH_Event *slot)
{
    /* Owning this slot guarantees a spare entry in free_slots. In normal RTOS
     * operation this cannot block. Retain ownership across a transient SDK
     * error rather than leaking a slot or reporting a delivered event twice. */
    while (tal_queue_post(event_queue.free_slots, &slot, QUEUE_WAIT_FOREVER) != OPRT_OK) {}
}

bool hh_event_try_post(const HH_Event *event, uint32_t timeout)
{
    HH_Event *slot;
    if (!event_queue.ready || !event) return false;
    if (tal_queue_fetch(event_queue.free_slots, &slot, timeout) != OPRT_OK) return false;
    memcpy(slot, event, sizeof(*slot));
    if (tal_queue_post(event_queue.pending, &slot, 0) != OPRT_OK) {
        return_slot(slot);
        return false;
    }
    return true;
}

bool hh_event_fetch(HH_Event *event, uint32_t timeout)
{
    HH_Event *slot;
    if (!event_queue.ready || !event) return false;
    if (tal_queue_fetch(event_queue.pending, &slot, timeout) != OPRT_OK) return false;
    memcpy(event, slot, sizeof(*event));
    return_slot(slot);
    return true;
}
