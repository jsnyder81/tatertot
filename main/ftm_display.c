/* FTM Display Implementation for Waveshare ESP32-C6-Touch-AMOLED-1.32
 *
 * This file implements display functionality for showing FTM status
 * on a SH8601 AMOLED display with CST820 touch controller using LVGL.
 * Based on official Waveshare ESP-IDF examples.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "ftm_display.h"
#include "ftm_battery.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "ftm_display";

/* Defensive macro for LVGL widget creation - prevents NULL pointer crashes on heap exhaustion */
#define LV_CREATE_CHECK(widget, create_call) \
    do { \
        widget = create_call; \
        if (widget == NULL) { \
            ESP_LOGE(TAG, "LVGL OOM: %s", #create_call); \
            return; \
        } \
    } while(0)

/* External declarations for scan results from ftm_main.c */
extern uint16_t g_scan_ap_num;
extern wifi_ap_record_t *g_ap_list_buffer;

/* External declarations for FTM measurement data from ftm_main.c */
extern uint8_t s_ftm_report_num_entries;
extern uint32_t s_rtt_est;
extern uint32_t s_dist_est;
extern wifi_ap_record_t s_last_ftm_target_ap;

#define LCD_BIT_PER_PIXEL (16)
#define LVGL_BUF_HEIGHT (LCD_V_RES / 10)  /* Reduced from /4 to save ~128KB of DMA memory */

/* SH8601 LCD Initialization Commands - from Waveshare example */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xC4, (uint8_t[]) {0x80}, 1, 0},
    {0x3A, (uint8_t[]) {0x55}, 1, 0},
    {0x35, (uint8_t[]) {0x00}, 1, 0},
    {0x53, (uint8_t[]) {0x20}, 1, 0},
    {0x51, (uint8_t[]) {0xFF}, 1, 0},
    {0x63, (uint8_t[]) {0xFF}, 1, 0},
    {0x2A, (uint8_t[]) {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]) {0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]) {0x00}, 0, 100},
    {0x29, (uint8_t[]) {0x00}, 0, 0},
};

/* LVGL Objects */
static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t disp_drv;
static lv_disp_t *disp;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

/* UI Elements */
static lv_obj_t *status_label = NULL;
static lv_obj_t *distance_label = NULL;
static lv_obj_t *ap_name_label = NULL;  /* Shows which AP was used for ranging */
static lv_obj_t *rtt_label = NULL;
static lv_obj_t *ap_info_label = NULL;
static lv_obj_t *mode_label = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *scan_btn_label = NULL;
static lv_obj_t *ftm_btn = NULL;
static lv_obj_t *ftm_btn_label = NULL;
static lv_obj_t *center_clickable = NULL;  /* Clickable area for toggling continuous mode */

/* Battery UI Elements */
static lv_obj_t *battery_icon = NULL;
static lv_obj_t *battery_fill = NULL;
static lv_obj_t *battery_tip = NULL;
static lv_obj_t *battery_percent_label = NULL;

/* System Info Screen UI Elements */
static lv_obj_t *sysinfo_label = NULL;
static lv_obj_t *shutdown_btn = NULL;

/* AP Scanner Screen UI Elements */
static lv_obj_t *ap_scanner_list = NULL;

/* FTM Configuration Settings */
static struct {
    uint8_t frame_count;    // Default: 32 (range: 8-64)
    uint8_t burst_period;   // Default: 2 (range: 2-255, x100ms)
    uint8_t channel;        // Default: 0 (0 = auto, 1-14 = specific channel)
    uint8_t bandwidth;      // Default: 20 (20 or 40 MHz)
} g_ftm_config = {32, 2, 0, 20};

/* Settings Screen UI Elements */
static lv_obj_t *settings_frame_slider = NULL;
static lv_obj_t *settings_frame_label = NULL;
static lv_obj_t *settings_burst_slider = NULL;
static lv_obj_t *settings_burst_label = NULL;
static lv_obj_t *settings_channel_label = NULL;
static lv_obj_t *settings_bandwidth_label = NULL;

/* Responder Screen UI Elements */
static lv_obj_t *responder_status_label = NULL;
static lv_obj_t *responder_ssid_label = NULL;
static lv_obj_t *responder_password_label = NULL;
static lv_obj_t *responder_channel_label = NULL;
static lv_obj_t *responder_bandwidth_label = NULL;
static lv_obj_t *responder_btn = NULL;
static lv_obj_t *responder_btn_label = NULL;

/* Display Handle */
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static SemaphoreHandle_t lvgl_mux = NULL;

/* Touch Handle */
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t touch_dev_handle = NULL;
static bool touch_initialized = false;

/* Touch Callbacks */
static ftm_touch_callback_t scan_callback = NULL;
static ftm_touch_callback_t ftm_callback = NULL;
static ftm_touch_callback_t ftm_long_press_callback = NULL;
static ftm_touch_callback_t center_click_callback = NULL;
static ftm_touch_callback_t shutdown_callback = NULL;

/* Swipe Gesture Detection State */
#define GESTURE_MIN_DISTANCE    80     // Minimum swipe distance in pixels
#define GESTURE_MAX_TIME_MS     800    // Maximum swipe duration in milliseconds
#define GESTURE_ANGLE_TOLERANCE 30.0f  // Degrees from vertical (0° = perfect vertical)

typedef struct {
    bool is_touching;           // Touch currently active
    lv_point_t touch_start;     // Touch start position
    lv_point_t touch_current;   // Current touch position
    uint32_t touch_start_time;  // Touch start timestamp (ms)
    bool gesture_detected;      // Gesture already detected this touch
} gesture_state_t;

static gesture_state_t g_gesture_state = {0};

/* Screen Manager Types - Must be defined before UI update queue */
typedef enum {
    SCREEN_HOME = 0,           // Dashboard
    SCREEN_AP_SCANNER,         // AP list browser
    SCREEN_SETTINGS,           // FTM configuration
    SCREEN_RESPONDER,          // AP responder mode
    SCREEN_SYSTEM_INFO,        // Battery & system info
    SCREEN_COUNT
} ftm_screen_id_t;

/* UI Update Queue - for thread-safe deferred updates */
#define UI_UPDATE_QUEUE_SIZE 16

typedef enum {
    UI_UPDATE_STATUS,
    UI_UPDATE_STATUS_TEXT,
    UI_UPDATE_DISTANCE,
    UI_UPDATE_DISTANCE_ERROR,
    UI_UPDATE_AP_NAME,
    UI_UPDATE_AP_INFO,
    UI_UPDATE_BATTERY,
    UI_UPDATE_CONTINUOUS_INDICATOR,
    UI_UPDATE_SHOW_FTM_BUTTON,
    UI_UPDATE_HIDE_FTM_BUTTON,
    UI_UPDATE_SHOW_BATTERY,
    UI_UPDATE_HIDE_BATTERY,
    UI_NAVIGATE_TO_SCREEN,
} ui_update_type_t;

typedef struct {
    ui_update_type_t type;
    union {
        struct {
            ftm_display_status_t status;
        } status_update;

        struct {
            char text[32];  /* Reduced from 64 - longest message is ~30 chars */
            lv_color_t color;
        } status_text_update;

        struct {
            uint32_t distance_cm;
            uint32_t rtt_nsec;
        } distance_update;

        struct {
            char ap_name[33];
        } ap_name_update;

        struct {
            char ssid[33];
            int8_t rssi;
            uint8_t channel;
        } ap_info_update;

        struct {
            uint32_t voltage_mv;
            uint8_t percentage;
        } battery_update;

        struct {
            bool active;
        } continuous_indicator_update;

        struct {
            ftm_screen_id_t screen_id;
        } navigate_update;
    } data;
} ui_update_msg_t;

static QueueHandle_t ui_update_queue = NULL;

/* Screen Manager - Multi-screen navigation system */
typedef struct {
    lv_obj_t *screen;          // LVGL screen object (NULL if not created)
    bool is_created;           // Track if screen has been initialized
    void (*create_fn)(void);   // Function to create screen content
    void (*update_fn)(void);   // Function to refresh screen data (optional)
} ftm_screen_t;

static ftm_screen_t g_screens[SCREEN_COUNT] = {0};
static ftm_screen_id_t g_current_screen = SCREEN_HOME;

/* Forward declarations of screen creation functions */
static void screen_home_create(void);
static void screen_ap_scanner_create(void);
static void screen_settings_create(void);
static void screen_responder_create(void);
static void screen_system_info_create(void);

/* Forward declarations of screen update functions */
static void screen_home_update(void);
static void screen_system_info_update(void);
static void screen_ap_scanner_update(void);
static void screen_responder_update(void);

/* Forward declaration for event handlers */
static void ap_item_clicked_cb(lv_event_t *e);

/* Callback for LVGL flush completion */
static bool lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

