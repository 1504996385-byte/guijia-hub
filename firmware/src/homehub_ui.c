#include "homehub_platform.h"
#include "homehub_event_queue.h"
#include "homehub_face.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"
#include "tdl_display_manage.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_puhui_16_2);
#define FACE_BG 0x050a07
#define CURVE_POINTS 17
/* Every object and every point buffer is created once. The application task owns
 * lifecycle changes; LVGL callbacks use the already-held display mutex. */
static lv_obj_t *face_root,*settings,*eyes[2],*closed_eyes[2],*brows[2],*lids[2];
static lv_obj_t *smile,*open_mouth,*mouth_cut,*cheeks[2],*tear,*dots[3],*button_labels[2];
static lv_point_t closed_points[2][CURVE_POINTS],brow_points[2][CURVE_POINTS];
static lv_point_t lid_points[2][2],smile_points[CURVE_POINTS];
static HH_Face face;
static bool ready,settings_open;
static int selected[2]={HH_TRIGGER_AUTO,HH_TRIGGER_AUTO};
static unsigned preview_index=HH_EMOTION_HAPPY;
static uint32_t last_activity,last_tick,menu_activity;
static uint8_t current_brightness=75;

void hh_ui_wake(void){last_activity=(uint32_t)tal_system_get_millisecond();}
static lv_obj_t *shape(lv_obj_t *parent,int w,int h,int radius,uint32_t color)
{
    lv_obj_t *o=lv_obj_create(parent);lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o,w,h);lv_obj_set_style_radius(o,radius,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    return o;
}
static lv_obj_t *line(lv_obj_t *parent,unsigned width,uint32_t color)
{
    lv_obj_t *o=lv_line_create(parent);lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_line_width(o,width,0);lv_obj_set_style_line_color(o,lv_color_hex(color),0);
    lv_obj_set_style_line_rounded(o,true,0);lv_obj_set_style_line_opa(o,LV_OPA_COVER,0);
    return o;
}
/* Pinned LVGL8 RGB565 fill_normal rounds partial opacity 252 to 256 in an
 * 8-bit temporary, making unmasked rectangle centers vanish. Snap that final
 * transition step to opaque; native primitives and the SDK remain unchanged. */
