#ifndef HOMEHUB_CLOUD_CONFIG_H
#define HOMEHUB_CLOUD_CONFIG_H
#include <stdbool.h>
typedef struct {
    const char *supabase_host,*supabase_key,*supabase_ca;
    const char *tuya_host,*tuya_client_id,*tuya_secret,*tuya_device_id,*tuya_ca;
    const char *tuya_colour_code;
    const char *fallback_ssid,*fallback_password;
    bool supabase_plain_http;
    unsigned short supabase_port;
} HH_CloudSettings;
/* NULL in a public checkout without the ignored local configuration. */
const HH_CloudSettings *hh_cloud_settings(void);
#endif
