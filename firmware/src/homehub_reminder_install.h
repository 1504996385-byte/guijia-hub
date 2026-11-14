#ifndef HOMEHUB_REMINDER_INSTALL_H
#define HOMEHUB_REMINDER_INSTALL_H
#include <stdbool.h>

/* Startup only, after TF mount and before audio/network consumers. Each
 * installer independently preserves a verified old-version backup. */
bool hh_reminder_install(void);
bool hh_departure_install(void);
bool hh_arrival_install(void);
#endif
