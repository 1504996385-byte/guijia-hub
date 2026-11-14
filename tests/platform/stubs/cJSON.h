#ifndef HH_HOST_CJSON_H
#define HH_HOST_CJSON_H
typedef struct cJSON { char *valuestring; int valueint; } cJSON;
cJSON *cJSON_Parse(const char *text);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *json,const char *key);
int cJSON_IsString(const cJSON *json);
int cJSON_IsTrue(const cJSON *json);
int cJSON_IsNumber(const cJSON *json);
void cJSON_Delete(cJSON *json);
#endif
