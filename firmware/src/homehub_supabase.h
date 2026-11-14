#ifndef HOMEHUB_SUPABASE_H
#define HOMEHUB_SUPABASE_H
#include "homehub_core.h"
bool hh_supabase_enabled(void);
void hh_supabase_start(void);
/* Key-event logs are best effort; repeated POSTs are not an exactly-once API.
 * No unsupported played column is invented for this Supabase schema. */
void hh_supabase_action(const HH_Action *action);
/* Application calls ONLY after the matching durable snapshot acknowledgement.
 * An audio download or event submission alone must never call this. */
void hh_supabase_downloaded(const HH_Message *message);
#endif
