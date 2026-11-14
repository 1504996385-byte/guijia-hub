/* Execute the actual hh_ui_tick body with the actual portable face model.
 * Only display/render plumbing is stubbed; no LVGL process or SDK build. */
#include "homehub_face.h"
#include <stdio.h>
typedef void *TDL_DISP_HANDLE_T;
#define OPRT_OK 0
#define DISPLAY_NAME "host-display"
static HH_Face face;
static struct {bool pir_enabled;} hh_device;
static bool ready,settings_open;
static uint32_t last_activity,last_tick,menu_activity;
static uint8_t current_brightness;
static unsigned failures,locks,renders,menu_closes,brightness_calls,dim_calls;
static void lv_vendor_disp_lock(void){locks++;}
static void lv_vendor_disp_unlock(void){locks--;}
static void show_face(void){settings_open=false;menu_closes++;}
static void render_face(uint32_t now){(void)now;renders++;}
static TDL_DISP_HANDLE_T tdl_disp_find_dev(const char *name){(void)name;return (void*)1;}
static int tdl_disp_set_brightness(TDL_DISP_HANDLE_T display,uint8_t value){
    (void)display;brightness_calls++;if(value!=75)dim_calls++;return OPRT_OK;
}
#include "ui_tick_actual.h"
#define CHECK(x) do{if(!(x)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);failures++;return;}}while(0)
static void reset(void){
    hh_face_init(&face,0);hh_device.pir_enabled=true;ready=true;settings_open=false;
    last_activity=last_tick=menu_activity=0;current_brightness=75;
    locks=renders=menu_closes=brightness_calls=dim_calls=0;
}
static void idle_past_sixty_seconds_stays_awake_and_naturally_blinks(void){
    reset();unsigned blinks=0,open=0;
    for(uint32_t now=61000;now<=120000;now+=50){
        hh_ui_tick(HH_AUDIO_IDLE,now);
        CHECK(face.mode<=HH_FACE_JOY&&current_brightness==75&&!dim_calls&&!locks);
        if(face.pose.eye_h[0]<25)blinks++;
        if(face.pose.eye_h[0]>100)open++;
    }
    CHECK(blinks>0&&open>100&&renders>100&&!brightness_calls);
}
static void menu_still_returns_after_twenty_seconds_without_dimming(void){
    reset();settings_open=true;menu_activity=100000;
    hh_ui_tick(HH_AUDIO_IDLE,119950);CHECK(settings_open&&!menu_closes&&!renders);
    hh_ui_tick(HH_AUDIO_IDLE,120000);
    CHECK(!settings_open&&menu_closes==1&&renders==1&&face.mode==HH_FACE_NEUTRAL);
    CHECK(current_brightness==75&&!dim_calls&&!locks);
}
static void stale_dim_level_is_restored_to_normal(void){
    reset();current_brightness=15;hh_ui_tick(HH_AUDIO_IDLE,61000);
    CHECK(current_brightness==75&&brightness_calls==1&&!dim_calls);
}
static void idle_cycles_at_thirty_seconds_and_menu_pauses_rotation(void){
    reset();hh_ui_tick(HH_AUDIO_IDLE,29950);CHECK(face.mode==HH_FACE_NEUTRAL);
    hh_ui_tick(HH_AUDIO_IDLE,30000);CHECK(face.mode==HH_FACE_HAPPY);
    hh_ui_tick(HH_AUDIO_IDLE,59950);CHECK(face.mode==HH_FACE_HAPPY);
    hh_ui_tick(HH_AUDIO_IDLE,60000);CHECK(face.mode==HH_FACE_ANGRY);
    settings_open=true;menu_activity=60000;
    for(uint32_t now=60050;now<150000;now+=50){
        menu_activity=now;hh_ui_tick(HH_AUDIO_IDLE,now);CHECK(face.mode==HH_FACE_ANGRY);
    }
    settings_open=false;hh_ui_tick(HH_AUDIO_IDLE,150000);CHECK(face.mode==HH_FACE_ANGRY);
    hh_ui_tick(HH_AUDIO_IDLE,179950);CHECK(face.mode==HH_FACE_ANGRY);
    hh_ui_tick(HH_AUDIO_IDLE,180000);CHECK(face.mode==HH_FACE_SAD);
}
#define RUN(t) do{unsigned before=failures;t();if(before==failures)printf("PASS %s\n",#t);}while(0)
int main(void){
    RUN(idle_past_sixty_seconds_stays_awake_and_naturally_blinks);
    RUN(menu_still_returns_after_twenty_seconds_without_dimming);
    RUN(stale_dim_level_is_restored_to_normal);
    RUN(idle_cycles_at_thirty_seconds_and_menu_pauses_rotation);
    printf("UI idle: %u failure(s)\n",failures);return failures?1:0;
}
