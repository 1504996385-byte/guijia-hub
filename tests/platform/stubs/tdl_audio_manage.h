#ifndef HH_HOST_TDL_AUDIO_MANAGE_H
#define HH_HOST_TDL_AUDIO_MANAGE_H
#include <stdint.h>
typedef void *TDL_AUDIO_HANDLE_T;
typedef enum { TDL_AUDIO_FRAME_FORMAT_PCM=0 } TDL_AUDIO_FRAME_FORMAT_E;
typedef enum { TDL_AUDIO_TEST_STATUS=0 } TDL_AUDIO_STATUS_E;
typedef struct { uint32_t sample_rate,sample_bits,sample_ch_num; } TDL_AUDIO_INFO_T;
typedef void(*TDL_AUDIO_MIC_CB)(TDL_AUDIO_FRAME_FORMAT_E,TDL_AUDIO_STATUS_E,uint8_t*,uint32_t);
int tdl_audio_find(const char*,TDL_AUDIO_HANDLE_T*);
int tdl_audio_open(TDL_AUDIO_HANDLE_T,TDL_AUDIO_MIC_CB);
int tdl_audio_get_info(TDL_AUDIO_HANDLE_T,TDL_AUDIO_INFO_T*);
int tdl_audio_volume_set(TDL_AUDIO_HANDLE_T,uint8_t);
int tdl_audio_play(TDL_AUDIO_HANDLE_T,uint8_t*,uint32_t);
int tdl_audio_play_stop(TDL_AUDIO_HANDLE_T);
#endif
