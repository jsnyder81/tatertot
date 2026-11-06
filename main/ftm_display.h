/* FTM Display Support for Waveshare ESP32-C6-Touch-AMOLED-1.32
 *
 * This file provides display functionality for showing FTM status
 * on a SH8601 AMOLED display with CST820 touch controller.
 */

#ifndef FTM_DISPLAY_H
#define FTM_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

/* Display Configuration - Waveshare ESP32-C6-Touch-AMOLED-1.32 */
#define LCD_H_RES              466
#define LCD_V_RES              466
#define LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)  // QSPI clock speed
#define LCD_BK_LIGHT_ON_LEVEL  1
#define LCD_BK_LIGHT_OFF_LEVEL !LCD_BK_LIGHT_ON_LEVEL

/* QSPI Pin Configuration - Waveshare ESP32-C6-Touch-AMOLED-1.32 (SH8601 Display) */
/* Official Waveshare pinout from user_config.h */
#define PIN_NUM_LCD_CS         22   // QSPI Chip Select
#define PIN_NUM_LCD_SCLK       18   // QSPI Clock
#define PIN_NUM_LCD_SDIO0      19   // QSPI Data 0
#define PIN_NUM_LCD_SDIO1      20   // QSPI Data 1
#define PIN_NUM_LCD_SDIO2      10   // QSPI Data 2
#define PIN_NUM_LCD_SDIO3      11   // QSPI Data 3
#define PIN_NUM_LCD_RST        21   // Display Reset
#define PIN_NUM_LCD_EN         2    // Power Control (changed from 3 to 2 per Waveshare)

/* Touch Pin Configuration - CST820 (I2C) */
/* Official Waveshare pinout - NOTE: Uses I2C_NUM_0 with address 0x15 */
#define PIN_NUM_TOUCH_SDA      8    // I2C Data (changed from 6 to 8 per Waveshare)
#define PIN_NUM_TOUCH_SCL      7    // I2C Clock
#define PIN_NUM_TOUCH_RST      -1   // Not used (changed from 1 to -1 per Waveshare)
#define PIN_NUM_TOUCH_INT      -1   // Not used (changed from 0 to -1 per Waveshare)
#define TOUCH_I2C_NUM          I2C_NUM_0
#define TOUCH_I2C_ADDR         0x15

/* Button Configuration */
#define PIN_NUM_BUTTON         1    // Power button

/* FTM Status Codes */
typedef enum {
    FTM_DISPLAY_IDLE,
    FTM_DISPLAY_SCANNING,
    FTM_DISPLAY_CONNECTING,
    FTM_DISPLAY_MEASURING,
    FTM_DISPLAY_SUCCESS,
    FTM_DISPLAY_FAILED,
    FTM_DISPLAY_AP_MODE,
    FTM_DISPLAY_CONTINUOUS
} ftm_display_status_t;

/* Display Initialization */
esp_err_t ftm_display_init(void);

/* Screen Navigation - Multi-screen UI system */
esp_err_t ftm_screen_manager_init(void);
esp_err_t ftm_screen_navigate_to(int screen_id);  // Use int for external API
esp_err_t ftm_screen_navigate_next(void);
esp_err_t ftm_screen_navigate_prev(void);
int ftm_screen_get_current(void);  // Use int for external API

/* Display Update Functions */
void ftm_display_update_status(ftm_display_status_t status);
void ftm_display_update_status_text(const char *text, lv_color_t color);
void ftm_display_update_distance(uint32_t distance_cm, uint32_t rtt_nsec);
void ftm_display_update_distance_error(void);
void ftm_display_update_ap_name(const char *ap_name);
void ftm_display_update_ap_info(const char *ssid, int8_t rssi, uint8_t channel);
void ftm_display_update_mode(const char *mode_str);
void ftm_display_clear(void);

/* Display Task Management */
void ftm_display_task_start(void);

/* Touch Callback Type */
typedef void (*ftm_touch_callback_t)(void);

/* Touch Functions */
void ftm_display_set_scan_callback(ftm_touch_callback_t callback);
void ftm_display_set_ftm_callback(ftm_touch_callback_t callback);
void ftm_display_set_ftm_long_press_callback(ftm_touch_callback_t callback);
void ftm_display_set_center_click_callback(ftm_touch_callback_t callback);
void ftm_display_show_ftm_button(void);
void ftm_display_hide_ftm_button(void);
void ftm_display_set_continuous_indicator(bool active);

/* FTM Configuration Functions */
uint8_t ftm_display_get_frame_count(void);
uint8_t ftm_display_get_burst_period(void);
uint8_t ftm_display_get_channel(void);
uint8_t ftm_display_get_bandwidth(void);

/* FTM Responder Functions (implemented in ftm_main.c) */
bool ftm_responder_is_running(void);
esp_err_t ftm_responder_start(const char *ssid, const char *password, uint8_t channel, uint8_t bandwidth);
esp_err_t ftm_responder_stop(void);
const char* ftm_responder_get_ssid(void);
uint8_t ftm_responder_get_channel(void);

/* Continuous Mode Functions (implemented in ftm_main.c) */
bool ftm_continuous_mode_is_running(void);
esp_err_t ftm_continuous_mode_toggle(void);

/* FTM Whitelist Functions (implemented in ftm_main.c) */
bool is_known_ftm_responder(const char *ssid);

/* AP Selection Functions (implemented in ftm_main.c) */
void ftm_set_selected_ap_index(int16_t index);

/* Battery Display Functions */
void ftm_display_update_battery(uint32_t voltage_mv, uint8_t percentage);
void ftm_display_show_battery(void);
void ftm_display_hide_battery(void);

/* Shutdown Functions */
void ftm_display_set_shutdown_callback(ftm_touch_callback_t callback);

/* Screenshot Support Functions */

/**
 * @brief Get LVGL display object for screenshot capture
 * @return Pointer to LVGL display object, or NULL if not initialized
 */
lv_disp_t *ftm_display_get_disp(void);

/**
 * @brief Lock LVGL mutex for thread-safe access
 * @param timeout_ms Timeout in milliseconds
 * @return true if mutex acquired, false on timeout
 */
bool ftm_display_lock(uint32_t timeout_ms);

/**
 * @brief Unlock LVGL mutex
 */
void ftm_display_unlock(void);

#endif // FTM_DISPLAY_H