/* LVGL Display Flush Callback - with X offset like Waveshare example */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t) drv->user_data;
    const int offsetx1 = area->x1 + 0x06;  // X offset like Waveshare example
    const int offsetx2 = area->x2 + 0x06;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

    esp_lcd_panel_draw_bitmap(panel, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

/* LVGL rounder callback - align to even pixels */
static void lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

/* LVGL Timer Callback */
static void lvgl_tick_timer_cb(void *arg)
{
    lv_tick_inc(2);  // 2ms tick like Waveshare
}

/* I2C Read from CST820 Touch Controller - based on Waveshare code */
static uint8_t cst820_get_coords(uint16_t *x, uint16_t *y)
{
    if (!touch_initialized || touch_dev_handle == NULL) {
        return 0;
    }

    uint8_t gesture_num[2] = {0, 0};
    uint8_t event = 0x01;
    uint8_t gpos[4] = {0};

    /* Read gesture and number of touch points */
    esp_err_t ret = i2c_master_transmit_receive(touch_dev_handle,
                                                 (const uint8_t[]){0x02}, 1,
                                                 gesture_num, 2, 100);
    if (ret != ESP_OK) {
        return 0;
    }

    event = gesture_num[1] >> 6;

    /* If we have touch points and event is valid */
    if (gesture_num[0] && (event != 0x01)) {
        /* Read position data */
        ret = i2c_master_transmit_receive(touch_dev_handle,
                                          (const uint8_t[]){0x03}, 1,
                                          gpos, 4, 100);
        if (ret != ESP_OK) {
            return 0;
        }

        *x = (((uint16_t)gpos[0] & 0x0f) << 8 | gpos[1]);
        *y = (((uint16_t)gpos[2] & 0x0f) << 8 | gpos[3]);
        return 1;
    }

    return 0;
}

/* Gesture Evaluation - Called on touch release */
static void gesture_evaluate_and_trigger(void)
{
    if (g_gesture_state.gesture_detected) {
        return;  // Already handled this gesture
    }

    /* Calculate gesture metrics */
    int16_t dx = g_gesture_state.touch_current.x - g_gesture_state.touch_start.x;
    int16_t dy = g_gesture_state.touch_current.y - g_gesture_state.touch_start.y;

    float distance = sqrtf(dx * dx + dy * dy);
    uint32_t duration_ms = esp_timer_get_time() / 1000 - g_gesture_state.touch_start_time;

    /* Calculate angle from vertical (0° = perfect vertical swipe) */
    float angle_from_vertical = (float)fabs(atan2f(fabsf((float)dx), fabsf((float)dy)) * 180.0f / M_PI);

    ESP_LOGD(TAG, "Gesture: distance=%.1f, duration=%lu ms, angle=%.1f°, dy=%d",
             distance, duration_ms, angle_from_vertical, dy);

    /* Check if gesture meets swipe criteria */
    if (distance >= GESTURE_MIN_DISTANCE &&
        duration_ms <= GESTURE_MAX_TIME_MS &&
        angle_from_vertical <= GESTURE_ANGLE_TOLERANCE) {

        /* Determine swipe direction based on vertical component */
        if (dy < 0) {
            /* Swipe up = next screen */
            ESP_LOGI(TAG, "Swipe UP detected (distance=%.1f, angle=%.1f°)", distance, angle_from_vertical);
            ftm_screen_navigate_next();
            g_gesture_state.gesture_detected = true;
        } else if (dy > 0) {
            /* Swipe down = previous screen */
            ESP_LOGI(TAG, "Swipe DOWN detected (distance=%.1f, angle=%.1f°)", distance, angle_from_vertical);
            ftm_screen_navigate_prev();
            g_gesture_state.gesture_detected = true;
        }
    }
}

/* Touch Input Read Callback */
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;

    if (cst820_get_coords(&x, &y)) {
        /* Try different coordinate mappings - comment/uncomment to test */

        /* Option 1: Waveshare style (both inverted) - used when display is not rotated */
        // data->point.x = (LCD_H_RES - x);
        // data->point.y = (LCD_V_RES - y);

        /* Option 2: No inversion (direct mapping) - CURRENT: for 180° rotated display */
        data->point.x = x;
        data->point.y = y;

        /* Option 3: X only inverted */
        // data->point.x = (LCD_H_RES - x);
        // data->point.y = y;

        /* Option 4: Y only inverted */
        // data->point.x = x;
        // data->point.y = (LCD_V_RES - y);

        /* Option 5: Swap X and Y */
        // data->point.x = y;
        // data->point.y = x;

        /* Option 6: Swap X and Y, both inverted */
        // data->point.x = (LCD_V_RES - y);
        // data->point.y = (LCD_H_RES - x);

        /* Clamp to screen boundaries */
        if (data->point.x > LCD_H_RES)
            data->point.x = LCD_H_RES;
        if (data->point.y > LCD_V_RES)
            data->point.y = LCD_V_RES;

        data->state = LV_INDEV_STATE_PRESSED;

        /* Gesture tracking: Record touch start or update current position */
        if (!g_gesture_state.is_touching) {
            /* New touch started */
            g_gesture_state.is_touching = true;
            g_gesture_state.touch_start.x = data->point.x;
            g_gesture_state.touch_start.y = data->point.y;
            g_gesture_state.touch_current.x = data->point.x;
            g_gesture_state.touch_current.y = data->point.y;
            g_gesture_state.touch_start_time = esp_timer_get_time() / 1000;  // Convert to milliseconds
            g_gesture_state.gesture_detected = false;
        } else {
            /* Touch move - update current position */
            g_gesture_state.touch_current.x = data->point.x;
            g_gesture_state.touch_current.y = data->point.y;
        }

        // ESP_LOGI(TAG, "Touch: raw(%d,%d) -> mapped(%d,%d)", x, y, data->point.x, data->point.y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;

        /* Gesture tracking: Evaluate gesture on touch release */
        if (g_gesture_state.is_touching) {
            gesture_evaluate_and_trigger();
            g_gesture_state.is_touching = false;
        }
    }
}

/* Helper function to calculate angle from center of screen to a point */
static float calculate_angle(int16_t x, int16_t y)
{
    int16_t center_x = LCD_H_RES / 2;
    int16_t center_y = LCD_V_RES / 2;
    int16_t dx = x - center_x;
    int16_t dy = y - center_y;

    /* Calculate angle in degrees (0° = right, increases counter-clockwise) */
    float angle = atan2f(dy, dx) * 180.0f / M_PI;

    /* Convert to LVGL's angle system (0° = top, increases clockwise) */
    angle = 90.0f - angle;

    /* Normalize to 0-360 range */
    if (angle < 0) angle += 360.0f;
    if (angle >= 360.0f) angle -= 360.0f;

    return angle;
}

/* Center Area Click Event Handler */
static void center_area_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        /* Get touch point for logging */
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        ESP_LOGI(TAG, "*** CENTER AREA CLICKED at (%d,%d) ***", point.x, point.y);

        /* Note: We're already holding lvgl_mux here since we're in an LVGL event callback.
         * The callback registration happens from main task, so no additional semaphore
         * handling is needed for the callback invocation itself.
         */
        if (center_click_callback) {
            center_click_callback();
        } else {
            ESP_LOGW(TAG, "Center click callback not set");
        }
    }
}

/* Unified Button Event Handler - routes clicks based on angle */
static void button_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    /* Filter out hits in the center area during hit test phase */
    if (code == LV_EVENT_HIT_TEST) {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        /* Calculate distance from center */
        int16_t center_x = LCD_H_RES / 2;
        int16_t center_y = LCD_V_RES / 2;
        int16_t dx = point.x - center_x;
        int16_t dy = point.y - center_y;
        float distance = sqrtf(dx * dx + dy * dy);
        float screen_radius = LCD_H_RES / 2.0f;

        /* If click is in center area, tell LVGL this object shouldn't handle it */
        if (distance < screen_radius * 0.65f) {
            lv_hit_test_info_t *info = lv_event_get_param(e);
            info->res = false;  /* Don't handle this click */
            return;
        }
    }

    /* Handle both click and long-press events */
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        float angle = calculate_angle(point.x, point.y);

        /* Calculate distance from center to ensure we're near the edge */
        int16_t center_x = LCD_H_RES / 2;
        int16_t center_y = LCD_V_RES / 2;
        int16_t dx = point.x - center_x;
        int16_t dy = point.y - center_y;
        float distance = sqrtf(dx * dx + dy * dy);
        float screen_radius = LCD_H_RES / 2.0f;

        const char *event_type = (code == LV_EVENT_CLICKED) ? "Click" : "Long-press";
        ESP_LOGI(TAG, "%s at (%d,%d), angle=%.1f°, dist=%.1f (radius=%.1f)",
                 event_type, point.x, point.y, angle, distance, screen_radius);

        /* Center region - trigger continuous mode toggle */
        if (distance < screen_radius * 0.65f) {
            ESP_LOGI(TAG, "*** CENTER AREA CLICKED at (%.1f, %.1f) ***", distance, screen_radius * 0.65f);
            if (center_click_callback) {
                center_click_callback();
            } else {
                ESP_LOGW(TAG, "Center click callback not set");
            }
            return;
        }

        /* SCAN button covers 225° to 315° (left side, 9 o'clock) */
        if (angle >= 225.0f && angle <= 315.0f) {
            if (code == LV_EVENT_CLICKED) {
                ESP_LOGI(TAG, "*** SCAN BUTTON CLICKED ***");
                if (scan_callback) {
                    scan_callback();
                } else {
                    ESP_LOGW(TAG, "Scan callback not set");
                }
            }
            /* Ignore long-press on SCAN button */
        }
        /* FTM button covers 45° to 135° (right side, 3 o'clock) */
        else if (angle >= 45.0f && angle <= 135.0f) {
            /* Check if FTM button is hidden */
            if (ftm_btn && lv_obj_has_flag(ftm_btn, LV_OBJ_FLAG_HIDDEN)) {
                ESP_LOGI(TAG, "FTM button %s ignored - button is hidden", event_type);
                return;
            }

            if (code == LV_EVENT_CLICKED) {
                ESP_LOGI(TAG, "*** FTM BUTTON CLICKED ***");
                if (ftm_callback) {
                    ftm_callback();
                } else {
                    ESP_LOGW(TAG, "FTM callback not set");
                }
            } else if (code == LV_EVENT_LONG_PRESSED) {
                ESP_LOGI(TAG, "*** FTM BUTTON LONG-PRESSED ***");
                if (ftm_long_press_callback) {
                    ftm_long_press_callback();
                } else {
                    ESP_LOGW(TAG, "FTM long-press callback not set");
                }
            }
        }
        else {
            ESP_LOGI(TAG, "%s ignored - not in button regions (SCAN: 225-315°, FTM: 45-135°)", event_type);
        }
    }
}

