#ifndef HOMEHUB_EMOTION_H
#define HOMEHUB_EMOTION_H
#include <stdbool.h>

/* Reply expression, not a diagnosis of the user's mood. */
typedef enum {
    HH_EMOTION_NEUTRAL = 0,
    HH_EMOTION_HAPPY,
    HH_EMOTION_ANGRY,
    HH_EMOTION_SAD,
    HH_EMOTION_JOY,
    HH_EMOTION_COUNT
} HH_Emotion;

/* Documented Tuya emoji code/name. Unknown or malformed input returns false. */
bool hh_emotion_parse(const char *code, HH_Emotion *emotion);
#endif
