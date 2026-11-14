#ifndef HOMEHUB_TUYA_LIGHT_H
#define HOMEHUB_TUYA_LIGHT_H
#include "homehub_core.h"
/* Start once before the background worker. Seed actual debounced inputs;
 * initialization never changes the lamp. Actions run in the app task. */
void hh_tuya_light_start(bool key0, bool key1);
void hh_tuya_light_action(const HH_Action *action);
/* Only the existing background cloud worker may call tick. */
void hh_tuya_light_tick(void);
#endif
