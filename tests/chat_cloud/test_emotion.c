#include "homehub_emotion.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(x) do {if(!(x)){printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);failures++;return;}}while(0)
#define RUN(x) do {int n=failures;x();printf("%s %s\n",failures==n?"PASS":"FAIL",#x);}while(0)
static void documented_codes_and_names_map_to_five_expressions(void){
    const struct {const char *code,*name;HH_Emotion emotion;} labels[]={
        {"U+1F636","NEUTRAL",HH_EMOTION_NEUTRAL},{"U+1F642","HAPPY",HH_EMOTION_HAPPY},
        {"U+1F620","ANGRY",HH_EMOTION_ANGRY},{"U+1F614","SAD",HH_EMOTION_SAD},
        {"U+1F606","LAUGHING",HH_EMOTION_JOY},{"U+1F602","FUNNY",HH_EMOTION_JOY}};
    for(unsigned i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
        HH_Emotion out=HH_EMOTION_COUNT;CHECK(hh_emotion_parse(labels[i].code,&out)&&out==labels[i].emotion);
        out=HH_EMOTION_COUNT;CHECK(hh_emotion_parse(labels[i].name,&out)&&out==labels[i].emotion);
    }
}
static void unknown_and_malformed_labels_do_not_change_output(void){
    const char *invalid[]={NULL,"","happy","JOY","U+1F642junk","U+1F642 "," U+1F642","U+0000","U+1F60D","U+","1F642","U+110000","开心","I feel HAPPY"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){HH_Emotion out=HH_EMOTION_ANGRY;CHECK(!hh_emotion_parse(invalid[i],&out)&&out==HH_EMOTION_ANGRY);}
    CHECK(!hh_emotion_parse("U+1F642",NULL));
}
int main(void){RUN(documented_codes_and_names_map_to_five_expressions);RUN(unknown_and_malformed_labels_do_not_change_output);printf("%d failure(s)\n",failures);return failures?1:0;}
