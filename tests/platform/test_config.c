/* Actual storage/configuration source, host TKL, and the SDK's real cJSON. */
#include <cJSON.h>
#define HH_HOST_CJSON_H
#define cJSON_Parse config_inert_Parse
#define cJSON_GetObjectItemCaseSensitive config_inert_GetObjectItemCaseSensitive
#define cJSON_IsString config_inert_IsString
#define cJSON_IsTrue config_inert_IsTrue
#define cJSON_IsNumber config_inert_IsNumber
#define cJSON_Delete config_inert_Delete
#define HH_STORAGE_SOURCE "config_storage_source.h"
#define main storage_suite_unused_main
#include "test_storage.c"
#undef main

static bool load_config_text(const char *json)
{
    reset();
    tkl_fs_remove(HH_ROOT "/config.json");
    if(json && !hh_file_write_atomic(HH_ROOT "/config.json",(const uint8_t*)json,strlen(json)))return false;
    return hh_load_config();
}
static void missing_config_enables_pir_and_keeps_chat_defaults(void)
{
    CHECK(!load_config_text(NULL));
    CHECK(hh_device.pir_enabled);
    CHECK(hh_device.chat_enabled&&hh_valid_id(hh_device.tuya_product_id)&&hh_device.volume==100);
}
static void empty_config_enables_pir_and_keeps_chat_defaults(void)
{
    CHECK(load_config_text("{}"));
    CHECK(hh_device.pir_enabled);
    CHECK(hh_device.chat_enabled&&hh_valid_id(hh_device.tuya_product_id)&&hh_device.volume==100);
}
static void explicit_pir_false_is_preserved(void)
{
    CHECK(load_config_text("{\"pir_enabled\":false}"));
    CHECK(!hh_device.pir_enabled&&hh_device.chat_enabled);
}
static void explicit_pir_true_is_preserved(void)
{
    CHECK(load_config_text("{\"pir_enabled\":true}"));
    CHECK(hh_device.pir_enabled&&hh_device.chat_enabled);
}
static void explicit_chat_false_keeps_default_pir_enabled(void)
{
    CHECK(load_config_text("{\"chat_enabled\":false}"));
    CHECK(!hh_device.chat_enabled&&hh_device.pir_enabled);
}
static void explicit_pir_and_chat_false_are_preserved(void)
{
    CHECK(load_config_text("{\"pir_enabled\":false,\"chat_enabled\":false}"));
    CHECK(!hh_device.chat_enabled&&!hh_device.pir_enabled);
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    snprintf(runtime_root,sizeof(runtime_root),"%s",argv[1]);
    RUN(missing_config_enables_pir_and_keeps_chat_defaults);
    RUN(empty_config_enables_pir_and_keeps_chat_defaults);
    RUN(explicit_pir_false_is_preserved);
    RUN(explicit_pir_true_is_preserved);
    RUN(explicit_chat_false_keeps_default_pir_enabled);
    RUN(explicit_pir_and_chat_false_are_preserved);
    printf("%d failure(s)\n",failures);return failures?1:0;
}
