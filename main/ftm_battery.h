/* Battery Monitor for ESP32-C6-Touch-AMOLED-1.32
 *
 * This module provides lithium-ion battery voltage monitoring
 * using the ESP32-C6's ADC to measure battery voltage.
 */

#ifndef FTM_BATTERY_H
#define FTM_BATTERY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Battery Configuration for ESP32-C6-Touch-AMOLED-1.32 */
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_0    // GPIO 0 (trying Waveshare config)
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12  // Full-scale voltage 3.3V
#define BATTERY_ADC_WIDTH       ADC_BITWIDTH_12  // 12-bit resolution (0-4095)

/* Power Control - BAT_EN enables battery power when USB disconnected */
#define BATTERY_ENABLE_GPIO     GPIO_NUM_2       // GPIO 2 (BAT_EN)

/* Charging detection configuration
 * GPIO21 is NOT connected to ETA6098 STAT pin on this hardware
 * Charging detection is done by monitoring voltage trends over time
 *
 * With 0.5A charge rate, voltage increases slowly (~1-2mV/min)
 * Using 60 samples (1 minute) with 2mV threshold for reliable detection
 */
#define BATTERY_CHARGING_DETECTION_SAMPLES  60   // Number of samples (60 seconds)
#define BATTERY_CHARGING_THRESHOLD_MV       2    // Minimum voltage increase (mV) to detect charging

/* Battery voltage divider configuration (from schematic)
 * R9 = 100kΩ (to VBAT), R7 = 200kΩ (to GND)
 *
 * ACTUAL BEHAVIOR: ADC reads ~3.3V which matches battery voltage directly
 * This suggests either:
 * 1. The voltage divider has minimal effect at high impedance
 * 2. We're reading VSYS instead of VBAT
 * 3. The schematic annotation is different from actual circuit
 *
 * For now, use ADC voltage directly (ratio 1:1)
 */
#define BATTERY_VOLTAGE_DIVIDER_R1  1  // No correction: ADC voltage = Battery voltage
#define BATTERY_VOLTAGE_DIVIDER_R2  1  // Ratio: 1:1

/* Battery voltage thresholds (millivolts) */
#define BATTERY_VOLTAGE_MAX     4200   // Fully charged Li-ion
#define BATTERY_VOLTAGE_NOM     3700   // Nominal voltage
#define BATTERY_VOLTAGE_LOW     3400   // Low battery warning
#define BATTERY_VOLTAGE_CRIT    3000   // Critical low voltage

/* Battery percentage calculation */
#define BATTERY_PERCENT_FULL    100
#define BATTERY_PERCENT_EMPTY   0

/* Battery monitoring task configuration */
#define BATTERY_TASK_STACK_SIZE 2048
#define BATTERY_TASK_PRIORITY   5
#define BATTERY_UPDATE_INTERVAL_MS  20000  // Update every 20 seconds

/**
 * @brief Battery status information
 */
typedef struct {
    uint32_t voltage_mv;        // Battery voltage in millivolts
    uint32_t adc_raw;           // Raw ADC reading
    uint8_t percentage;         // Battery percentage (0-100%)
    bool is_charging;           // Charging status (reserved for future use)
    bool is_low;                // Low battery flag
    bool is_critical;           // Critical battery flag
} battery_status_t;

/**
 * @brief Battery event callback function type
 *
 * @param status Pointer to current battery status
 */
typedef void (*battery_event_callback_t)(const battery_status_t *status);

/**
 * @brief Initialize the battery monitoring system
 *
 * Configures ADC for battery voltage measurement and calibration.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_init(void);

/**
 * @brief Start the battery monitoring task
 *
 * Starts a FreeRTOS task that periodically reads battery voltage
 * and triggers callbacks on status changes.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_start(void);

/**
 * @brief Stop the battery monitoring task
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_stop(void);

/**
 * @brief Get current battery status
 *
 * @param[out] status Pointer to battery_status_t structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_get_status(battery_status_t *status);

/**
 * @brief Read raw battery voltage
 *
 * @param[out] voltage_mv Pointer to store voltage in millivolts
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_read_voltage(uint32_t *voltage_mv);


/**
 * @brief Calculate battery percentage from voltage
 *
 * Uses a linear approximation between min and max voltage.
 *
 * @param voltage_mv Battery voltage in millivolts
 * @return Battery percentage (0-100%)
 */
uint8_t battery_calculate_percentage(uint32_t voltage_mv);

/**
 * @brief Register a callback for battery status updates
 *
 * The callback will be invoked whenever the battery status changes
 * (voltage, percentage, or flags).
 *
 * @param callback Callback function pointer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_register_callback(battery_event_callback_t callback);

/**
 * @brief Deinitialize the battery monitoring system
 *
 * Stops monitoring task and releases resources.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_deinit(void);

#endif // FTM_BATTERY_H
