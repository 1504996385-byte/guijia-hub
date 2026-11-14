#include "homehub_emotion.h"
#include <stddef.h>
#include <string.h>

bool hh_emotion_parse(const char *code, HH_Emotion *emotion)
{
    /* Only documented reply labels supported by the five-face design. */
    static const struct { const char *code,*name; HH_Emotion emotion; } labels[] = {
        {"U+1F636","NEUTRAL",HH_EMOTION_NEUTRAL},
        {"U+1F642","HAPPY",HH_EMOTION_HAPPY},
        {"U+1F620","ANGRY",HH_EMOTION_ANGRY},
        {"U+1F614","SAD",HH_EMOTION_SAD},
        {"U+1F606","LAUGHING",HH_EMOTION_JOY},
        {"U+1F602","FUNNY",HH_EMOTION_JOY}
    };
    if(!code || !emotion)return false;
    for(size_t i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
        if(!strcmp(code,labels[i].code) || !strcmp(code,labels[i].name)){
            *emotion=labels[i].emotion;return true;
        }
    }
    return false;
}