/* LVGL Task */
/* Forward declarations of internal update functions */
static void update_status_internal(ftm_display_status_t status);
static void update_status_text_internal(const char *text, lv_color_t color);
static void update_distance_internal(uint32_t distance_cm, uint32_t rtt_nsec);
static void update_distance_error_internal(void);
static void update_ap_name_internal(const char *ap_name);
static void update_ap_info_internal(const char *ssid, int8_t rssi, uint8_t channel);
static void update_battery_internal(uint32_t voltage_mv, uint8_t percentage);
static void update_continuous_indicator_internal(bool active);
static void show_ftm_button_internal(void);
static void hide_ftm_button_internal(void);
static void show_battery_internal(void);
static void hide_battery_internal(void);
static void navigate_to_screen_internal(ftm_screen_id_t screen_id);

/* Process UI update messages from the queue (called within LVGL task with mutex held) */
static void process_ui_update(ui_update_msg_t *msg)
{
    switch (msg->type) {
        case UI_UPDATE_STATUS:
            update_status_internal(msg->data.status_update.status);
            break;

        case UI_UPDATE_STATUS_TEXT:
            update_status_text_internal(msg->data.status_text_update.text,
                                      msg->data.status_text_update.color);
            break;

        case UI_UPDATE_DISTANCE:
            update_distance_internal(msg->data.distance_update.distance_cm,
                                   msg->data.distance_update.rtt_nsec);
            break;

        case UI_UPDATE_DISTANCE_ERROR:
            update_distance_error_internal();
            break;

        case UI_UPDATE_AP_NAME:
            update_ap_name_internal(msg->data.ap_name_update.ap_name);
            break;

        case UI_UPDATE_AP_INFO:
            update_ap_info_internal(msg->data.ap_info_update.ssid,
                                  msg->data.ap_info_update.rssi,
                                  msg->data.ap_info_update.channel);
            break;

        case UI_UPDATE_BATTERY:
            update_battery_internal(msg->data.battery_update.voltage_mv,
                                  msg->data.battery_update.percentage);
            break;

        case UI_UPDATE_CONTINUOUS_INDICATOR:
            update_continuous_indicator_internal(msg->data.continuous_indicator_update.active);
            break;

        case UI_UPDATE_SHOW_FTM_BUTTON:
            show_ftm_button_internal();
            break;

        case UI_UPDATE_HIDE_FTM_BUTTON:
            hide_ftm_button_internal();
            break;

        case UI_UPDATE_SHOW_BATTERY:
            show_battery_internal();
            break;

        case UI_UPDATE_HIDE_BATTERY:
            hide_battery_internal();
            break;

        case UI_NAVIGATE_TO_SCREEN:
            navigate_to_screen_internal(msg->data.navigate_update.screen_id);
            break;

        default:
            ESP_LOGW(TAG, "Unknown UI update type: %d", msg->type);
            break;
    }
}

/* Screen Manager Functions */

/* Internal screen navigation - called with mutex already held */
static void navigate_to_screen_internal(ftm_screen_id_t screen_id)
{
    if (screen_id >= SCREEN_COUNT) {
        ESP_LOGW(TAG, "Invalid screen ID: %d", screen_id);
        return;
    }

    /* Create screen if not already created */
    if (!g_screens[screen_id].is_created) {
        ESP_LOGI(TAG, "Creating screen %d for first time", screen_id);
        g_screens[screen_id].screen = lv_obj_create(NULL);
        if (g_screens[screen_id].screen == NULL) {
            ESP_LOGE(TAG, "Failed to create screen %d - LVGL OOM", screen_id);
            return;
        }
        lv_obj_set_style_bg_color(g_screens[screen_id].screen, lv_color_black(), 0);

        if (g_screens[screen_id].create_fn) {
            g_screens[screen_id].create_fn();
        }
        g_screens[screen_id].is_created = true;
    }

    /* Update screen data before showing */
    if (g_screens[screen_id].update_fn) {
        g_screens[screen_id].update_fn();
    }

    /* Load screen with no animation for MVP */
    lv_scr_load_anim(g_screens[screen_id].screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);

    g_current_screen = screen_id;
    ESP_LOGI(TAG, "Navigated to screen %d", screen_id);
}

/* Initialize Screen Manager - Queue-based API */
esp_err_t ftm_screen_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing screen manager");

    /* Register screen creation functions */
    g_screens[SCREEN_HOME].create_fn = screen_home_create;
    g_screens[SCREEN_HOME].update_fn = screen_home_update;

    g_screens[SCREEN_AP_SCANNER].create_fn = screen_ap_scanner_create;
    g_screens[SCREEN_AP_SCANNER].update_fn = screen_ap_scanner_update;

    g_screens[SCREEN_SETTINGS].create_fn = screen_settings_create;
    g_screens[SCREEN_SETTINGS].update_fn = NULL;  // Settings are interactive

    g_screens[SCREEN_RESPONDER].create_fn = screen_responder_create;
    g_screens[SCREEN_RESPONDER].update_fn = screen_responder_update;

    g_screens[SCREEN_SYSTEM_INFO].create_fn = screen_system_info_create;
    g_screens[SCREEN_SYSTEM_INFO].update_fn = screen_system_info_update;

    ESP_LOGI(TAG, "Screen manager initialized with %d screens", SCREEN_COUNT);
    return ESP_OK;
}

/* Navigate to specific screen - Queue-based API */
esp_err_t ftm_screen_navigate_to(int screen_id)
{
    if (ui_update_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (screen_id < 0 || screen_id >= SCREEN_COUNT) return ESP_ERR_INVALID_ARG;

    ui_update_msg_t msg = {
        .type = UI_NAVIGATE_TO_SCREEN,
        .data.navigate_update.screen_id = (ftm_screen_id_t)screen_id
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping screen navigation");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* Navigate to next screen (for swipe up) - Queue-based API */
esp_err_t ftm_screen_navigate_next(void)
{
    ftm_screen_id_t next = (g_current_screen + 1) % SCREEN_COUNT;
    return ftm_screen_navigate_to(next);
}

/* Navigate to previous screen (for swipe down) - Queue-based API */
esp_err_t ftm_screen_navigate_prev(void)
{
    ftm_screen_id_t prev = (g_current_screen == 0) ?
                           (SCREEN_COUNT - 1) : (g_current_screen - 1);
    return ftm_screen_navigate_to(prev);
}

/* Get current screen ID */
int ftm_screen_get_current(void)
{
    return (int)g_current_screen;
}

/* LVGL Task with Queue Processing and Screen Management */
static void lvgl_task(void *arg)
{
    uint32_t task_delay_ms = 500;
    ESP_LOGI(TAG, "Starting LVGL task");

    while (1) {
        if (xSemaphoreTake(lvgl_mux, portMAX_DELAY) == pdTRUE) {
            /* Process all pending UI updates from queue */
            ui_update_msg_t msg;
            while (xQueueReceive(ui_update_queue, &msg, 0) == pdTRUE) {
                process_ui_update(&msg);
            }

            /* Run LVGL timer handler */
            task_delay_ms = lv_timer_handler();
            xSemaphoreGive(lvgl_mux);
        }

        /* Clamp delay */
        if (task_delay_ms > 500) {
            task_delay_ms = 500;
        } else if (task_delay_ms < 5) {
            task_delay_ms = 5;
        }

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

/* Initialize the Display */
esp_err_t ftm_display_init(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare ESP32-C6-Touch-AMOLED-1.32 display");

    /* Create LVGL mutex */
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL mutex");
        return ESP_FAIL;
    }

    /* Create UI update queue for thread-safe deferred updates */
    ui_update_queue = xQueueCreate(UI_UPDATE_QUEUE_SIZE, sizeof(ui_update_msg_t));
    if (ui_update_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI update queue");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "UI update queue created (size: %d)", UI_UPDATE_QUEUE_SIZE);

    /* Initialize display power control GPIO */
    ESP_LOGI(TAG, "Configuring display power control GPIO (pin %d)", PIN_NUM_LCD_EN);
    gpio_config_t en_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_LCD_EN
    };
    ESP_ERROR_CHECK(gpio_config(&en_gpio_config));
    gpio_set_level(PIN_NUM_LCD_EN, 1);  // Enable display power
    ESP_LOGI(TAG, "Display power enabled");
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Initialize QSPI bus for SH8601 display - match Waveshare config exactly */
    ESP_LOGI(TAG, "Initializing QSPI bus (SPI2_HOST)");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = PIN_NUM_LCD_SCLK;
    buscfg.data0_io_num = PIN_NUM_LCD_SDIO0;
    buscfg.data1_io_num = PIN_NUM_LCD_SDIO1;
    buscfg.data2_io_num = PIN_NUM_LCD_SDIO2;
    buscfg.data3_io_num = PIN_NUM_LCD_SDIO3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "QSPI bus initialized");

    /* Initialize LCD panel IO with QSPI - match Waveshare config */
    ESP_LOGI(TAG, "Initializing LCD panel IO with QSPI");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_NUM_LCD_CS;
    io_config.dc_gpio_num = -1;  // QSPI mode
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;  // 40MHz like Waveshare
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lcd_color_trans_done_cb;
    io_config.user_ctx = &disp_drv;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = 1;  // Enable QSPI mode

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));
    ESP_LOGI(TAG, "LCD panel IO initialized");

    /* Initialize SH8601 panel with vendor config */
    ESP_LOGI(TAG, "Initializing SH8601 AMOLED panel");

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_NUM_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* Turn on the display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "SH8601 panel initialized and turned on successfully");

    /* Initialize I2C bus for CST820 touch controller */
    ESP_LOGI(TAG, "Initializing I2C master bus for CST820 touch controller");

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_NUM,
        .scl_io_num = PIN_NUM_TOUCH_SCL,
        .sda_io_num = PIN_NUM_TOUCH_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t i2c_ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
    if (i2c_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(i2c_ret));
        touch_initialized = false;
    } else {
        /* Add CST820 touch device to I2C bus */
        i2c_device_config_t touch_dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TOUCH_I2C_ADDR,
            .scl_speed_hz = 300000,  // 300kHz like Waveshare
        };

        i2c_ret = i2c_master_bus_add_device(i2c_bus_handle, &touch_dev_config, &touch_dev_handle);
        if (i2c_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add touch device to I2C bus: %s", esp_err_to_name(i2c_ret));
            i2c_del_master_bus(i2c_bus_handle);
            i2c_bus_handle = NULL;
            touch_initialized = false;
        } else {
            touch_initialized = true;
            ESP_LOGI(TAG, "CST820 touch controller initialized successfully");
        }
    }

    /* Initialize LVGL */
    lv_init();

    /* Allocate draw buffers */
    buf1 = (lv_color_t *)heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer 1 (%zu bytes) - insufficient DMA memory",
                 LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t));
        return ESP_ERR_NO_MEM;
    }

    buf2 = (lv_color_t *)heap_caps_malloc(LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer 2 (%zu bytes) - insufficient DMA memory",
                 LCD_H_RES * LVGL_BUF_HEIGHT * sizeof(lv_color_t));
        free(buf1);
        buf1 = NULL;
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * LVGL_BUF_HEIGHT);

    /* Initialize display driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;  // Add rounder like Waveshare
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    disp_drv.sw_rotate = 1;  // Enable software rotation
    disp_drv.rotated = LV_DISP_ROT_180;  // Rotate 180° to fix upside-down display
    disp = lv_disp_drv_register(&disp_drv);

    /* Register touch input device */
    if (touch_initialized) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = lvgl_touch_read_cb;
        lv_indev_drv_register(&indev_drv);
        ESP_LOGI(TAG, "Touch input device registered with LVGL");
    } else {
        ESP_LOGW(TAG, "Touch input device not registered - touch unavailable");
    }

    /* Create LVGL tick timer */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2 * 1000));  // 2ms tick

    /* UI elements are now created by screen_home_create() via screen manager */

    ESP_LOGI(TAG, "Display initialized - SH8601 AMOLED (466x466), Touch CST820 %s",
             touch_initialized ? "enabled" : "disabled");
    return ESP_OK;
}

