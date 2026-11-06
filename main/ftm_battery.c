/* Battery Monitor for ESP32-C6-Touch-AMOLED-1.32
 *
 * Implementation of battery voltage monitoring using ADC
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "ftm_battery.h"

static const char *TAG = "battery";

/* ADC handles */
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_calibrated = false;

/* Battery monitoring task */
static TaskHandle_t battery_task_handle = NULL;
static SemaphoreHandle_t battery_mutex = NULL;

/* Battery status */
static battery_status_t current_status = {
    .voltage_mv = 0,
    .adc_raw = 0,
    .percentage = 0,
    .is_charging = false,
    .is_low = false,
    .is_critical = false
};

/* Callback */
static battery_event_callback_t status_callback = NULL;

/* Voltage trend tracking for charging detection */
static uint32_t voltage_history[BATTERY_CHARGING_DETECTION_SAMPLES] = {0};
static uint8_t voltage_history_index = 0;
static bool voltage_history_filled = false;

/* Voltage snapshot logging - to diagnose VBAT vs VSYS reading issue */
static uint32_t last_voltage_snapshot = 0;
static uint32_t snapshot_counter = 0;

/**
 * @brief Initialize ADC calibration
 */
static esp_err_t battery_adc_calibration_init(void)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK) {
        adc_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration initialized (curve fitting)");
    }
    return ret;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_WIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    if (ret == ESP_OK) {
        adc_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration initialized (line fitting)");
    }
    return ret;
#else
    ESP_LOGW(TAG, "ADC calibration not supported on this chip");
    adc_calibrated = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/**
 * @brief Deinitialize ADC calibration
 */
static void battery_adc_calibration_deinit(void)
{
    if (adc_calibrated && adc_cali_handle) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(adc_cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(adc_cali_handle);
#endif
        adc_cali_handle = NULL;
        adc_calibrated = false;
        ESP_LOGI(TAG, "ADC calibration deinitialized");
    }
}

esp_err_t battery_monitor_init(void)
{
    esp_err_t ret;

    /* Configure GPIO2 (LCD_EN/BAT_EN) as output with pull-up
     * This allows the device to run on battery when USB is disconnected
     * Enable pull-up to help maintain HIGH state during power transitions
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BATTERY_ENABLE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // Enable pull-up for power stability
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure BAT_EN GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Enable battery power by setting BAT_EN HIGH
     * Also configure GPIO to hold its state during sleep/power transitions
     */
    gpio_set_level(BATTERY_ENABLE_GPIO, 1);
    gpio_hold_en(BATTERY_ENABLE_GPIO);  // Hold GPIO state even during power transitions
    ESP_LOGI(TAG, "BAT_EN (GPIO%d) enabled and held - battery power active", BATTERY_ENABLE_GPIO);

    /* Create mutex for thread-safe access */
    if (battery_mutex == NULL) {
        battery_mutex = xSemaphoreCreateMutex();
        if (battery_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create battery mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Configure ADC oneshot mode */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC unit: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure ADC channel */
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = BATTERY_ADC_WIDTH,
        .atten = BATTERY_ADC_ATTEN,
    };
    ret = adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return ret;
    }

    /* Initialize ADC calibration */
    ret = battery_adc_calibration_init();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "ADC calibration failed, will use raw values: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Battery monitor initialized (ADC1 Channel %d)", BATTERY_ADC_CHANNEL);
    return ESP_OK;
}

esp_err_t battery_read_voltage(uint32_t *voltage_mv)
{
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "Battery monitor not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (voltage_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Read ADC raw value */
    int adc_raw = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &adc_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Convert to voltage */
    int voltage_adc_mv = 0;
    if (adc_calibrated) {
        ret = adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_adc_mv);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC calibration conversion failed: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* No calibration available, use linear approximation
         * For 12-bit ADC with 3.3V reference: voltage = (raw / 4095) * 3300
         */
        voltage_adc_mv = (adc_raw * 3300) / 4095;
    }

    /* Apply 3x voltage divider correction
     * Empirical measurement: ADC=1387mV with no battery (charger at 4.2V)
     * Ratio: 1387/4200 = 0.33 → V_ADC = V_BAT / 3
     * Therefore: V_BAT = V_ADC × 3
     *
     * Actual board resistor ratio appears to be 2:1 (not 1:2 from schematic)
     */
    *voltage_mv = voltage_adc_mv * 3;

    /* Debug logging disabled to reduce console noise */
    // ESP_LOGI(TAG, "ADC: raw=%d, calibrated=%d mV, final(3x)=%lu mV",
    //          adc_raw, voltage_adc_mv, *voltage_mv);

    return ESP_OK;
}

/**
 * @brief Detect charging by analyzing voltage trend
 *
 * Tracks voltage history and detects charging if voltage is consistently increasing
 */
static bool battery_detect_charging_from_trend(uint32_t current_voltage_mv)
{
    /* Update voltage history circular buffer */
    voltage_history[voltage_history_index] = current_voltage_mv;
    voltage_history_index = (voltage_history_index + 1) % BATTERY_CHARGING_DETECTION_SAMPLES;

    if (!voltage_history_filled && voltage_history_index == 0) {
        voltage_history_filled = true;
    }

    /* Need full buffer to detect trend */
    if (!voltage_history_filled) {
        return false;  // Not enough data yet
    }

    /* Calculate average of first half vs second half */
    uint32_t first_half_sum = 0;
    uint32_t second_half_sum = 0;
    uint8_t half = BATTERY_CHARGING_DETECTION_SAMPLES / 2;

    for (uint8_t i = 0; i < half; i++) {
        first_half_sum += voltage_history[i];
        second_half_sum += voltage_history[half + i];
    }

    uint32_t first_half_avg = first_half_sum / half;
    uint32_t second_half_avg = second_half_sum / half;

    /* Charging if second half voltage is higher by threshold */
    bool is_charging = (second_half_avg > first_half_avg) &&
                       ((second_half_avg - first_half_avg) >= BATTERY_CHARGING_THRESHOLD_MV);

    return is_charging;
}

