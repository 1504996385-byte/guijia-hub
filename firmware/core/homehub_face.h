#ifndef HOMEHUB_FACE_H
#define HOMEHUB_FACE_H
#include "homehub_core.h"
#include "homehub_emotion.h"

/* Application-owned face lifecycle; no LVGL/cloud/heap dependency. */
typedef enum {
    HH_FACE_NEUTRAL, HH_FACE_HAPPY, HH_FACE_ANGRY, HH_FACE_SAD, HH_FACE_JOY,
    HH_FACE_LISTEN, HH_FACE_THINK, HH_FACE_TALK, HH_FACE_SLEEP, HH_FACE_ERROR
} HH_FaceMode;

typedef struct {
    int16_t eye_h[2], eye_y[2], eye_closed, eye_bend;
    int16_t brow_slope, brow_opa, mouth_width, mouth_bend, mouth_open, mouth_height;
    int16_t cheek_opa, tear_opa, dots_opa, red, green, blue;
    int16_t glance, breathe;
} HH_FacePose;

typedef struct {
    HH_AudioState audio;
    uint32_t turn, hold_since, error_since, preview_since, last_step;
    uint32_t idle_since;
    HH_Emotion emotion, preview_emotion;
    HH_Emotion idle_emotion;
    bool has_emotion, holding, error, preview, posed;
    bool idle_active, idle_rotation_paused;
    HH_FaceMode mode;
    HH_FacePose pose, base;
} HH_Face;

void hh_face_init(HH_Face *face, uint32_t now);
void hh_face_update(HH_Face *face, HH_AudioState audio, uint32_t turn, uint32_t now);
bool hh_face_emotion(HH_Face *face, uint32_t turn, HH_Emotion emotion);
void hh_face_error(HH_Face *face, uint32_t now);
void hh_face_preview(HH_Face *face, HH_Emotion emotion, uint32_t now);
/* Settings pause the idle cycle without stopping blinks or animation. */
void hh_face_pause_rotation(HH_Face *face, bool paused);
/* 50 ms caller cadence. Transitions settle gently; all geometry stays bounded. */
void hh_face_step(HH_Face *face, uint32_t now, bool sleeping);
int hh_face_next_trigger(int trigger);
#endif