/* Internal update function - called with mutex already held */
static void update_status_internal(ftm_display_status_t status)
{
    if (status_label == NULL) return;

    lv_color_t color;
    const char *text;

    switch (status) {
        case FTM_DISPLAY_IDLE:
            text = "Ready";
            color = lv_color_make(100, 200, 100);
            break;
        case FTM_DISPLAY_SCANNING:
            text = "Scanning...";
            color = lv_color_make(100, 150, 255);
            break;
        case FTM_DISPLAY_CONNECTING:
            text = "Connecting...";
            color = lv_color_make(255, 200, 100);
            break;
        case FTM_DISPLAY_MEASURING:
            text = "Measuring FTM...";
            color = lv_color_make(255, 255, 100);
            break;
        case FTM_DISPLAY_SUCCESS:
            text = "Success";
            color = lv_color_make(100, 255, 100);
            break;
        case FTM_DISPLAY_FAILED:
            text = "Failed";
            color = lv_color_make(255, 100, 100);
            break;
        case FTM_DISPLAY_AP_MODE:
            text = "AP Mode";
            color = lv_color_make(200, 100, 255);
            break;
        case FTM_DISPLAY_CONTINUOUS:
            text = "Continuous";
            color = lv_color_make(100, 255, 255);
            break;
        default:
            text = "Unknown";
            color = lv_color_make(150, 150, 150);
            break;
    }

    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, color, 0);
}

/* Update Display Status - Queue-based API */
void ftm_display_update_status(ftm_display_status_t status)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_STATUS,
        .data.status_update.status = status
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping status update");
    }
}

/* Internal update function - called with mutex already held */
static void update_status_text_internal(const char *text, lv_color_t color)
{
    if (status_label == NULL) return;

    lv_label_set_text(status_label, text);
    lv_obj_set_style_text_color(status_label, color, 0);
}

/* Update Status with Custom Text - Queue-based API */
void ftm_display_update_status_text(const char *text, lv_color_t color)
{
    if (ui_update_queue == NULL || text == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_STATUS_TEXT,
        .data.status_text_update.color = color
    };

    strlcpy(msg.data.status_text_update.text, text, sizeof(msg.data.status_text_update.text));

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping status text update");
    }
}

/* Internal update function - called with mutex already held */
static void update_distance_internal(uint32_t distance_cm, uint32_t rtt_nsec)
{
    if (distance_label == NULL || rtt_label == NULL) return;

    char dist_str[32];

    /* Simplified format for larger text readability */
    snprintf(dist_str, sizeof(dist_str), "%lu.%02lu m",
             distance_cm / 100, distance_cm % 100);

    lv_label_set_text(distance_label, dist_str);
    lv_obj_set_style_text_color(distance_label, lv_color_white(), 0);  // Reset to white

    /* Note: rtt_label now preserved for channel/RSSI info - don't overwrite it */
    /* RTT info is still available in logs if needed */
}

/* Update Distance Display - Queue-based API */
void ftm_display_update_distance(uint32_t distance_cm, uint32_t rtt_nsec)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_DISTANCE,
        .data.distance_update.distance_cm = distance_cm,
        .data.distance_update.rtt_nsec = rtt_nsec
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping distance update");
    }
}

/* Internal update function - called with mutex already held */
static void update_distance_error_internal(void)
{
    if (distance_label == NULL || rtt_label == NULL) return;

    lv_label_set_text(distance_label, "ERR");
    lv_obj_set_style_text_color(distance_label, lv_color_make(255, 100, 100), 0);
    lv_label_set_text(rtt_label, "Error");
}

/* Update Distance Display to Show Error - Queue-based API */
void ftm_display_update_distance_error(void)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_DISTANCE_ERROR
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping distance error update");
    }
}

/* Internal update function - called with mutex already held */
static void update_ap_name_internal(const char *ap_name)
{
    if (ap_name_label == NULL) return;

    if (ap_name != NULL && strlen(ap_name) > 0) {
        lv_label_set_text(ap_name_label, ap_name);
    } else {
        lv_label_set_text(ap_name_label, "");
    }
}

/* Update AP Name Display - Queue-based API */
void ftm_display_update_ap_name(const char *ap_name)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_AP_NAME
    };

    if (ap_name != NULL) {
        strlcpy(msg.data.ap_name_update.ap_name, ap_name, sizeof(msg.data.ap_name_update.ap_name));
    } else {
        msg.data.ap_name_update.ap_name[0] = '\0';
    }

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping AP name update");
    }
}

/* Internal update function - called with mutex already held */
static void update_ap_info_internal(const char *ssid, int8_t rssi, uint8_t channel)
{
    if (ap_name_label == NULL || rtt_label == NULL) return;

    char channel_rssi_str[32];
    snprintf(channel_rssi_str, sizeof(channel_rssi_str), "Ch:%d | RSSI:%ddBm", channel, rssi);

    lv_label_set_text(ap_name_label, ssid);  // Update centered AP name label
    lv_label_set_text(rtt_label, channel_rssi_str);  // Show channel/RSSI below AP name
}

/* Update AP Information - Queue-based API */
void ftm_display_update_ap_info(const char *ssid, int8_t rssi, uint8_t channel)
{
    if (ui_update_queue == NULL || ssid == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_AP_INFO,
        .data.ap_info_update.rssi = rssi,
        .data.ap_info_update.channel = channel
    };

    strlcpy(msg.data.ap_info_update.ssid, ssid, sizeof(msg.data.ap_info_update.ssid));

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping AP info update");
    }
}

/* Update Mode Display */
void ftm_display_update_mode(const char *mode_str)
{
    /* Mode label removed - mode info shown in status label */
    (void)mode_str;
}

/* Clear Display */
void ftm_display_clear(void)
{
    if (lvgl_mux == NULL) return;

    if (xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (distance_label) lv_label_set_text(distance_label, "-- m");
        if (ap_name_label) lv_label_set_text(ap_name_label, "");
        if (rtt_label) lv_label_set_text(rtt_label, "RTT: --");
        if (ap_info_label) lv_label_set_text(ap_info_label, "");
        xSemaphoreGive(lvgl_mux);
    }
}

/* Start Display Task */
void ftm_display_task_start(void)
{
    xTaskCreate(lvgl_task, "lvgl_task", 6 * 1024, NULL, 2, NULL);
}

/* Set Scan Button Callback */
void ftm_display_set_scan_callback(ftm_touch_callback_t callback)
{
    scan_callback = callback;
    ESP_LOGI(TAG, "Scan callback registered");
}

/* Set FTM Button Callback */
void ftm_display_set_ftm_callback(ftm_touch_callback_t callback)
{
    ftm_callback = callback;
    ESP_LOGI(TAG, "FTM callback registered");
}

void ftm_display_set_ftm_long_press_callback(ftm_touch_callback_t callback)
{
    ftm_long_press_callback = callback;
    ESP_LOGI(TAG, "FTM long-press callback registered");
}