uint8_t battery_calculate_percentage(uint32_t voltage_mv)
{
    /* Simple linear approximation between critical and max voltage */
    if (voltage_mv >= BATTERY_VOLTAGE_MAX) {
        return BATTERY_PERCENT_FULL;
    }
    if (voltage_mv <= BATTERY_VOLTAGE_CRIT) {
        return BATTERY_PERCENT_EMPTY;
    }

    /* Linear interpolation */
    uint32_t range = BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_CRIT;
    uint32_t offset = voltage_mv - BATTERY_VOLTAGE_CRIT;
    uint8_t percentage = (offset * BATTERY_PERCENT_FULL) / range;

    return percentage;
}

esp_err_t battery_get_status(battery_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (battery_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Thread-safe copy of current status */
    if (xSemaphoreTake(battery_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(status, &current_status, sizeof(battery_status_t));
        xSemaphoreGive(battery_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

/**
 * @brief Update battery status
 */
static esp_err_t battery_update_status(void)
{
    uint32_t voltage_mv = 0;
    esp_err_t ret = battery_read_voltage(&voltage_mv);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Detect charging from voltage trend */
    bool is_charging = battery_detect_charging_from_trend(voltage_mv);

    /* Snapshot logging every 10 seconds to help diagnose VBAT vs VSYS issue
     * This persists across USB disconnect/reconnect cycles
     */
    snapshot_counter++;
    if (snapshot_counter % 10 == 0) {
        if (last_voltage_snapshot == 0) {
            last_voltage_snapshot = voltage_mv;
        }
        /* Voltage snapshot logging disabled to reduce console noise */
        // int32_t voltage_change = (int32_t)voltage_mv - (int32_t)last_voltage_snapshot;
        // ESP_LOGW(TAG, "VOLTAGE SNAPSHOT #%lu: Current=%lu mV, Last=%lu mV, Change=%ld mV",
        //          snapshot_counter / 10, voltage_mv, last_voltage_snapshot, voltage_change);
        // ESP_LOGW(TAG, "  -> Unplug USB now and wait 5sec, then plug back in to compare readings");
        last_voltage_snapshot = voltage_mv;
    }

    /* Update status structure */
    if (xSemaphoreTake(battery_mutex, portMAX_DELAY) == pdTRUE) {
        current_status.voltage_mv = voltage_mv;
        current_status.percentage = battery_calculate_percentage(voltage_mv);
        current_status.is_charging = is_charging;
        current_status.is_low = (voltage_mv < BATTERY_VOLTAGE_LOW);
        current_status.is_critical = (voltage_mv < BATTERY_VOLTAGE_CRIT);

        /* Battery status logging disabled to reduce console noise */
        // ESP_LOGI(TAG, "Battery: %lu mV (%d%%) %s",
        //          voltage_mv, current_status.percentage,
        //          is_charging ? "[CHARGING]" : voltage_history_filled ? "[NOT CHARGING]" : "[DETECTING]");

        /* Trigger callback if registered */
        if (status_callback != NULL) {
            status_callback(&current_status);
        }

        xSemaphoreGive(battery_mutex);
    }

    return ESP_OK;
}

/**
 * @brief Battery monitoring task
 */
static void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Battery monitoring task started");

    while (1) {
        esp_err_t ret = battery_update_status();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Battery: %lu mV (%d%%)%s%s",
                     current_status.voltage_mv,
                     current_status.percentage,
                     current_status.is_low ? " [LOW]" : "",
                     current_status.is_critical ? " [CRITICAL]" : "");
        } else {
            ESP_LOGW(TAG, "Failed to update battery status: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_UPDATE_INTERVAL_MS));
    }
}

esp_err_t battery_monitor_start(void)
{
    if (adc_handle == NULL) {
        ESP_LOGE(TAG, "Battery monitor not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (battery_task_handle != NULL) {
        ESP_LOGW(TAG, "Battery monitoring task already running");
        return ESP_OK;
    }

    /* Create battery monitoring task */
    BaseType_t ret = xTaskCreate(
        battery_monitor_task,
        "battery_monitor",
        BATTERY_TASK_STACK_SIZE,
        NULL,
        BATTERY_TASK_PRIORITY,
        &battery_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create battery monitoring task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Battery monitoring task started");
    return ESP_OK;
}

esp_err_t battery_monitor_stop(void)
{
    if (battery_task_handle == NULL) {
        ESP_LOGW(TAG, "Battery monitoring task not running");
        return ESP_OK;
    }

    /* Delete task */
    vTaskDelete(battery_task_handle);
    battery_task_handle = NULL;

    ESP_LOGI(TAG, "Battery monitoring task stopped");
    return ESP_OK;
}

esp_err_t battery_register_callback(battery_event_callback_t callback)
{
    status_callback = callback;
    ESP_LOGI(TAG, "Battery status callback %s", callback ? "registered" : "unregistered");
    return ESP_OK;
}

esp_err_t battery_monitor_deinit(void)
{
    /* Stop monitoring task */
    battery_monitor_stop();

    /* Deinitialize ADC calibration */
    battery_adc_calibration_deinit();

    /* Delete ADC handle */
    if (adc_handle != NULL) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    /* Delete mutex */
    if (battery_mutex != NULL) {
        vSemaphoreDelete(battery_mutex);
        battery_mutex = NULL;
    }

    /* Clear callback */
    status_callback = NULL;

    ESP_LOGI(TAG, "Battery monitor deinitialized");
    return ESP_OK;
}
