/* Independent host driver. All drawing/object behavior comes from SDK LVGL8
 * and the actual firmware UI translation unit. No substitute renderer. */
#include "homehub_platform.h"
#include "homehub_chat.h"
#include "lvgl.h"
#include "lv_vendor.h"
#include "tdl_display_manage.h"
#if HH_RENDER_WITH_EMOTION
#include "homehub_emotion.h"
void hh_ui_chat_emotion(uint32_t turn, HH_Emotion emotion);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 466
#define HEIGHT 466
static lv_color_t draw_buffer[WIDTH * 48];
static uint16_t pixels[WIDTH * HEIGHT];
static lv_disp_draw_buf_t display_buffer;
static lv_disp_drv_t display_driver;
static uint32_t host_now;
static unsigned object_creates, flushes, tests, failures, queue_posts;
static unsigned lock_depth, maximum_lock_depth;
static bool queue_reject;
static uint32_t queue_timeout;
static HH_Event queued;
static uint8_t brightness;
static const char *output_directory;
HH_DeviceConfig hh_device;
QUEUE_HANDLE hh_events = (void *)1, hh_audio_jobs;
MUTEX_HANDLE hh_fs_mutex;
bool hh_storage_ready = true;
volatile bool hh_network_ready = true;
uint32_t hh_boot_id;

#define CHECK(condition, description) do { ++tests; if(!(condition)) { ++failures; \
    printf("FAIL %s (line %d)\n", description, __LINE__); } \
    else printf("PASS %s\n", description); } while(0)

void *tkl_system_malloc(size_t size) { return malloc(size); }
void tkl_system_free(void *pointer) { free(pointer); }
void *tkl_system_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
void *tkl_system_psram_malloc(size_t size) { return malloc(size); }
void tkl_system_psram_free(void *pointer) { free(pointer); }
void *tkl_system_psram_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
uint32_t tal_system_get_millisecond(void) { return host_now; }
uint32_t tkl_system_get_millisecond(void) { return host_now; }
void hh_chat_cloud_status(char *out, size_t capacity) { if(capacity) out[0] = 0; }
OPERATE_RET tal_queue_post(QUEUE_HANDLE queue, void *message, uint32_t timeout_ms)
{
    (void)queue;
    ++queue_posts;
    queue_timeout = timeout_ms;
    queued = *(HH_Event *)message;
    return queue_reject ? OPRT_COM_ERROR : OPRT_OK;
}
bool hh_event_try_post(const HH_Event *message, uint32_t timeout_ms)
{ return tal_queue_post(hh_events, (void *)message, timeout_ms) == OPRT_OK; }
TDL_DISP_HANDLE_T tdl_disp_find_dev(const char *name) { (void)name; return (void *)1; }
OPERATE_RET tdl_disp_set_brightness(TDL_DISP_HANDLE_T display, uint8_t value)
{ (void)display; brightness = value; return OPRT_OK; }

/* Compiling only lv_obj_class.c with a renamed factory lets this wrapper count
 * every real object creation, including allocate-delete churn between frames. */
lv_obj_t *hh_actual_lv_obj_class_create_obj(const lv_obj_class_t *class_p, lv_obj_t *parent);
lv_obj_t *lv_obj_class_create_obj(const lv_obj_class_t *class_p, lv_obj_t *parent)
{ ++object_creates; return hh_actual_lv_obj_class_create_obj(class_p, parent); }

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    int y, x;
    for(y = area->y1; y <= area->y2; ++y) {
        for(x = area->x1; x <= area->x2; ++x) {
            if(x >= 0 && y >= 0 && x < WIDTH && y < HEIGHT)
                pixels[y * WIDTH + x] = colors->full;
            ++colors;
        }
    }
    ++flushes;
    lv_disp_flush_ready(driver);
}
void lv_vendor_init(void *device)
{
    (void)device;
    lv_init();
    lv_disp_draw_buf_init(&display_buffer, draw_buffer, NULL, WIDTH * 48);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = WIDTH;
    display_driver.ver_res = HEIGHT;
    display_driver.draw_buf = &display_buffer;
    display_driver.flush_cb = flush;
    lv_disp_drv_register(&display_driver);
}
void lv_vendor_start(uint32_t priority, uint32_t stack_size) { (void)priority; (void)stack_size; }
void lv_vendor_disp_lock(void)
{ ++lock_depth; if(lock_depth > maximum_lock_depth) maximum_lock_depth = lock_depth; }
void lv_vendor_disp_unlock(void)
{ if(lock_depth) --lock_depth; else { puts("FAIL unmatched display unlock"); ++failures; } }