/* Register Center Click Callback */
void ftm_display_set_center_click_callback(ftm_touch_callback_t callback)
{
    center_click_callback = callback;
    ESP_LOGI(TAG, "Center click callback registered");
}

/* Register Shutdown Callback */
void ftm_display_set_shutdown_callback(ftm_touch_callback_t callback)
{
    shutdown_callback = callback;
    ESP_LOGI(TAG, "Shutdown callback registered");
}

/* Internal update function - called with mutex already held */
static void show_ftm_button_internal(void)
{
    if (ftm_btn) lv_obj_clear_flag(ftm_btn, LV_OBJ_FLAG_HIDDEN);
    if (ftm_btn_label) lv_obj_clear_flag(ftm_btn_label, LV_OBJ_FLAG_HIDDEN);
}

/* Show FTM Button - Queue-based API */
void ftm_display_show_ftm_button(void)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_SHOW_FTM_BUTTON
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping show FTM button update");
    }
}

/* Internal update function - called with mutex already held */
static void hide_ftm_button_internal(void)
{
    if (ftm_btn) lv_obj_add_flag(ftm_btn, LV_OBJ_FLAG_HIDDEN);
    if (ftm_btn_label) lv_obj_add_flag(ftm_btn_label, LV_OBJ_FLAG_HIDDEN);
}

/* Hide FTM Button - Queue-based API */
void ftm_display_hide_ftm_button(void)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_HIDE_FTM_BUTTON
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping hide FTM button update");
    }
}

/* Internal update function - called with mutex already held */
static void update_continuous_indicator_internal(bool active)
{
    if (ftm_btn_label == NULL) return;

    if (active) {
        /* Pure blue for continuous mode */
        lv_obj_set_style_text_color(ftm_btn_label, lv_color_make(0, 200, 255), 0);
    } else {
        /* White for normal mode */
        lv_obj_set_style_text_color(ftm_btn_label, lv_color_white(), 0);
    }
}

/* Set FTM Button Continuous Mode Indicator - Queue-based API */
void ftm_display_set_continuous_indicator(bool active)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_CONTINUOUS_INDICATOR,
        .data.continuous_indicator_update.active = active
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping continuous indicator update");
    }
}

/* Internal update function - called with mutex already held */
static void update_battery_internal(uint32_t voltage_mv, uint8_t percentage)
{
    if (battery_icon == NULL) return;

    /* Update percentage label */
    char percent_text[8];
    snprintf(percent_text, sizeof(percent_text), "%d%%", percentage);
    lv_label_set_text(battery_percent_label, percent_text);

    /* Calculate fill width based on percentage (max 40 pixels) */
    uint8_t fill_width = (percentage * 40) / 100;
    if (fill_width > 40) fill_width = 40;
    if (fill_width < 2) fill_width = 2;  /* Minimum 2px so it's visible */
    lv_obj_set_width(battery_fill, fill_width);

    /* Update color based on battery level */
    lv_color_t fill_color;
    if (percentage > 50) {
        /* Green for good battery (>50%) */
        fill_color = lv_color_make(100, 200, 100);
    } else if (percentage > 20) {
        /* Yellow/orange for medium battery (20-50%) */
        fill_color = lv_color_make(255, 200, 0);
    } else {
        /* Red for low battery (<20%) */
        fill_color = lv_color_make(255, 50, 50);
    }
    lv_obj_set_style_bg_color(battery_fill, fill_color, 0);
}

/* Update Battery Display - Queue-based API */
void ftm_display_update_battery(uint32_t voltage_mv, uint8_t percentage)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_BATTERY,
        .data.battery_update.voltage_mv = voltage_mv,
        .data.battery_update.percentage = percentage
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping battery update");
    }
}

/* Internal update function - called with mutex already held */
static void show_battery_internal(void)
{
    if (battery_icon == NULL) return;

    lv_obj_clear_flag(battery_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);
}

/* Show Battery Icon - Queue-based API */
void ftm_display_show_battery(void)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_SHOW_BATTERY
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping show battery update");
    }
}

/* Internal update function - called with mutex already held */
static void hide_battery_internal(void)
{
    if (battery_icon == NULL) return;

    lv_obj_add_flag(battery_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);
}

/* Hide Battery Icon - Queue-based API */
void ftm_display_hide_battery(void)
{
    if (ui_update_queue == NULL) return;

    ui_update_msg_t msg = {
        .type = UI_UPDATE_HIDE_BATTERY
    };

    if (xQueueSend(ui_update_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "UI update queue full, dropping hide battery update");
    }
}

/* ============================================================================
 * FTM Configuration Getters
 * ============================================================================ */

uint8_t ftm_display_get_frame_count(void)
{
    return g_ftm_config.frame_count;
}

uint8_t ftm_display_get_burst_period(void)
{
    return g_ftm_config.burst_period;
}

uint8_t ftm_display_get_channel(void)
{
    return g_ftm_config.channel;
}

uint8_t ftm_display_get_bandwidth(void)
{
    return g_ftm_config.bandwidth;
}

/* ============================================================================
 * Screen Creation Functions
 * ============================================================================ */

