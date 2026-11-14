#ifndef HH_RENDER_TDL_DISPLAY_MANAGE_H
#define HH_RENDER_TDL_DISPLAY_MANAGE_H
#include <stdint.h>
#include "tal_api.h"
typedef void *TDL_DISP_HANDLE_T;
TDL_DISP_HANDLE_T tdl_disp_find_dev(const char *name);
OPERATE_RET tdl_disp_set_brightness(TDL_DISP_HANDLE_T display, uint8_t value);
#endif
