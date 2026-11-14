#include "homehub_face.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned failures,checks;
#define CHECK(x) do { checks++; if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);failures++;}} while(0)
static void settle(HH_Face *f,uint32_t now,bool sleep){for(unsigned i=0;i<25;i++)hh_face_step(f,now+i*50,sleep);}
static void idle_rotation_holds_each_expression_for_thirty_seconds(void)
{
    HH_Face f;hh_face_init(&f,1000);
    hh_face_step(&f,30999,false);CHECK(f.mode==HH_FACE_NEUTRAL);
    hh_face_step(&f,31000,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,60999,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,61000,false);CHECK(f.mode==HH_FACE_ANGRY);
    hh_face_step(&f,91000,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_step(&f,121000,false);CHECK(f.mode==HH_FACE_JOY);
    hh_face_step(&f,151000,false);CHECK(f.mode==HH_FACE_NEUTRAL);
    hh_face_init(&f,UINT32_MAX-1000u);
    hh_face_step(&f,(UINT32_MAX-1000u)+29999u,false);CHECK(f.mode==HH_FACE_NEUTRAL);
    hh_face_step(&f,(UINT32_MAX-1000u)+30000u,false);CHECK(f.mode==HH_FACE_HAPPY);
}
static void idle_rotation_never_overrides_busy_chat_or_preview(void)
{
    HH_Face f;hh_face_init(&f,0);hh_face_step(&f,30000,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_update(&f,HH_AUDIO_RECORDING,0,40000);hh_face_step(&f,150000,false);CHECK(f.mode==HH_FACE_LISTEN);
    hh_face_update(&f,HH_AUDIO_IDLE,0,150000);hh_face_step(&f,150000,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,179999,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,180000,false);CHECK(f.mode==HH_FACE_ANGRY);
    hh_face_update(&f,HH_AUDIO_CHAT_CAPTURE,18,180001);hh_face_step(&f,220000,false);CHECK(f.mode==HH_FACE_LISTEN);
    hh_face_update(&f,HH_AUDIO_CHAT_WAIT,18,220001);CHECK(hh_face_emotion(&f,18,HH_EMOTION_SAD));
    hh_face_step(&f,300000,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_update(&f,HH_AUDIO_CHAT_PLAY,18,300001);hh_face_step(&f,400000,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_update(&f,HH_AUDIO_IDLE,0,400000);hh_face_step(&f,405999,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_step(&f,406000,false);CHECK(f.mode==HH_FACE_ANGRY);
    hh_face_step(&f,435999,false);CHECK(f.mode==HH_FACE_ANGRY);
    hh_face_step(&f,436000,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_preview(&f,HH_EMOTION_JOY,436001);hh_face_step(&f,440000,false);CHECK(f.mode==HH_FACE_JOY);
    hh_face_step(&f,442001,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_step(&f,472000,false);CHECK(f.mode==HH_FACE_SAD);
    hh_face_step(&f,472001,false);CHECK(f.mode==HH_FACE_JOY);
    hh_face_error(&f,480000);hh_face_step(&f,482999,false);CHECK(f.mode==HH_FACE_ERROR);
    hh_face_step(&f,483000,false);CHECK(f.mode==HH_FACE_JOY);
    hh_face_step(&f,513000,false);CHECK(f.mode==HH_FACE_NEUTRAL);
}
static void idle_rotation_does_not_queue_stalled_ticks_or_hide_local_audio(void)
{
    HH_Face f;hh_face_init(&f,0);
    hh_face_step(&f,300000,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,300050,false);CHECK(f.mode==HH_FACE_HAPPY);
    const HH_AudioState states[]={HH_AUDIO_RECORDING,HH_AUDIO_SAVING,HH_AUDIO_PLAYING,HH_AUDIO_GREETING};
    const HH_FaceMode modes[]={HH_FACE_LISTEN,HH_FACE_THINK,HH_FACE_TALK,HH_FACE_TALK};
    for(unsigned i=0;i<4;i++){
        hh_face_init(&f,0);hh_face_step(&f,30000,false);CHECK(f.mode==HH_FACE_HAPPY);
        hh_face_update(&f,states[i],0,30001);hh_face_step(&f,90000,false);CHECK(f.mode==modes[i]);
        hh_face_update(&f,HH_AUDIO_IDLE,0,90001);hh_face_step(&f,90001,false);CHECK(f.mode==HH_FACE_HAPPY);
        hh_face_step(&f,120000,false);CHECK(f.mode==HH_FACE_HAPPY);
        hh_face_step(&f,120001,false);CHECK(f.mode==HH_FACE_ANGRY);
    }
}
int main(void)
{
    idle_rotation_holds_each_expression_for_thirty_seconds();
    idle_rotation_never_overrides_busy_chat_or_preview();
    idle_rotation_does_not_queue_stalled_ticks_or_hide_local_audio();
    HH_Face f;hh_face_init(&f,0);settle(&f,1000,false);
    CHECK(f.mode==HH_FACE_NEUTRAL);CHECK(f.pose.eye_h[0]>=104&&f.pose.eye_h[0]<=110);
    CHECK(f.pose.red==255&&f.pose.green==214&&f.pose.blue==154);
    CHECK(f.pose.mouth_bend>0);CHECK(!hh_face_emotion(&f,0,HH_EMOTION_HAPPY));
    hh_face_update(&f,HH_AUDIO_CHAT_CAPTURE,12,2300);hh_face_step(&f,2350,false);
    CHECK(f.turn==12&&f.mode==HH_FACE_LISTEN);
    CHECK(!hh_face_emotion(&f,11,HH_EMOTION_ANGRY));
    CHECK(!hh_face_emotion(&f,12,HH_EMOTION_HAPPY)); /* reply tags never decorate capture */
    hh_face_update(&f,HH_AUDIO_CHAT_WAIT,12,2500);hh_face_step(&f,2550,false);CHECK(f.mode==HH_FACE_THINK);
    CHECK(hh_face_emotion(&f,12,HH_EMOTION_HAPPY));settle(&f,2600,false);
    CHECK(f.mode==HH_FACE_HAPPY&&f.pose.eye_closed>=250&&f.pose.cheek_opa>0);
    CHECK(!hh_face_emotion(&f,12,HH_EMOTION_COUNT));CHECK(f.emotion==HH_EMOTION_HAPPY);
    hh_face_update(&f,HH_AUDIO_CHAT_PLAY,12,4000);hh_face_step(&f,4050,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_update(&f,HH_AUDIO_IDLE,0,4500);hh_face_step(&f,4600,false);
    CHECK(f.mode==HH_FACE_HAPPY&&f.turn==0&&f.holding);
    CHECK(!hh_face_emotion(&f,12,HH_EMOTION_ANGRY));
    hh_face_step(&f,10499,false);CHECK(f.mode==HH_FACE_HAPPY);
    hh_face_step(&f,10500,false);CHECK(f.mode==HH_FACE_NEUTRAL&&!f.has_emotion);
    hh_face_update(&f,HH_AUDIO_CHAT_CAPTURE,13,11000);hh_face_update(&f,HH_AUDIO_CHAT_WAIT,13,11500);
    CHECK(hh_face_emotion(&f,13,HH_EMOTION_SAD));settle(&f,11600,false);
    CHECK(f.mode==HH_FACE_SAD&&f.pose.red==179&&f.pose.green==214&&f.pose.blue==239);
    CHECK(f.pose.tear_opa>0&&f.pose.mouth_bend<0);
    hh_face_update(&f,HH_AUDIO_CHAT_STOPPING,13,13000);hh_face_step(&f,13050,false);
    CHECK(!f.has_emotion&&f.turn==0);CHECK(!hh_face_emotion(&f,13,HH_EMOTION_JOY));
    hh_face_error(&f,13100);settle(&f,13100,false);CHECK(f.mode==HH_FACE_ERROR);
    hh_face_update(&f,HH_AUDIO_CHAT_CAPTURE,14,14500);hh_face_step(&f,14550,false);
    CHECK(!f.error&&!f.preview&&!f.has_emotion&&f.mode==HH_FACE_LISTEN);
    hh_face_update(&f,HH_AUDIO_CHAT_WAIT,14,14600);CHECK(hh_face_emotion(&f,14,HH_EMOTION_ANGRY));
    settle(&f,14700,false);CHECK(f.pose.brow_opa>0&&f.pose.brow_slope>0&&f.pose.green==187);
    hh_face_update(&f,HH_AUDIO_CHAT_CAPTURE,15,16000);CHECK(!f.has_emotion&&f.turn==15);
    CHECK(!hh_face_emotion(&f,14,HH_EMOTION_SAD));
    hh_face_update(&f,HH_AUDIO_IDLE,0,16200);settle(&f,16300,true);CHECK(f.mode==HH_FACE_SLEEP);
    hh_face_preview(&f,HH_EMOTION_JOY,18000);settle(&f,18100,false);
    CHECK(f.mode==HH_FACE_JOY&&f.pose.mouth_open>250&&f.pose.mouth_height>=50);
    hh_face_step(&f,24000,false);CHECK(!f.preview&&f.mode==HH_FACE_NEUTRAL);
    hh_face_error(&f,0xffffff00u);hh_face_step(&f,0x100u,false);CHECK(f.mode==HH_FACE_ERROR);
    hh_face_step(&f,0x1000u,false);CHECK(!f.error);
    CHECK(hh_face_next_trigger(HH_TRIGGER_AUTO)==HH_ARRIVE);
    CHECK(hh_face_next_trigger(HH_ARRIVE)==HH_LEAVE);
    CHECK(hh_face_next_trigger(HH_LEAVE)==HH_TRIGGER_AUTO);
    /* Thousands of actual pose steps exercise blink, smooth bounded geometry and wake. */
    hh_face_init(&f,0);unsigned blink=0,movement=0;int old=108,old_breathe=0;
    for(unsigned i=1;i<4000;i++){
        hh_face_step(&f,i*50,false);
        if(f.pose.eye_h[0]<25)blink++;
        if(f.pose.eye_h[0]!=old||f.pose.breathe!=old_breathe)movement++;
        old=f.pose.eye_h[0];old_breathe=f.pose.breathe;
        CHECK(f.pose.eye_h[0]>=5&&f.pose.eye_h[0]<=124);
        CHECK(f.pose.glance>=-4&&f.pose.glance<=4&&f.pose.breathe>=-2&&f.pose.breathe<=2);
    }
    CHECK(blink>15&&blink<250);CHECK(movement>300);
    printf("%u checks, %u failure(s)\n",checks,failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