static bool visible(lv_obj_t *object)
{
    lv_obj_t *p = object;
    while(p) { if(lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return false; p = lv_obj_get_parent(p); }
    return true;
}
static unsigned count_class(lv_obj_t *parent, const lv_obj_class_t *class_p, bool only_visible)
{
    unsigned count = (!only_visible || visible(parent)) && lv_obj_check_type(parent, class_p);
    uint32_t i;
    for(i = 0; i < lv_obj_get_child_cnt(parent); ++i)
        count += count_class(lv_obj_get_child(parent, i), class_p, only_visible);
    return count;
}
static unsigned total_objects(lv_obj_t *parent)
{
    unsigned count = 1;
    uint32_t i;
    for(i = 0; i < lv_obj_get_child_cnt(parent); ++i) count += total_objects(lv_obj_get_child(parent, i));
    return count;
}
static lv_obj_t *find_label(lv_obj_t *parent, const char *text)
{
    uint32_t i;
    if(visible(parent) && lv_obj_check_type(parent, &lv_label_class)) {
        const char *source = lv_label_get_text(parent);
        char normalized[256]; unsigned n = 0;
        while(*source && n < sizeof(normalized) - 1) {
            if(*source != ' ' || !n || normalized[n - 1] != ' ') normalized[n++] = *source;
            ++source;
        }
        normalized[n] = 0;
        if(strstr(normalized, text)) return parent;
    }
    for(i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(parent, i), text);
        if(found) return found;
    }
    return NULL;
}
static lv_obj_t *button_for_label(const char *text)
{
    lv_obj_t *object = find_label(lv_scr_act(), text);
    while(object && !lv_obj_check_type(object, &lv_btn_class)) object = lv_obj_get_parent(object);
    return object;
}
static void advance(HH_AudioState state, uint32_t milliseconds)
{
    uint32_t end = host_now + milliseconds;
    while(host_now < end) {
        host_now += 50;
        hh_ui_tick(state, host_now);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}
static unsigned long long pixel_hash(void)
{
    unsigned i;
    unsigned long long value = 1469598103934665603ULL;
    for(i = 0; i < WIDTH * HEIGHT; ++i) { value ^= pixels[i]; value *= 1099511628211ULL; }
    return value;
}
static bool lit_pixel(int x,int y) { return ((pixels[y * WIDTH + x] >> 5) & 63) > 24; }
static void write_u16(FILE *file, unsigned value)
{ fputc(value & 255, file); fputc((value >> 8) & 255, file); }
static void write_u32(FILE *file, uint32_t value)
{ write_u16(file, value & 65535); write_u16(file, value >> 16); }
static unsigned long long capture(const char *name)
{
    char filename[1024];
    int y, x;
    const unsigned stride = (WIDTH * 3 + 3) & ~3u;
    FILE *file;
    lv_refr_now(NULL);
    {
        unsigned long long incremental = pixel_hash();
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        CHECK(incremental == pixel_hash(), "incremental framebuffer equals full refresh at same pose");
    }
    snprintf(filename, sizeof(filename), "%s/pose-%s.bmp", output_directory, name);
    file = fopen(filename, "wb");
    if(!file) { printf("FAIL cannot write %s\n", filename); ++failures; return 0; }
    fputc('B', file); fputc('M', file); write_u32(file, 54 + stride * HEIGHT);
    write_u32(file, 0); write_u32(file, 54); write_u32(file, 40);
    write_u32(file, WIDTH); write_u32(file, HEIGHT); write_u16(file, 1); write_u16(file, 24);
    write_u32(file, 0); write_u32(file, stride * HEIGHT); write_u32(file, 2835); write_u32(file, 2835);
    write_u32(file, 0); write_u32(file, 0);
    for(y = HEIGHT - 1; y >= 0; --y) {
        for(x = 0; x < WIDTH; ++x) {
            uint16_t p = pixels[y * WIDTH + x];
            unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            fputc((b * 255 + 15) / 31, file); fputc((g * 255 + 31) / 63, file); fputc((r * 255 + 15) / 31, file);
        }
        for(x = WIDTH * 3; x < (int)stride; ++x) fputc(0, file);
    }
    fclose(file);
    printf("IMAGE %s hash=%llx\n", name, pixel_hash());
    return pixel_hash();
}
static void check_main_is_face(void)
{
    CHECK(count_class(lv_scr_act(), &lv_label_class, true) == 0, "main face has no visible labels/subtitles/status/buttons");
    CHECK(count_class(lv_scr_act(), &lv_btn_class, true) == 0, "main face has no persistent buttons");
    CHECK(count_class(lv_scr_act(), &lv_canvas_class, false) == 0, "face uses no canvas framebuffer");
    CHECK(count_class(lv_scr_act(), &lv_img_class, false) == 0, "face uses no full-screen raster image");
}
static void check_neutral_geometry(void)
{
    unsigned side;
    uint16_t warm = lv_color_hex(0xffd69a).full;
    CHECK(pixels[400 * WIDTH + 233] == lv_color_hex(0x050a07).full,
          "neutral background matches approved deep green in RGB565");
    for(side = 0; side < 2; ++side) {
        int x, y, xmin = WIDTH, xmax = -1, ymin = HEIGHT, ymax = -1;
        int expected_x = side ? 324 : 142;
        for(y = 100; y <= 277; ++y) for(x = expected_x - 42; x <= expected_x + 42; ++x) {
            if(pixels[y * WIDTH + x] == warm) {
                if(x < xmin) xmin = x; if(x > xmax) xmax = x;
                if(y < ymin) ymin = y; if(y > ymax) ymax = y;
            }
        }
        CHECK(xmax - xmin + 1 >= 66 && xmax - xmin + 1 <= 70 &&
              ymax - ymin + 1 >= 104 && ymax - ymin + 1 <= 112,
              "neutral warm eye is approximately approved 68 by 108 pixels");
        CHECK(abs((xmin + xmax) / 2 - expected_x) <= 4 && abs((ymin + ymax) / 2 - 211) <= 5,
              "neutral eye center matches approved circular-screen coordinates");
        printf("GEOMETRY eye%u bounds=(%d,%d)-(%d,%d)\n", side, xmin, ymin, xmax, ymax);
    }
}

int main(int argc, char **argv)
{
    HH_Action action;
    unsigned created_after_init, count_after_init, i;
    unsigned long long hashes[5], listening, thinking, talking, neutral;
    lv_obj_t *button;
    if(argc != 2 && argc != 3) return 2;
    output_directory = argv[1];
    memset(&action, 0, sizeof(action)); action.chat_turn = 41;
    hh_device.pir_enabled = true;
    hh_ui_init();
    advance(HH_AUDIO_IDLE, 1000);
#if HH_RENDER_WITH_EMOTION
    /* Fresh process per expression uses the same absolute animation phase.
     * An animated face that ignores emotion cannot pass by changing over time. */
    if(argc == 3) {
        const HH_Emotion emotions[] = {HH_EMOTION_NEUTRAL, HH_EMOTION_HAPPY, HH_EMOTION_ANGRY, HH_EMOTION_SAD, HH_EMOTION_JOY};
        const char *names[] = {"10-emotion-neutral", "11-happy", "12-angry", "13-sad", "14-joy"};
        int pose = atoi(argv[2]);
        if(pose < 0 || pose > 4) return 2;
        hh_ui_update(&action, HH_AUDIO_CHAT_CAPTURE); advance(HH_AUDIO_CHAT_CAPTURE, 100);
        hh_ui_update(&action, HH_AUDIO_CHAT_WAIT); advance(HH_AUDIO_CHAT_WAIT, 100);
        hh_ui_update(&action, HH_AUDIO_CHAT_PLAY); advance(HH_AUDIO_CHAT_PLAY, 100);
        hh_ui_chat_emotion(41, emotions[pose]); advance(HH_AUDIO_CHAT_PLAY, 700);
        capture(names[pose]);
        if(pose == 3) CHECK(lit_pixel(331, 258), "sad tear center is filled without dark split");
        return failures ? 1 : 0;
    }
#endif
    created_after_init = object_creates;
    count_after_init = total_objects(lv_scr_act());
    printf("INFO LVGL=%d.%d.%d color_depth=%d objects=%u created=%u\n", LVGL_VERSION_MAJOR,
           LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LV_COLOR_DEPTH, count_after_init, created_after_init);
    check_main_is_face();
    neutral = capture("00-neutral");
    check_neutral_geometry();
    CHECK(pixels[HEIGHT * WIDTH / 2 + 233] != 0xffff, "face background is not an opaque white default screen");
    hh_ui_network("NETWORK MUST NOT BE DISPLAYED");
    hh_ui_chat_text(false, "CHAT MUST NOT BE DISPLAYED");
    hh_ui_update(&action, HH_AUDIO_CHAT_CAPTURE); advance(HH_AUDIO_CHAT_CAPTURE, 700);
    listening = capture("05-listening");
    CHECK(lit_pixel(233, 299), "listening mouth center is filled without dark split");
    check_main_is_face();
    hh_ui_update(&action, HH_AUDIO_CHAT_WAIT); advance(HH_AUDIO_CHAT_WAIT, 700);
    thinking = capture("06-thinking");
    hh_ui_update(&action, HH_AUDIO_CHAT_PLAY); advance(HH_AUDIO_CHAT_PLAY, 700);
    talking = capture("07-talking");
    CHECK(listening != thinking && thinking != talking && neutral != listening, "listening/thinking/talking produce distinct actual framebuffers");
#if HH_RENDER_WITH_EMOTION
    {
        const HH_Emotion emotions[] = {HH_EMOTION_NEUTRAL, HH_EMOTION_HAPPY, HH_EMOTION_ANGRY, HH_EMOTION_SAD, HH_EMOTION_JOY};
        const char *names[] = {"10-emotion-neutral", "11-happy", "12-angry", "13-sad", "14-joy"};
        for(i = 0; i < 5; ++i) {
            hh_ui_chat_emotion(41, emotions[i]);
            advance(HH_AUDIO_CHAT_PLAY, 700);
            hashes[i] = capture(names[i]);
        }
        for(i = 1; i < 5; ++i) {
            unsigned j; bool distinct = true;
            for(j = 0; j < i; ++j) if(hashes[i] == hashes[j]) distinct = false;
            CHECK(distinct, "reply emotion has distinct real rendered pixels");
        }
    }
#else
    (void)hashes;
#endif
    hh_ui_update(&action, HH_AUDIO_IDLE); advance(HH_AUDIO_IDLE, 1500);
    hh_ui_feedback("录音失败"); advance(HH_AUDIO_IDLE, 700); capture("20-error");
    CHECK(lit_pixel(142, 211) && lit_pixel(324, 211), "error eye centers are filled without dark split");
    advance(HH_AUDIO_IDLE, 65000); capture("21-sleep");
    CHECK(brightness == 15, "idle PIR timeout dims display");
    hh_ui_wake(); advance(HH_AUDIO_IDLE, 600); capture("22-wake");
    CHECK(brightness == 75, "wake restores display brightness");
    CHECK(object_creates == created_after_init, "all animation ticks allocate zero additional LVGL objects");
    CHECK(total_objects(lv_scr_act()) == count_after_init, "animation object tree remains bounded");
    lv_event_send(lv_scr_act(), LV_EVENT_LONG_PRESSED, NULL);
    advance(HH_AUDIO_IDLE, 200); capture("30-settings");
    CHECK(count_class(lv_scr_act(), &lv_btn_class, true) >= 3, "long press reveals temporary settings controls");
    button = button_for_label("我 自动");
    CHECK(button != NULL, "settings retains recipient one automatic preselection");
    if(button) {
        queue_reject = true;
        lv_event_send(button, LV_EVENT_CLICKED, NULL);
        CHECK(queue_posts == 1 && queue_timeout == 0, "recipient action uses nonblocking queue post even when full");
        CHECK(find_label(lv_scr_act(), "我 自动") != NULL, "queue rejection does not claim changed selection");
        queue_reject = false;
        lv_event_send(button, LV_EVENT_CLICKED, NULL);
        CHECK(queued.type == EV_SET_TRIGGER && queued.action.recipient == 0 && queued.trigger == HH_ARRIVE,
              "recipient one selects next arrival through existing action queue");
        CHECK(find_label(lv_scr_act(), "我 下次归位") != NULL, "accepted arrival preselection is visible");
        lv_event_send(button, LV_EVENT_CLICKED, NULL);
        CHECK(queued.trigger == HH_LEAVE, "recipient preselection cycles arrival to removal");
        lv_event_send(button, LV_EVENT_CLICKED, NULL);
        CHECK(queued.trigger == HH_TRIGGER_AUTO, "recipient preselection cycles removal to automatic");
    }
    button = button_for_label("TA 自动");
    CHECK(button != NULL, "settings retains recipient two automatic preselection");
    if(button) {
        lv_event_send(button, LV_EVENT_CLICKED, NULL);
        CHECK(queued.action.recipient == 1 && queued.trigger == HH_ARRIVE && queue_timeout == 0,
              "recipient two posts correct nonblocking independent selection");
    }
    CHECK(find_label(lv_scr_act(), "预览") != NULL, "settings emotion preview is explicitly labelled");
    button = button_for_label("返回");
    CHECK(button != NULL, "temporary settings provides return control");
    if(button) lv_event_send(button, LV_EVENT_CLICKED, NULL);
    advance(HH_AUDIO_IDLE, 300); capture("31-settings-closed");
    check_main_is_face();
    lv_event_send(lv_scr_act(), LV_EVENT_LONG_PRESSED, NULL);
    button = button_for_label("表情预览");
    CHECK(button != NULL, "labelled emotion preview is an operable control");
    if(button) lv_event_send(button, LV_EVENT_CLICKED, NULL);
    advance(HH_AUDIO_IDLE, 700); capture("32-preview");
    check_main_is_face();
    CHECK(object_creates == created_after_init, "opening/closing settings also reuses precreated objects");
    CHECK(lock_depth == 0 && maximum_lock_depth <= 1, "UI public calls balance display lock without nesting");
    CHECK(flushes > 0, "real LVGL software renderer flushed RGB565 pixels");
    printf("%u assertion(s), %u failure(s), %u flushes, %u total object creations\n", tests, failures, flushes, object_creates);
    return failures ? 1 : 0;
}
