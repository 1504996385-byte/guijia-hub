/* The reused host harness defines inert JSON functions for its storage-only
 * suite. Keep those symbols separate, then compile production configuration
 * parsing against the real SDK cJSON implementation. */
#undef cJSON_Parse
#undef cJSON_GetObjectItemCaseSensitive
#undef cJSON_IsString
#undef cJSON_IsTrue
#undef cJSON_IsNumber
#undef cJSON_Delete
#include "../../firmware/src/homehub_storage.c"
