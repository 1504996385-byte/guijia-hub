/* Deliberately inert implementation used ONLY to prove behavioral tests fail. */
#include "homehub_core.h"
#include <string.h>
HH_Config hh_default_config(void) { HH_Config c; memset(&c,0,sizeof(c)); return c; }
void hh_init(HH_Core*c,const HH_Config*f,HH_ActionFn a,void*x,uint32_t n){memset(c,0,sizeof(*c));c->config=*f;c->action=a;c->context=x;(void)n;}
void hh_sample(HH_Core*c,uint32_t n,const bool*k,const bool*b,bool p){(void)c;(void)n;(void)k;(void)b;(void)p;}
void hh_sample_with_chat(HH_Core*c,uint32_t n,const bool*k,const bool*b,bool p,bool key){(void)key;hh_sample(c,n,k,b,p);}
void hh_chat_set_availability(HH_Core*c,HH_ChatAvailability a){(void)c;(void)a;}
HH_Result hh_chat_voice_wake(HH_Core*c,uint32_t n){(void)c;(void)n;return HH_BUSY;}
HH_Result hh_chat_voice_activity(HH_Core*c,uint32_t t,bool s,uint32_t n){(void)c;(void)t;(void)s;(void)n;return HH_BUSY;}
HH_Result hh_chat_report(HH_Core*c,uint32_t t,HH_ChatEvent e,bool s,uint32_t n){(void)c;(void)t;(void)e;(void)s;(void)n;return HH_INVALID;}
HH_Result hh_set_record_trigger(HH_Core*c,HH_Trigger t){(void)c;(void)t;return HH_INVALID;}
HH_Result hh_set_next_trigger(HH_Core*c,unsigned r,int t){(void)c;(void)r;(void)t;return HH_INVALID;}
void hh_recording_started(HH_Core*c,bool s){(void)c;(void)s;}
HH_Result hh_recording_saved(HH_Core*c,const char*i,const char*p,bool s){(void)c;(void)i;(void)p;(void)s;return HH_INVALID;}
HH_Result hh_add_message(HH_Core*c,const HH_Message*m){(void)c;(void)m;return HH_INVALID;}
HH_Result hh_playback_finished(HH_Core*c,const char*i,bool s){(void)c;(void)i;(void)s;return HH_INVALID;}
HH_Result hh_greeting_ready(HH_Core*c,const char*p,bool a){(void)c;(void)p;(void)a;return HH_INVALID;}
HH_Result hh_request_greeting(HH_Core*c,unsigned r,HH_Trigger t){(void)c;(void)r;(void)t;return HH_INVALID;}
void hh_export(const HH_Core*c,HH_Snapshot*s){(void)c;memset(s,0,sizeof(*s));}
HH_Result hh_restore(HH_Core*c,const HH_Snapshot*s){(void)c;(void)s;return HH_INVALID;}
size_t hh_pending_count(const HH_Core*c,unsigned r){(void)c;(void)r;return 0;}
const HH_Message*hh_find_message(const HH_Core*c,const char*i){(void)c;(void)i;return 0;}