/* SCREEN_HOME: Dashboard with current measurement and controls */
static void screen_home_create(void)
{
    lv_obj_t *scr = g_screens[SCREEN_HOME].screen;
    lv_obj_t *title_label = NULL;
    ESP_LOGI(TAG, "Creating HOME screen");

    /* Title Label - Top of screen */
    LV_CREATE_CHECK(title_label, lv_label_create(scr));
    lv_label_set_text(title_label, "WiFi FTM");
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 40);

    /* Status Label - Below title */
    LV_CREATE_CHECK(status_label, lv_label_create(scr));
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, lv_color_make(100, 200, 100), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 80);

    /* Distance Label - CENTER OF SCREEN, very prominent */
    LV_CREATE_CHECK(distance_label, lv_label_create(scr));
    lv_label_set_text(distance_label, "0.00 m");
    lv_obj_set_style_text_color(distance_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(distance_label, &lv_font_montserrat_48, 0);
    lv_obj_align(distance_label, LV_ALIGN_CENTER, 0, 0);  /* Dead center */

    /* Make distance label clickable for continuous mode toggle */
    lv_obj_add_flag(distance_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(distance_label, center_area_event_cb, LV_EVENT_CLICKED, NULL);
    ESP_LOGI(TAG, "Distance label made clickable for continuous mode toggle");

    /* AP Name Label - Shows which AP was used for ranging */
    LV_CREATE_CHECK(ap_name_label, lv_label_create(scr));
    lv_label_set_text(ap_name_label, "");
    lv_obj_set_style_text_color(ap_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ap_name_label, &lv_font_montserrat_20, 0);
    lv_obj_align(ap_name_label, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_text_align(ap_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ap_name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ap_name_label, 350);

    /* RTT Label - Below AP name */
    LV_CREATE_CHECK(rtt_label, lv_label_create(scr));
    lv_label_set_text(rtt_label, "");
    lv_obj_set_style_text_color(rtt_label, lv_color_make(150, 150, 150), 0);
    lv_obj_set_style_text_font(rtt_label, &lv_font_montserrat_20, 0);
    lv_obj_align(rtt_label, LV_ALIGN_CENTER, 0, 80);

    /* AP Info Label - Above buttons area */
    LV_CREATE_CHECK(ap_info_label, lv_label_create(scr));
    lv_label_set_text(ap_info_label, "");
    lv_obj_set_style_text_color(ap_info_label, lv_color_make(120, 120, 180), 0);
    lv_obj_set_style_text_font(ap_info_label, &lv_font_montserrat_16, 0);
    lv_obj_align(ap_info_label, LV_ALIGN_BOTTOM_MID, 0, -105);
    lv_label_set_long_mode(ap_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ap_info_label, 350);

    /* Mode Label - removed, not needed */
    mode_label = NULL;

    /* Create touch buttons if touch controller initialized successfully */
    if (touch_initialized) {
        /* Left Arc Button (SCAN) - Left side arc (225° to 315° touch, 135° to 225° visual) */
        LV_CREATE_CHECK(scan_btn, lv_arc_create(scr));
        lv_arc_set_bg_angles(scan_btn, 135, 225);  /* Visual: bottom-left (rotated -90° from touch) */
        lv_arc_set_rotation(scan_btn, 0);
        lv_obj_set_size(scan_btn, 466, 466);
        lv_obj_center(scan_btn);
        ESP_LOGI(TAG, "Created SCAN button: visual angles 135-225, touch angles 225-315, size 466x466");

        /* Make arc invisible to eliminate rendering overhead - keep only touch functionality */
        lv_obj_set_style_opa(scan_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_opa(scan_btn, LV_OPA_TRANSP, LV_PART_INDICATOR);

        /* Hide the knob */
        lv_obj_set_style_bg_opa(scan_btn, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(scan_btn, 0, LV_PART_KNOB);

        /* Configure as clickable button (disable arc adjustment) */
        lv_arc_set_value(scan_btn, 100);
        lv_arc_set_mode(scan_btn, LV_ARC_MODE_NORMAL);
        lv_arc_set_change_rate(scan_btn, 0);  /* Disable value changes */
        lv_obj_clear_flag(scan_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_style(scan_btn, NULL, LV_STATE_EDITED);  /* Remove edited state styling */
        lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(scan_btn, button_event_cb, LV_EVENT_HIT_TEST, NULL);  /* Filter center clicks */
        lv_obj_add_event_cb(scan_btn, button_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(scan_btn, button_event_cb, LV_EVENT_LONG_PRESSED, NULL);
        ESP_LOGI(TAG, "SCAN button configured as non-adjustable with unified event callback");

        /* Scan button label - positioned at 9 o'clock (left center) */
        LV_CREATE_CHECK(scan_btn_label, lv_label_create(scr));
        lv_label_set_text(scan_btn_label, "SCAN");
        lv_obj_set_style_text_color(scan_btn_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(scan_btn_label, &lv_font_montserrat_24, 0);
        lv_obj_align(scan_btn_label, LV_ALIGN_LEFT_MID, 30, 0);

        /* Right Arc Button (FTM) - Right side arc (45° to 135° touch, 315° to 45° visual) */
        LV_CREATE_CHECK(ftm_btn, lv_arc_create(scr));
        lv_arc_set_bg_angles(ftm_btn, 315, 45);  /* Visual: bottom-right (rotated -90° from touch) */
        lv_arc_set_rotation(ftm_btn, 0);
        lv_obj_set_size(ftm_btn, 466, 466);
        lv_obj_center(ftm_btn);
        ESP_LOGI(TAG, "Created FTM button: visual angles 315-45, touch angles 45-135, size 466x466");

        /* Make arc invisible to eliminate rendering overhead - keep only touch functionality */
        lv_obj_set_style_opa(ftm_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_opa(ftm_btn, LV_OPA_TRANSP, LV_PART_INDICATOR);

        /* Hide the knob */
        lv_obj_set_style_bg_opa(ftm_btn, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(ftm_btn, 0, LV_PART_KNOB);

        /* Configure as clickable button (disable arc adjustment) */
        lv_arc_set_value(ftm_btn, 100);
        lv_arc_set_mode(ftm_btn, LV_ARC_MODE_NORMAL);
        lv_arc_set_change_rate(ftm_btn, 0);  /* Disable value changes */
        lv_obj_clear_flag(ftm_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_style(ftm_btn, NULL, LV_STATE_EDITED);  /* Remove edited state styling */
        lv_obj_add_flag(ftm_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ftm_btn, button_event_cb, LV_EVENT_HIT_TEST, NULL);  /* Filter center clicks */
        lv_obj_add_event_cb(ftm_btn, button_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ftm_btn, button_event_cb, LV_EVENT_LONG_PRESSED, NULL);
        ESP_LOGI(TAG, "FTM button configured as non-adjustable with unified event callback");

        /* FTM button label - positioned at 3 o'clock (right center) */
        LV_CREATE_CHECK(ftm_btn_label, lv_label_create(scr));
        lv_label_set_text(ftm_btn_label, "FTM");
        lv_obj_set_style_text_color(ftm_btn_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(ftm_btn_label, &lv_font_montserrat_24, 0);
        lv_obj_align(ftm_btn_label, LV_ALIGN_RIGHT_MID, -30, 0);

        /* Hide FTM button and label until scan completes */
        lv_obj_add_flag(ftm_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ftm_btn_label, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG, "Arc buttons created at bottom of round display (FTM button hidden until scan)");

        /* Center Clickable Area - for toggling continuous mode
         * Created AFTER arc buttons so it's on top in z-order */
        LV_CREATE_CHECK(center_clickable, lv_obj_create(scr));
        lv_obj_set_size(center_clickable, 200, 200);  /* Cover center display area */
        lv_obj_align(center_clickable, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_opa(center_clickable, LV_OPA_TRANSP, 0);  /* Transparent background */
        lv_obj_set_style_border_width(center_clickable, 0, 0);  /* No border */
        lv_obj_add_flag(center_clickable, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(center_clickable, LV_OBJ_FLAG_SCROLLABLE);  /* Prevent scrolling */
        lv_obj_move_foreground(center_clickable);  /* Ensure it's in front for click priority */
        lv_obj_add_event_cb(center_clickable, center_area_event_cb, LV_EVENT_CLICKED, NULL);
        ESP_LOGI(TAG, "Center clickable area created (200x200px) on top of arc buttons");
    } else {
        ESP_LOGW(TAG, "Touch disabled - buttons not created. Use console commands.");
    }

    /* Create battery icon at center-bottom of screen (safe for round display) */
    /* Battery body (outline) */
    battery_icon = lv_obj_create(scr);
    lv_obj_set_size(battery_icon, 50, 25);
    lv_obj_align(battery_icon, LV_ALIGN_BOTTOM_MID, 0, -15);  /* Center bottom, 15px from edge */
    lv_obj_set_style_border_width(battery_icon, 2, 0);
    lv_obj_set_style_border_color(battery_icon, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(battery_icon, 3, 0);
    lv_obj_clear_flag(battery_icon, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery tip (positive terminal) */
    battery_tip = lv_obj_create(scr);
    lv_obj_set_size(battery_tip, 4, 12);
    lv_obj_align_to(battery_tip, battery_icon, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip, 0, 0);
    lv_obj_set_style_radius(battery_tip, 2, 0);
    lv_obj_clear_flag(battery_tip, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery fill (shows charge level) */
    battery_fill = lv_obj_create(battery_icon);
    lv_obj_set_size(battery_fill, 40, 17);  /* Slightly larger fill */
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(battery_fill, lv_color_make(100, 200, 100), 0);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_fill, 0, 0);
    lv_obj_set_style_radius(battery_fill, 2, 0);
    lv_obj_clear_flag(battery_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* Battery percentage label - place above battery icon */
    battery_percent_label = lv_label_create(scr);
    lv_label_set_text(battery_percent_label, "100%");
    lv_obj_set_style_text_color(battery_percent_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_14, 0);
    lv_obj_align_to(battery_percent_label, battery_icon, LV_ALIGN_OUT_TOP_MID, 0, -3);

    /* Initially hide battery UI until first update */
    lv_obj_add_flag(battery_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_percent_label, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Battery icon created (hidden until first update)");
}

static void screen_home_update(void)
{
    /* Placeholder: Will update based on current FTM state */
    ESP_LOGD(TAG, "Updating HOME screen");
}

/* AP item click handler - starts FTM to selected AP */
static void ap_item_clicked_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    /* Get the AP index from user data */
    uint16_t ap_index = (uint16_t)(uintptr_t)lv_event_get_user_data(e);

    if (!g_ap_list_buffer || ap_index >= g_scan_ap_num) {
        ESP_LOGW(TAG, "Invalid AP index: %d", ap_index);
        return;
    }

    wifi_ap_record_t *selected_ap = &g_ap_list_buffer[ap_index];
    ESP_LOGI(TAG, "AP selected: %s (RSSI: %d, CH: %d, FTM: %s)",
             selected_ap->ssid,
             selected_ap->rssi,
             selected_ap->primary,
             selected_ap->ftm_responder ? "Yes" : "No");

    /* Store the selected AP index for the FTM callback to use */
    ftm_set_selected_ap_index(ap_index);

    /* Navigate back to HOME screen and trigger FTM callback */
    ftm_screen_navigate_to(SCREEN_HOME);

    /* Trigger the FTM callback if set (this will start the FTM measurement) */
    if (ftm_callback) {
        ftm_callback();
    }
}

/* SCREEN_AP_SCANNER: AP list browser */
static void screen_ap_scanner_create(void)
{
    lv_obj_t *scr = g_screens[SCREEN_AP_SCANNER].screen;
    lv_obj_t *title = NULL;
    lv_obj_t *hint = NULL;
    ESP_LOGI(TAG, "Creating AP_SCANNER screen");

    /* Title */
    LV_CREATE_CHECK(title, lv_label_create(scr));
    lv_label_set_text(title, "Select Access Point");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* Create scrollable list - transparent background, smaller height for easier swiping */
    LV_CREATE_CHECK(ap_scanner_list, lv_list_create(scr));
    lv_obj_set_size(ap_scanner_list, 400, 300);  /* Reduced from 380 to 300 for swipe zones */
    lv_obj_align(ap_scanner_list, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(ap_scanner_list, LV_OPA_TRANSP, 0);  /* Invisible background */
    lv_obj_set_style_border_width(ap_scanner_list, 0, 0);  /* No border */

    /* Hint text at bottom */
    LV_CREATE_CHECK(hint, lv_label_create(scr));
    lv_label_set_text(hint, "Tap AP to measure");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_make(120, 120, 120), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    ESP_LOGI(TAG, "AP_SCANNER screen created");
}

/* Comparison function for sorting APs: FTM responders first, then by RSSI */
static int ap_sort_compare(const void *a, const void *b)
{
    const wifi_ap_record_t *ap_a = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *ap_b = (const wifi_ap_record_t *)b;

    /* Check if AP is FTM capable (either flag from scan OR whitelist match) */
    bool is_ftm_a = ap_a->ftm_responder || is_known_ftm_responder((const char*)ap_a->ssid);
    bool is_ftm_b = ap_b->ftm_responder || is_known_ftm_responder((const char*)ap_b->ssid);

    /* FTM capable APs first */
    if (is_ftm_a && !is_ftm_b) {
        return -1;  // a comes before b
    }
    if (!is_ftm_a && is_ftm_b) {
        return 1;   // b comes before a
    }

    /* Both same FTM status, sort by RSSI (stronger signal first) */
    if (ap_a->rssi > ap_b->rssi) {
        return -1;  // a comes before b (stronger signal)
    }
    if (ap_a->rssi < ap_b->rssi) {
        return 1;   // b comes before a
    }

    return 0;  // Equal
}

static void screen_ap_scanner_update(void)
{
    if (ap_scanner_list == NULL) return;

    /* Clear existing list items */
    lv_obj_clean(ap_scanner_list);

    /* Check if we have scan results */
    if (!g_ap_list_buffer || g_scan_ap_num == 0) {
        /* Show "No APs found" message */
        lv_obj_t *btn = lv_list_add_btn(ap_scanner_list, LV_SYMBOL_WARNING, "No APs found - Scan first");

        /* Set black background and white text - only default state to save memory */
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 1), lv_color_white(), 0);

        ESP_LOGD(TAG, "AP_SCANNER updated: No scan results");
        return;
    }

    /* Sort APs: FTM responders first, then by RSSI (strongest first) */
    qsort(g_ap_list_buffer, g_scan_ap_num, sizeof(wifi_ap_record_t), ap_sort_compare);

    /* Limit to top 10 APs to save memory */
    uint16_t ap_count = (g_scan_ap_num > 10) ? 10 : g_scan_ap_num;

    /* Populate list with sorted scan results (max 10) */
    for (uint16_t i = 0; i < ap_count; i++) {
        /* Check if AP is FTM capable (either flag or whitelist) */
        bool is_ftm = g_ap_list_buffer[i].ftm_responder ||
                      is_known_ftm_responder((const char*)g_ap_list_buffer[i].ssid);

        char buf[80];
        snprintf(buf, sizeof(buf), "%s\nCh:%d  RSSI:%d%s",
                 g_ap_list_buffer[i].ssid,
                 g_ap_list_buffer[i].primary,
                 g_ap_list_buffer[i].rssi,
                 is_ftm ? "  [FTM]" : "");

        /* Create list item with WiFi icon */
        lv_obj_t *btn = lv_list_add_btn(ap_scanner_list, LV_SYMBOL_WIFI, buf);

        /* Add click event with AP index as user data */
        lv_obj_add_event_cb(btn, ap_item_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        /* Set black background - only default state to save memory */
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

        /* Set white text - only default state to save memory */
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), 0);  // WiFi icon
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 1), lv_color_white(), 0);  // AP text

        /* Make FTM-capable APs stand out with larger font */
        if (is_ftm) {
            lv_obj_set_style_text_font(lv_obj_get_child(btn, 1), &lv_font_montserrat_20, 0);
        }
    }

    ESP_LOGI(TAG, "AP_SCANNER updated: showing %d of %d APs", ap_count, g_scan_ap_num);
}


/* SCREEN_SETTINGS: Event handlers */
static void settings_frame_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    g_ftm_config.frame_count = (uint8_t)value;

    char buf[32];
    snprintf(buf, sizeof(buf), "Frame Count: %ld", (long)value);
    lv_label_set_text(settings_frame_label, buf);

    ESP_LOGI(TAG, "FTM frame count changed to: %ld", (long)value);
}

static void settings_burst_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    g_ftm_config.burst_period = (uint8_t)value;

    char buf[48];
    snprintf(buf, sizeof(buf), "Burst Period: %ld (%ld ms)",
             (long)value, (long)(value * 100));
    lv_label_set_text(settings_burst_label, buf);

    ESP_LOGI(TAG, "FTM burst period changed to: %ld (%ld ms)", (long)value, (long)(value * 100));
}


/* SCREEN_SETTINGS: FTM configuration */
static void screen_settings_create(void)
{
    lv_obj_t *scr = g_screens[SCREEN_SETTINGS].screen;
    lv_obj_t *title = NULL;
    lv_obj_t *info = NULL;
    lv_obj_t *hint = NULL;
    ESP_LOGI(TAG, "Creating SETTINGS screen");

    /* Title */
    LV_CREATE_CHECK(title, lv_label_create(scr));
    lv_label_set_text(title, "FTM Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* Frame Count Slider */
    LV_CREATE_CHECK(settings_frame_label, lv_label_create(scr));
    char frame_buf[32];
    snprintf(frame_buf, sizeof(frame_buf), "Frame Count: %d", g_ftm_config.frame_count);
    lv_label_set_text(settings_frame_label, frame_buf);
    lv_obj_set_style_text_font(settings_frame_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_frame_label, lv_color_make(200, 200, 200), 0);
    lv_obj_align(settings_frame_label, LV_ALIGN_TOP_MID, 0, 55);

    LV_CREATE_CHECK(settings_frame_slider, lv_slider_create(scr));
    lv_slider_set_range(settings_frame_slider, 8, 64);
    lv_slider_set_value(settings_frame_slider, g_ftm_config.frame_count, LV_ANIM_OFF);
    lv_obj_set_width(settings_frame_slider, 350);
    lv_obj_align(settings_frame_slider, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_add_event_cb(settings_frame_slider, settings_frame_slider_cb,
                       LV_EVENT_VALUE_CHANGED, NULL);

    /* Burst Period Slider */
    LV_CREATE_CHECK(settings_burst_label, lv_label_create(scr));
    char burst_buf[48];
    snprintf(burst_buf, sizeof(burst_buf), "Burst Period: %d (%d ms)",
             g_ftm_config.burst_period, g_ftm_config.burst_period * 100);
    lv_label_set_text(settings_burst_label, burst_buf);
    lv_obj_set_style_text_font(settings_burst_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_burst_label, lv_color_make(200, 200, 200), 0);
    lv_obj_align(settings_burst_label, LV_ALIGN_TOP_MID, 0, 135);

    LV_CREATE_CHECK(settings_burst_slider, lv_slider_create(scr));
    lv_slider_set_range(settings_burst_slider, 2, 32);  // Limit to 32 for UI practicality
    lv_slider_set_value(settings_burst_slider, g_ftm_config.burst_period, LV_ANIM_OFF);
    lv_obj_set_width(settings_burst_slider, 350);
    lv_obj_align(settings_burst_slider, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_add_event_cb(settings_burst_slider, settings_burst_slider_cb,
                       LV_EVENT_VALUE_CHANGED, NULL);

    /* Channel Display (Read-only for MVP) */
    LV_CREATE_CHECK(settings_channel_label, lv_label_create(scr));
    lv_label_set_text(settings_channel_label, "Channel: Auto");
    lv_obj_set_style_text_font(settings_channel_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_channel_label, lv_color_make(150, 150, 150), 0);
    lv_obj_align(settings_channel_label, LV_ALIGN_TOP_MID, 0, 225);

    /* Bandwidth Display (Read-only for MVP) */
    LV_CREATE_CHECK(settings_bandwidth_label, lv_label_create(scr));
    char bw_buf[32];
    snprintf(bw_buf, sizeof(bw_buf), "Bandwidth: %d MHz", g_ftm_config.bandwidth);
    lv_label_set_text(settings_bandwidth_label, bw_buf);
    lv_obj_set_style_text_font(settings_bandwidth_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_bandwidth_label, lv_color_make(150, 150, 150), 0);
    lv_obj_align(settings_bandwidth_label, LV_ALIGN_TOP_MID, 0, 255);

    /* Info text */
    LV_CREATE_CHECK(info, lv_label_create(scr));
    lv_label_set_text(info, "Settings apply to next\nFTM measurement");
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(info, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 310);

    /* Hint text */
    LV_CREATE_CHECK(hint, lv_label_create(scr));
    lv_label_set_text(hint, "Swipe to navigate");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_make(100, 100, 100), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* SCREEN_RESPONDER: Event handlers */
static void responder_start_stop_clicked_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    bool is_running = ftm_responder_is_running();

    if (is_running) {
        /* Stop the responder */
        ESP_LOGI(TAG, "Stopping FTM Responder");
        esp_err_t ret = ftm_responder_stop();
        if (ret == ESP_OK) {
            lv_label_set_text(responder_status_label, "Status: Stopped");
            lv_obj_set_style_text_color(responder_status_label, lv_color_make(200, 200, 200), 0);
            lv_obj_set_style_bg_color(responder_btn, lv_color_make(0, 150, 0), 0);  // Green
            lv_label_set_text(responder_btn_label, "START AP");
        } else {
            ESP_LOGE(TAG, "Failed to stop FTM Responder");
        }
    } else {
        /* Start the responder */
        ESP_LOGI(TAG, "Starting FTM Responder");

        /* Generate MAC-based SSID */
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        char ssid[32];
        snprintf(ssid, sizeof(ssid), "TatorTot_%02X%02X", mac[4], mac[5]);

        /* Fixed config for MVP */
        const char *password = "ftm12345";
        uint8_t channel = 6;
        uint8_t bandwidth = 20;

        esp_err_t ret = ftm_responder_start(ssid, password, channel, bandwidth);
        if (ret == ESP_OK) {
            lv_label_set_text(responder_status_label, "Status: Running");
            lv_obj_set_style_text_color(responder_status_label, lv_color_make(0, 255, 0), 0);  // Bright green
            lv_obj_set_style_bg_color(responder_btn, lv_color_make(200, 0, 0), 0);  // Red
            lv_label_set_text(responder_btn_label, "STOP AP");

            /* Update SSID label */
            char ssid_text[48];
            snprintf(ssid_text, sizeof(ssid_text), "SSID: %s", ssid);
            lv_label_set_text(responder_ssid_label, ssid_text);

            /* Update channel label */
            char channel_text[32];
            snprintf(channel_text, sizeof(channel_text), "Channel: %d", channel);
            lv_label_set_text(responder_channel_label, channel_text);
        } else {
            ESP_LOGE(TAG, "Failed to start FTM Responder");
        }
    }
}

/* SCREEN_RESPONDER: AP responder mode */
static void screen_responder_create(void)
{
    lv_obj_t *scr = g_screens[SCREEN_RESPONDER].screen;
    lv_obj_t *title = NULL;
    lv_obj_t *info = NULL;
    ESP_LOGI(TAG, "Creating RESPONDER screen");

    /* Title */
    LV_CREATE_CHECK(title, lv_label_create(scr));
    lv_label_set_text(title, "FTM Responder Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* Status label */
    LV_CREATE_CHECK(responder_status_label, lv_label_create(scr));
    lv_label_set_text(responder_status_label, "Status: Stopped");
    lv_obj_set_style_text_font(responder_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(responder_status_label, lv_color_make(200, 200, 200), 0);
    lv_obj_align(responder_status_label, LV_ALIGN_TOP_MID, 0, 55);

    /* SSID label */
    LV_CREATE_CHECK(responder_ssid_label, lv_label_create(scr));
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid_text[48];
    snprintf(ssid_text, sizeof(ssid_text), "SSID: TatorTot_%02X%02X", mac[4], mac[5]);
    lv_label_set_text(responder_ssid_label, ssid_text);
    lv_obj_set_style_text_font(responder_ssid_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(responder_ssid_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(responder_ssid_label, LV_ALIGN_TOP_MID, 0, 95);

    /* Password label */
    LV_CREATE_CHECK(responder_password_label, lv_label_create(scr));
    lv_label_set_text(responder_password_label, "Password: ftm12345");
    lv_obj_set_style_text_font(responder_password_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(responder_password_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(responder_password_label, LV_ALIGN_TOP_MID, 0, 125);

    /* Channel label */
    LV_CREATE_CHECK(responder_channel_label, lv_label_create(scr));
    lv_label_set_text(responder_channel_label, "Channel: 6");
    lv_obj_set_style_text_font(responder_channel_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(responder_channel_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(responder_channel_label, LV_ALIGN_TOP_MID, 0, 155);

    /* Bandwidth label */
    LV_CREATE_CHECK(responder_bandwidth_label, lv_label_create(scr));
    lv_label_set_text(responder_bandwidth_label, "Bandwidth: 20 MHz");
    lv_obj_set_style_text_font(responder_bandwidth_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(responder_bandwidth_label, lv_color_make(180, 180, 180), 0);
    lv_obj_align(responder_bandwidth_label, LV_ALIGN_TOP_MID, 0, 185);

    /* Start/Stop button */
    LV_CREATE_CHECK(responder_btn, lv_btn_create(scr));
    lv_obj_set_size(responder_btn, 200, 60);
    lv_obj_align(responder_btn, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(responder_btn, lv_color_make(0, 150, 0), 0);  // Green for START
    lv_obj_set_style_radius(responder_btn, 10, 0);
    lv_obj_add_event_cb(responder_btn, responder_start_stop_clicked_cb, LV_EVENT_CLICKED, NULL);

    LV_CREATE_CHECK(responder_btn_label, lv_label_create(responder_btn));
    lv_label_set_text(responder_btn_label, "START AP");
    lv_obj_set_style_text_font(responder_btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(responder_btn_label);

    /* Info text */
    LV_CREATE_CHECK(info, lv_label_create(scr));
    lv_label_set_text(info, "Enables FTM responder\nfor distance measurements");
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(info, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -30);

    ESP_LOGI(TAG, "RESPONDER screen created");
}

static void screen_responder_update(void)
{
    /* Update status based on current AP state */
    bool is_running = ftm_responder_is_running();

    if (is_running) {
        lv_label_set_text(responder_status_label, "Status: Running");
        lv_obj_set_style_text_color(responder_status_label, lv_color_make(0, 255, 0), 0);
        lv_obj_set_style_bg_color(responder_btn, lv_color_make(200, 0, 0), 0);  // Red
        lv_label_set_text(responder_btn_label, "STOP AP");

        /* Update SSID from running config */
        const char *ssid = ftm_responder_get_ssid();
        if (ssid && strlen(ssid) > 0) {
            char ssid_text[48];
            snprintf(ssid_text, sizeof(ssid_text), "SSID: %s", ssid);
            lv_label_set_text(responder_ssid_label, ssid_text);
        }

        /* Update channel from running config */
        uint8_t channel = ftm_responder_get_channel();
        char channel_text[32];
        snprintf(channel_text, sizeof(channel_text), "Channel: %d", channel);
        lv_label_set_text(responder_channel_label, channel_text);
    } else {
        lv_label_set_text(responder_status_label, "Status: Stopped");
        lv_obj_set_style_text_color(responder_status_label, lv_color_make(200, 200, 200), 0);
        lv_obj_set_style_bg_color(responder_btn, lv_color_make(0, 150, 0), 0);  // Green
        lv_label_set_text(responder_btn_label, "START AP");
    }

    ESP_LOGD(TAG, "RESPONDER screen updated (running: %d)", is_running);
}

/* Shutdown button event callback */
static void shutdown_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Shutdown button pressed");
        if (shutdown_callback) {
            shutdown_callback();
        }
    }
}

/* SCREEN_SYSTEM_INFO: Battery & system info */
static void screen_system_info_create(void)
{
    lv_obj_t *scr = g_screens[SCREEN_SYSTEM_INFO].screen;
    lv_obj_t *title = NULL;
    ESP_LOGI(TAG, "Creating SYSTEM_INFO screen");

    /* Title */
    LV_CREATE_CHECK(title, lv_label_create(scr));
    lv_label_set_text(title, "System Info");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Info label - multi-line text showing system status */
    LV_CREATE_CHECK(sysinfo_label, lv_label_create(scr));
    lv_label_set_text(sysinfo_label, "Loading...");
    lv_obj_set_style_text_font(sysinfo_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sysinfo_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_width(sysinfo_label, 400);  /* Allow text wrapping */
    lv_label_set_long_mode(sysinfo_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(sysinfo_label, LV_ALIGN_TOP_LEFT, 35, 60);

    /* Shutdown button */
    LV_CREATE_CHECK(shutdown_btn, lv_btn_create(scr));
    lv_obj_set_size(shutdown_btn, 140, 50);
    lv_obj_align(shutdown_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    /* Purple/blue color - R/B swapped due to display color order quirk */
    lv_obj_set_style_bg_color(shutdown_btn, lv_color_make(176, 39, 156), 0);
    lv_obj_set_style_bg_color(shutdown_btn, lv_color_make(200, 80, 180), LV_STATE_PRESSED);
    lv_obj_set_style_radius(shutdown_btn, 10, 0);

    lv_obj_t *shutdown_label = lv_label_create(shutdown_btn);
    if (shutdown_label) {
        lv_label_set_text(shutdown_label, "Shutdown");
        lv_obj_set_style_text_font(shutdown_label, &lv_font_montserrat_20, 0);
        lv_obj_center(shutdown_label);
    }

    /* Add click event handler */
    lv_obj_add_event_cb(shutdown_btn, shutdown_btn_event_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "SYSTEM_INFO screen created");
}

static void screen_system_info_update(void)
{
    if (sysinfo_label == NULL) return;

    /* Get battery status */
    battery_status_t battery_status;
    esp_err_t batt_err = battery_get_status(&battery_status);

    uint32_t voltage_mv = (batt_err == ESP_OK) ? battery_status.voltage_mv : 0;
    uint8_t percentage = (batt_err == ESP_OK) ? battery_status.percentage : 0;
    bool is_charging = (batt_err == ESP_OK) ? battery_status.is_charging : false;

    /* Get WiFi mode */
    wifi_mode_t wifi_mode;
    esp_wifi_get_mode(&wifi_mode);
    const char *wifi_mode_str = (wifi_mode == WIFI_MODE_STA) ? "STA" :
                                 (wifi_mode == WIFI_MODE_AP) ? "AP" :
                                 (wifi_mode == WIFI_MODE_APSTA) ? "STA+AP" : "OFF";

    /* Get uptime */
    uint32_t uptime_sec = esp_timer_get_time() / 1000000;
    uint32_t hours = uptime_sec / 3600;
    uint32_t minutes = (uptime_sec % 3600) / 60;
    uint32_t seconds = uptime_sec % 60;

    /* Get LVGL memory info */
    lv_mem_monitor_t mem_mon;
    lv_mem_monitor(&mem_mon);

    /* Log if LVGL heap is leaking (diagnostic for memory leak detection) */
    static uint8_t last_used_pct = 0;
    uint8_t current_used_pct = mem_mon.used_pct;
    if (current_used_pct > last_used_pct + 5) {  // 5% increase
        ESP_LOGW(TAG, "LVGL heap increased from %u%% to %u%% (leak detected!)",
                 last_used_pct, current_used_pct);
        last_used_pct = current_used_pct;
    } else if (current_used_pct < last_used_pct) {
        /* Reset baseline if heap usage decreased */
        last_used_pct = current_used_pct;
    }

    /* Format system info text */
    char buf[384];
    snprintf(buf, sizeof(buf),
             "Battery: %u%%%s\n"
             "Voltage: %lu mV\n"
             "\n"
             "WiFi Mode: %s\n"
             "\n"
             "Free Heap: %lu KB\n"
             "Min Heap: %lu KB\n"
             "\n"
             "LVGL Free: %lu KB\n"
             "LVGL Frag: %u%%\n"
             "\n"
             "Uptime: %luh %lum %lus\n"
             "\n"
             "Swipe up/down to navigate",
             percentage,
             is_charging ? " (charging)" : "",
             voltage_mv,
             wifi_mode_str,
             esp_get_free_heap_size() / 1024,
             esp_get_minimum_free_heap_size() / 1024,
             (unsigned long)(mem_mon.free_size / 1024),
             mem_mon.frag_pct,
             hours, minutes, seconds);

    lv_label_set_text(sysinfo_label, buf);
    ESP_LOGD(TAG, "SYSTEM_INFO screen updated");
}

/* Screenshot Support Functions */

lv_disp_t *ftm_display_get_disp(void)
{
    return disp;
}

bool ftm_display_lock(uint32_t timeout_ms)
{
    if (lvgl_mux == NULL) {
        return false;
    }
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ftm_display_unlock(void)
{
    if (lvgl_mux != NULL) {
        xSemaphoreGive(lvgl_mux);
    }
}
