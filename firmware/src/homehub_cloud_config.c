#include "homehub_cloud_config.h"
#include <stddef.h>
#if defined(HH_CLOUD_PRIVATE_CONFIG) && HH_CLOUD_PRIVATE_CONFIG == 1
#include "homehub_cloud_private.h"
#ifndef HH_SB_PLAIN_HTTP
#define HH_SB_PLAIN_HTTP 0
#endif
#ifndef HH_SB_PORT
#define HH_SB_PORT 0
#endif
static const HH_CloudSettings settings={
    .supabase_host=HH_SB_HOST,.supabase_key=HH_SB_KEY,.supabase_ca=HH_SB_CA,
    .tuya_host=HH_TUYA_HOST,.tuya_client_id=HH_TUYA_CLIENT_ID,
    .tuya_secret=HH_TUYA_SECRET,.tuya_device_id=HH_TUYA_DEVICE_ID,.tuya_ca=HH_TUYA_CA,
    .tuya_colour_code=HH_TUYA_COLOUR_CODE,
    .fallback_ssid=HH_FALLBACK_SSID,.fallback_password=HH_FALLBACK_PASSWORD,
    .supabase_plain_http=HH_SB_PLAIN_HTTP,.supabase_port=HH_SB_PORT
};
#endif
const HH_CloudSettings *hh_cloud_settings(void)
{
#if defined(HH_CLOUD_PRIVATE_CONFIG) && HH_CLOUD_PRIVATE_CONFIG == 1
    return &settings;
#else
    return NULL;
#endif
}
