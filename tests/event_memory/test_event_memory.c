#include "homehub_platform.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line=%u %s\n", (unsigned)__LINE__, #x); failures++; } } while (0)

int main(void)
{
    /* A 64-slot app queue must leave native memory for TCP and FatFs. This
     * uses the actual production event type, not the independent SDK fakes. */
    CHECK(sizeof(HH_Event) <= 512u);
    printf("event_bytes=%u queue64_bytes=%u\n", (unsigned)sizeof(HH_Event), (unsigned)(64u*sizeof(HH_Event)));

    HH_Event source={.type=EV_NETWORK_INFO,.ok=true}, queued={0};
    source.action.recipient=1;source.action.trigger=HH_ARRIVE;
    strcpy(source.action.audio_path,"/sdcard/homehub/voices/net1_0.wav");
    strcpy(source.text,"keep network text and audio path together");
    memcpy(&queued,&source,sizeof(queued));memset(&source,0,sizeof(source));
    CHECK(queued.type==EV_NETWORK_INFO && queued.ok && queued.action.recipient==1);
    CHECK(!strcmp(queued.action.audio_path,"/sdcard/homehub/voices/net1_0.wav"));
    CHECK(!strcmp(queued.text,"keep network text and audio path together"));

    source=(HH_Event){.type=EV_REMOTE_MESSAGE,.ok=true};
    memset(&source.message,0xa5,sizeof(source.message));
    strcpy(source.message.id,"remote_payload");
    strcpy(source.message.audio_path,"/sdcard/homehub/messages/remote_payload.wav");
    source.message.recipient=1;source.message.trigger=HH_LEAVE;
    source.message.source=HH_REMOTE;source.message.state=HH_PENDING;
    HH_Message expected=source.message;
    memcpy(&queued,&source,sizeof(queued));memset(&source,0,sizeof(source));
    CHECK(queued.type==EV_REMOTE_MESSAGE && queued.ok);
    CHECK(!memcmp(&queued.message,&expected,sizeof(expected)));

    source=(HH_Event){.type=EV_CHAT_TEXT,.chat_is_user_text=true,.emotion=HH_EMOTION_HAPPY};
    source.action.chat_turn=12345;strcpy(source.text,"complete chat text");
    memcpy(&queued,&source,sizeof(queued));
    CHECK(queued.type==EV_CHAT_TEXT && queued.chat_is_user_text);
    CHECK(queued.action.chat_turn==12345 && queued.emotion==HH_EMOTION_HAPPY);
    CHECK(!strcmp(queued.text,"complete chat text"));
    printf("event payload checks failures=%u\n",failures);
    return failures?1:0;
}
