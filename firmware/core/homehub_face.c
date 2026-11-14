#include "homehub_face.h"
#include <string.h>

static bool reply_audio(HH_AudioState s){return s==HH_AUDIO_CHAT_WAIT||s==HH_AUDIO_CHAT_PLAY;}
static int wave(uint32_t now,unsigned period,int amplitude)
{
    unsigned phase=(now%period)*1024u/period,x=phase%512u;
    int value=(int)(4u*x*(512u-x)) * amplitude/262144;
    return phase<512u?value:-value;
}
static int16_t ease(int16_t old,int16_t target)
{
    int d=target-old;
    return (int16_t)(old+(d>0?(d+3)/4:d<0?(d-3)/4:0));
}
void hh_face_init(HH_Face *f,uint32_t now)
{
    memset(f,0,sizeof(*f));f->last_step=now;
    hh_face_step(f,now,false);
}
void hh_face_update(HH_Face *f,HH_AudioState audio,uint32_t turn,uint32_t now)
{
    if(audio!=HH_AUDIO_IDLE || f->audio!=HH_AUDIO_IDLE)f->idle_active=false;
    if(audio==HH_AUDIO_CHAT_CAPTURE &&
       (f->audio!=HH_AUDIO_CHAT_CAPTURE || (turn && turn!=f->turn))) {
        f->has_emotion=false;f->holding=false;f->preview=false;f->error=false;
        f->turn=turn;
    } else if(reply_audio(audio) && turn && turn!=f->turn) {
        /* Recover if the first UI update happens after capture was submitted. */
        f->turn=turn;f->has_emotion=false;f->holding=false;f->preview=false;f->error=false;
    } else if(audio==HH_AUDIO_CHAT_STOPPING) {
        f->turn=0;f->has_emotion=false;f->holding=false;f->preview=false;
    } else if(audio==HH_AUDIO_IDLE && f->audio!=HH_AUDIO_IDLE) {
        f->holding=f->audio==HH_AUDIO_CHAT_PLAY&&f->has_emotion;
        f->hold_since=now;f->turn=0;
        if(!f->holding)f->has_emotion=false;
    } else if(audio<HH_AUDIO_CHAT_CAPTURE && audio!=HH_AUDIO_IDLE) {
        f->turn=0;f->has_emotion=false;f->holding=false;f->preview=false;
    }
    f->audio=audio;
}
bool hh_face_emotion(HH_Face *f,uint32_t turn,HH_Emotion emotion)
{
    if(!turn || turn!=f->turn || !reply_audio(f->audio) || (unsigned)emotion>=HH_EMOTION_COUNT)return false;
    if(!f->has_emotion || f->emotion!=emotion){f->emotion=emotion;f->has_emotion=true;}
    return true;
}
void hh_face_error(HH_Face *f,uint32_t now)
{
    f->idle_active=false;
    f->error=true;f->error_since=now;f->has_emotion=false;f->holding=false;f->preview=false;
}
void hh_face_preview(HH_Face *f,HH_Emotion emotion,uint32_t now)
{
    if((unsigned)emotion>=HH_EMOTION_COUNT || f->audio>=HH_AUDIO_CHAT_CAPTURE)return;
    f->idle_active=false;
    f->preview=true;f->preview_emotion=emotion;f->preview_since=now;f->error=false;
}
void hh_face_pause_rotation(HH_Face *f,bool paused)
{
    if(paused || f->idle_rotation_paused!=paused)f->idle_active=false;
    f->idle_rotation_paused=paused;
}
int hh_face_next_trigger(int t){return t==HH_TRIGGER_AUTO?HH_ARRIVE:t==HH_ARRIVE?HH_LEAVE:HH_TRIGGER_AUTO;}
void hh_face_step(HH_Face *f,uint32_t now,bool sleeping)
{
    if(f->holding&&(uint32_t)(now-f->hold_since)>=6000u){f->holding=false;f->has_emotion=false;}
    if(f->error&&(uint32_t)(now-f->error_since)>=3000u)f->error=false;
    if(f->preview&&(uint32_t)(now-f->preview_since)>=6000u)f->preview=false;
    bool idle=f->audio==HH_AUDIO_IDLE && !f->holding && !f->error && !f->preview &&
        !sleeping && !f->idle_rotation_paused;
    if(!idle)f->idle_active=false;
    else if(!f->idle_active){f->idle_since=now;f->idle_active=true;}
    else if((uint32_t)(now-f->idle_since)>=30000u){
        /* Skip missed deadlines after stalls; never cycle rapidly to catch up. */
        f->idle_since=now;f->idle_emotion=(HH_Emotion)((f->idle_emotion+1)%HH_EMOTION_COUNT);
    }
    HH_FaceMode m=f->audio==HH_AUDIO_IDLE?(HH_FaceMode)f->idle_emotion:HH_FACE_NEUTRAL;
    if(f->error)m=HH_FACE_ERROR;
    else if(f->audio==HH_AUDIO_CHAT_CAPTURE)m=HH_FACE_LISTEN;
    else if(f->has_emotion)m=(HH_FaceMode)f->emotion;
    else if(f->preview)m=(HH_FaceMode)f->preview_emotion;
    else if(f->audio==HH_AUDIO_CHAT_WAIT||f->audio==HH_AUDIO_SAVING)m=HH_FACE_THINK;
    else if(f->audio==HH_AUDIO_CHAT_PLAY||f->audio==HH_AUDIO_PLAYING||f->audio==HH_AUDIO_GREETING)m=HH_FACE_TALK;
    else if(f->audio==HH_AUDIO_RECORDING)m=HH_FACE_LISTEN;
    else if(sleeping)m=HH_FACE_SLEEP;
    f->mode=m;
    HH_FacePose p;memset(&p,0,sizeof(p));
    p.eye_h[0]=p.eye_h[1]=108;p.eye_y[0]=p.eye_y[1]=211;p.eye_bend=-24;
    p.red=255;p.green=214;p.blue=154;p.mouth_width=48;p.mouth_bend=23;p.mouth_height=12;
    switch(m){
    case HH_FACE_HAPPY:p.eye_closed=255;p.cheek_opa=178;p.mouth_bend=35;break;
    case HH_FACE_JOY:p.eye_closed=255;p.cheek_opa=178;p.mouth_open=255;p.mouth_width=66;p.mouth_height=54;break;
    case HH_FACE_ANGRY:p.red=255;p.green=187;p.blue=133;p.brow_opa=255;p.brow_slope=22;p.mouth_bend=-5;break;
    case HH_FACE_SAD:p.red=179;p.green=214;p.blue=239;p.eye_h[0]=p.eye_h[1]=81;
        p.brow_opa=220;p.brow_slope=-13;p.tear_opa=255;p.mouth_bend=-17;p.mouth_width=42;break;
    case HH_FACE_LISTEN:p.eye_h[0]=p.eye_h[1]=122;p.mouth_open=255;p.mouth_width=22;p.mouth_height=28;break;
    case HH_FACE_THINK:p.eye_h[0]=111;p.eye_h[1]=86;p.eye_y[1]=204;
        p.mouth_width=28;p.mouth_bend=8;p.dots_opa=255;break;
    case HH_FACE_SLEEP:p.eye_closed=255;p.eye_bend=7;p.mouth_open=255;p.mouth_width=14;p.mouth_height=12;break;
    case HH_FACE_ERROR:p.eye_h[0]=p.eye_h[1]=74;p.red=220;p.green=183;p.blue=156;
        p.brow_opa=200;p.brow_slope=-10;p.mouth_bend=-12;p.mouth_width=32;break;
    default:break;
    }
    if(m==HH_FACE_TALK || (f->audio==HH_AUDIO_CHAT_PLAY && m!=HH_FACE_JOY)) {
        p.mouth_open=255;p.mouth_width=52;p.mouth_height=(int16_t)(10+wave(now,450,24));
        if(p.mouth_height<10)p.mouth_height=(int16_t)(20-p.mouth_height);
    }
    if(!f->posed){f->base=p;f->posed=true;}
    else {
#define E(field) f->base.field=ease(f->base.field,p.field)
        E(eye_h[0]);E(eye_h[1]);E(eye_y[0]);E(eye_y[1]);E(eye_closed);E(eye_bend);
        E(brow_slope);E(brow_opa);E(mouth_width);E(mouth_bend);E(mouth_open);E(mouth_height);
        E(cheek_opa);E(tear_opa);E(dots_opa);E(red);E(green);E(blue);
#undef E
    }
    f->pose=f->base;
    /* Three subtly different blink intervals; blink never deforms the mouth. */
    unsigned phase=now%14100u,blink=256;
    unsigned local=phase>=13600u?phase-13600u:phase>=9300u?phase-9300u:phase>=4200u?phase-4200u:1000u;
    if(local<180u){int d=(int)local-90;if(d<0)d=-d;blink=15u+(unsigned)d*241u/90u;}
    for(unsigned i=0;i<2;i++){
        f->pose.eye_h[i]=(int16_t)(f->pose.eye_h[i]*blink/256u);
        if(f->pose.eye_h[i]<6)f->pose.eye_h[i]=6;
    }
    f->pose.glance=(int16_t)(m==HH_FACE_SLEEP?0:wave(now,9600,4));
    f->pose.breathe=(int16_t)wave(now,3500,2);f->last_step=now;
}