static lv_opa_t opacity(int value){return value>=252?LV_OPA_COVER:value<=3?LV_OPA_TRANSP:(lv_opa_t)value;}
/* Small native LVGL polylines approximate the preview's quadratic strokes. */
static void curve(lv_obj_t *o,lv_point_t *points,int x,int y,int width,int bend,int slope)
{
    for(int i=0;i<CURVE_POINTS;i++){
        points[i].x=(lv_coord_t)(width*i/16);
        points[i].y=(lv_coord_t)(40+2*bend*i*(16-i)/256+slope*(i-8)/16);
    }
    lv_line_set_points(o,points,CURVE_POINTS);lv_obj_set_pos(o,x-width/2,y-40);
}
static lv_obj_t *text(lv_obj_t *parent,const char *value)
{
    lv_obj_t *o=lv_label_create(parent);lv_label_set_text(o,value);
    lv_obj_set_style_text_font(o,&font_puhui_16_2,0);
    lv_obj_set_style_text_color(o,lv_color_hex(0xffd69a),0);
    lv_obj_center(o);lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
static void selection_label(unsigned r)
{
    char value[64];snprintf(value,sizeof(value),"%s  %s",r?"TA":"我",
        selected[r]==HH_TRIGGER_AUTO?"自动":selected[r]==HH_ARRIVE?"下次归位":"下次取走");
    lv_label_set_text(button_labels[r],value);
}
static void show_face(void)
{
    settings_open=false;lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(face_root,LV_OBJ_FLAG_HIDDEN);
}
static void face_event(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_PRESSED)hh_ui_wake();
    if(code!=LV_EVENT_LONG_PRESSED||settings_open)return;
    hh_ui_wake();menu_activity=last_activity;settings_open=true;
    lv_obj_add_flag(face_root,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(settings,LV_OBJ_FLAG_HIDDEN);
}
static void trigger_click(lv_event_t *event)
{
    unsigned r=(unsigned)(uintptr_t)lv_event_get_user_data(event);
    if(r>=2)return;
    int next=hh_face_next_trigger(selected[r]);
    HH_Event message={.type=EV_SET_TRIGGER,.trigger=next};message.action.recipient=r;
    /* This callback already holds LVGL's mutex. Never block or reacquire it. */
    if(!hh_event_try_post(&message,0))return;
    selected[r]=next;selection_label(r);hh_ui_wake();menu_activity=last_activity;
}
static void back_click(lv_event_t *event){(void)event;hh_ui_wake();show_face();}
static void preview_click(lv_event_t *event)
{
    (void)event;hh_ui_wake();
    hh_face_preview(&face,(HH_Emotion)(preview_index++%HH_EMOTION_COUNT),last_activity);
    show_face();
}
static lv_obj_t *menu_button(const char *value,int y,lv_event_cb_t callback,void *data)
{
    lv_obj_t *o=lv_btn_create(settings);lv_obj_remove_style_all(o);
    lv_obj_set_size(o,286,48);lv_obj_set_pos(o,90,y);
    lv_obj_set_style_radius(o,22,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x243126),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o,callback,LV_EVENT_CLICKED,data);text(o,value);return o;
}
static void render_face(uint32_t now)
{
    const HH_FacePose *p=&face.pose;
    lv_color_t color=lv_color_make((uint8_t)p->red,(uint8_t)p->green,(uint8_t)p->blue);
    int dx=p->glance,dy=p->breathe;
    for(unsigned i=0;i<2;i++){
        int x=(i?324:142)+dx,y=p->eye_y[i]+dy;
        lv_obj_set_size(eyes[i],68,p->eye_h[i]);lv_obj_set_pos(eyes[i],x-34,y-p->eye_h[i]/2);
        lv_obj_set_style_bg_color(eyes[i],color,0);
        lv_obj_set_style_bg_opa(eyes[i],opacity(255-p->eye_closed),0);
        lv_obj_set_style_shadow_color(eyes[i],color,0);
        lv_obj_set_style_shadow_opa(eyes[i],(255-p->eye_closed)*30/255,0);
        curve(closed_eyes[i],closed_points[i],x,211+dy,68,p->eye_bend,0);
        lv_obj_set_style_line_color(closed_eyes[i],color,0);
        lv_obj_set_style_line_opa(closed_eyes[i],opacity(p->eye_closed),0);
        int slope=p->brow_slope*(i?-1:1);
        curve(brows[i],brow_points[i],x,137+dy,68,-5,slope);
        lv_obj_set_style_line_color(brows[i],color,0);
        lv_obj_set_style_line_opa(brows[i],opacity(p->brow_opa),0);
        /* A background stroke cuts the top edge into the angled angry eyes. */
        lid_points[i][0].x=0;lid_points[i][1].x=78;
        lid_points[i][0].y=(lv_coord_t)(20-slope/2);lid_points[i][1].y=(lv_coord_t)(20+slope/2);
        lv_line_set_points(lids[i],lid_points[i],2);
        lv_obj_set_pos(lids[i],x-39,y-p->eye_h[i]/2-20);
        lv_obj_set_style_line_opa(lids[i],p->brow_slope>0?opacity(p->brow_opa):0,0);
        lv_obj_set_pos(cheeks[i],(i?343:85)+dx,275+dy);
        lv_obj_set_style_bg_opa(cheeks[i],p->cheek_opa,0);
    }
    int my=299+dy;
    curve(smile,smile_points,233+dx,my-3,p->mouth_width,p->mouth_bend,0);
    lv_obj_set_style_line_color(smile,color,0);lv_obj_set_style_line_opa(smile,opacity(255-p->mouth_open),0);
    int mh=p->mouth_height,offset=mh>34?(mh-34)*3/4:0;
    lv_obj_set_size(open_mouth,p->mouth_width,mh);
    lv_obj_set_pos(open_mouth,233+dx-p->mouth_width/2,my+offset-mh/2);
    lv_obj_set_style_radius(open_mouth,mh<36?mh/2:24,0);
    lv_obj_set_style_bg_color(open_mouth,color,0);lv_obj_set_style_bg_opa(open_mouth,opacity(p->mouth_open),0);
    lv_obj_set_pos(mouth_cut,214+dx,322+dy);
    lv_obj_set_style_bg_opa(mouth_cut,mh>40?p->cheek_opa*p->mouth_open/255:0,0);
    lv_obj_set_pos(tear,322+dx,245+dy);lv_obj_set_style_bg_opa(tear,opacity(p->tear_opa),0);
    for(unsigned i=0;i<3;i++){
        unsigned phase=(now+i*330u)%1600u;
        unsigned pulse=phase<800u?phase:1600u-phase;
        lv_obj_set_pos(dots[i],209+(int)i*20+dx,355+dy);
        lv_obj_set_style_bg_color(dots[i],color,0);
        lv_obj_set_style_bg_opa(dots[i],p->dots_opa*(65+pulse*165/800)/255,0);
    }
}
void hh_ui_init(void)
{
    lv_vendor_init(DISPLAY_NAME);lv_vendor_disp_lock();lv_obj_t *screen=lv_scr_act();
    lv_obj_set_style_bg_color(screen,lv_color_hex(FACE_BG),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);lv_obj_set_style_border_width(screen,0,0);
    lv_obj_set_style_pad_all(screen,0,0);lv_obj_set_style_radius(screen,233,0);
    lv_obj_set_style_clip_corner(screen,true,0);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen,face_event,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(screen,face_event,LV_EVENT_LONG_PRESSED,NULL);
    face_root=shape(screen,466,466,0,FACE_BG);lv_obj_set_style_bg_opa(face_root,0,0);
    for(unsigned i=0;i<2;i++){
        eyes[i]=shape(face_root,68,108,34,0xffd69a);
        lv_obj_set_style_shadow_width(eyes[i],12,0);lv_obj_set_style_shadow_spread(eyes[i],0,0);
        lv_obj_set_style_shadow_color(eyes[i],lv_color_hex(0xffd69a),0);
        lv_obj_set_style_shadow_opa(eyes[i],30,0);
        closed_eyes[i]=line(face_root,15,0xffd69a);
        lids[i]=line(face_root,32,FACE_BG);brows[i]=line(face_root,7,0xffd69a);
        cheeks[i]=shape(face_root,38,16,LV_RADIUS_CIRCLE,0xd68d69);
    }
    smile=line(face_root,8,0xffd69a);
    open_mouth=shape(face_root,48,12,6,0xffd69a);
    mouth_cut=shape(face_root,38,16,LV_RADIUS_CIRCLE,FACE_BG);
    tear=shape(face_root,14,26,LV_RADIUS_CIRCLE,0x9ecbea);
    for(unsigned i=0;i<3;i++)dots[i]=shape(face_root,8,8,LV_RADIUS_CIRCLE,0xffd69a);
    settings=shape(screen,466,466,233,FACE_BG);lv_obj_add_flag(settings,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *title=text(settings,"留言设置");lv_obj_align(title,LV_ALIGN_TOP_MID,0,91);
    for(unsigned i=0;i<2;i++){
        lv_obj_t *b=menu_button(i?"TA  自动":"我  自动",139+(int)i*58,trigger_click,(void*)(uintptr_t)i);
        button_labels[i]=lv_obj_get_child(b,0);
    }
    menu_button("表情预览（轮换）",267,preview_click,NULL);
    menu_button("返回",325,back_click,NULL);
    uint32_t now=(uint32_t)tal_system_get_millisecond();hh_face_init(&face,now);render_face(now);
    ready=true;last_tick=now;hh_ui_wake();
    PR_NOTICE("HH UI mode=warm-glow face_only=1 idle-expression=30000ms");
    lv_vendor_disp_unlock();lv_vendor_start(5,8192);
}
void hh_ui_network(const char *value){(void)value;}
void hh_ui_chat_text(bool user_text,const char *value){(void)user_text;(void)value;}
void hh_ui_chat_emotion(uint32_t turn,HH_Emotion emotion)
{
    if(!ready)return;
    lv_vendor_disp_lock();(void)hh_face_emotion(&face,turn,emotion);lv_vendor_disp_unlock();
}
void hh_ui_feedback(const char *value)
{
    if(!ready||!value)return;
    hh_ui_wake();
    /* Existing operational diagnostics only; reply emotion comes exclusively
     * from validated cloud enums, never from conversation text. */
    if(strstr(value,"失败")||strstr(value,"缺失")||strstr(value,"未就绪")||strstr(value,"未连接")){
        lv_vendor_disp_lock();hh_face_error(&face,last_activity);lv_vendor_disp_unlock();
    }
}
void hh_ui_update(const HH_Action *action,HH_AudioState state)
{
    if(!ready)return;
    uint32_t now=(uint32_t)tal_system_get_millisecond();lv_vendor_disp_lock();
    HH_AudioState before=face.audio;
    hh_face_update(&face,state,action?action->chat_turn:face.turn,now);
    if(state>=HH_AUDIO_CHAT_CAPTURE&&before!=state&&settings_open)show_face();
    if(action&&action->type==HH_ACT_RECORD_START&&action->recipient<2){
        selected[action->recipient]=HH_TRIGGER_AUTO;selection_label(action->recipient);
    }
    lv_vendor_disp_unlock();
}
void hh_ui_tick(HH_AudioState state,uint32_t now)
{
    if(!ready||(uint32_t)(now-last_tick)<50u)return;
    last_tick=now;lv_vendor_disp_lock();
    hh_face_update(&face,state,face.turn,now);
    if(settings_open&&(uint32_t)(now-menu_activity)>=20000u)show_face();
    hh_face_pause_rotation(&face,settings_open);
    /* Keep the display awake; natural blinks and explicit previews remain. */
    bool sleep=false;
    hh_face_step(&face,now,sleep);if(!settings_open)render_face(now);
    uint8_t brightness=75;
    lv_vendor_disp_unlock();
    if(brightness!=current_brightness){
        TDL_DISP_HANDLE_T display=tdl_disp_find_dev(DISPLAY_NAME);
        if(display&&tdl_disp_set_brightness(display,brightness)==OPRT_OK)current_brightness=brightness;
    }
}
