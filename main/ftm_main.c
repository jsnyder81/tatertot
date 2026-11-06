/* Wi-Fi FTM Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "cmd_system.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "lvgl.h"
#include "ftm_display.h"
#include "ftm_battery.h"

typedef struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_lit *disconnect;
    struct arg_end *end;
} wifi_sta_args_t;

typedef struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_int *channel;
    struct arg_int *bandwidth;
    struct arg_end *end;
} wifi_ap_args_t;

typedef struct {
    struct arg_str *ssid;
    struct arg_end *end;
} wifi_scan_arg_t;

typedef struct {
    /* FTM Initiator */
    struct arg_lit *initiator;
    struct arg_int *frm_count;
    struct arg_int *burst_period;
    struct arg_str *ssid;
    /* FTM Responder */
    struct arg_lit *responder;
    struct arg_lit *enable;
    struct arg_lit *disable;
    struct arg_int *offset;
    struct arg_end *end;
} wifi_ftm_args_t;

static wifi_sta_args_t sta_args;
static wifi_ap_args_t ap_args;
static wifi_scan_arg_t scan_args;
static wifi_ftm_args_t ftm_args;

wifi_config_t g_ap_config = {
    .ap.max_connection = 4,
    .ap.authmode = WIFI_AUTH_WPA2_PSK,
    .ap.ftm_responder = true
};

#define ETH_ALEN 6
#define MAX_CONNECT_RETRY_ATTEMPTS  5
#define DEFAULT_WAIT_TIME_MS        (10 * 1000)
#define MAX_FTM_BURSTS          8
#define DEFAULT_AP_CHANNEL      1
#define DEFAULT_AP_BANDWIDTH    20

static bool s_reconnect = true;
static int s_retry_num = 0;
static const char *TAG_STA = "ftm_station";
static const char *TAG_AP = "ftm_ap";

static EventGroupHandle_t s_wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static esp_event_handler_instance_t s_wifi_event_handler_instance = NULL;

static EventGroupHandle_t s_ftm_event_group;
static const int FTM_REPORT_BIT = BIT0;
static const int FTM_FAILURE_BIT = BIT1;

/* Exposed FTM result variables - updated from event handler */
uint8_t s_ftm_report_num_entries;  /* Exposed for ftm_display.c - read-only snapshot */
uint32_t s_rtt_est, s_dist_est;    /* Exposed for ftm_display.c - read-only snapshot */
wifi_ap_record_t s_last_ftm_target_ap;  /* Exposed for ftm_display.c - read-only snapshot */

static bool s_ap_started;

/* Busy flags to prevent concurrent scan/FTM operations - use atomic access */
static volatile uint32_t s_scan_in_progress = 0;
static volatile uint32_t s_ftm_in_progress = 0;

/* Continuous FTM mode state */
static volatile bool s_continuous_mode_active = false;
static wifi_ap_record_t s_continuous_target_ap;
static SemaphoreHandle_t s_continuous_mode_mutex = NULL;
static uint8_t s_continuous_failure_count = 0;
static TaskHandle_t s_continuous_mode_task = NULL;

/* Continuous mode cycle counter for periodic WiFi reinit */
static uint16_t s_ftm_cycle_count = 0;
#define FTM_REINIT_THRESHOLD 15  // Reinit WiFi every 15 cycles

/* Heap threshold for auto-reboot (workaround for WiFi FTM driver leak) */
#define HEAP_CRITICAL_THRESHOLD 18432  // 18KB - WiFi needs ~15KB for FTM frames + 3KB safety margin

/* Timeout for FTM wait in continuous mode (prevents infinite hang if WiFi disconnects) */
#define FTM_WAIT_TIMEOUT_MS 30000  // 30 seconds

/* Heap tracing for memory leak detection */
#define HEAP_TRACE_RECORDS 200  // ~8KB RAM - needed to capture multiple FTM cycles
static heap_trace_record_t s_heap_trace_records[HEAP_TRACE_RECORDS];
static bool s_heap_tracing_enabled = false;

/* Selected AP from UI - used to override automatic AP selection */
static int16_t s_selected_ap_index = -1;  // -1 means no selection

uint16_t g_scan_ap_num;
wifi_ap_record_t *g_ap_list_buffer;

/* Forward declarations for continuous mode functions */
static void set_continuous_mode(bool active, wifi_ap_record_t *target);
static bool get_continuous_mode(void);
static esp_err_t initiate_ftm_to_target(wifi_ap_record_t *ap_record);

/* Thread-safe accessors for scan/FTM progress flags */
static inline bool get_scan_in_progress(void) {
    return __atomic_load_n(&s_scan_in_progress, __ATOMIC_ACQUIRE) != 0;
}

static inline void set_scan_in_progress(bool value) {
    __atomic_store_n(&s_scan_in_progress, value ? 1 : 0, __ATOMIC_RELEASE);
}

static inline bool get_ftm_in_progress(void) {
    return __atomic_load_n(&s_ftm_in_progress, __ATOMIC_ACQUIRE) != 0;
}

static inline void set_ftm_in_progress(bool value) {
    __atomic_store_n(&s_ftm_in_progress, value ? 1 : 0, __ATOMIC_RELEASE);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
	if (event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;

        ESP_LOGI(TAG_STA, "Connected to %s (BSSID: "MACSTR", Channel: %d)", event->ssid,
                 MAC2STR(event->bssid), event->channel);

        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_reconnect && ++s_retry_num < MAX_CONNECT_RETRY_ATTEMPTS) {
            ESP_LOGI(TAG_STA, "sta disconnect, retry attempt %d...", s_retry_num);
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG_STA, "sta disconnected");
        }
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_id == WIFI_EVENT_FTM_REPORT) {
        wifi_event_ftm_report_t *event = (wifi_event_ftm_report_t *) event_data;

        s_rtt_est = event->rtt_est;
        s_dist_est = event->dist_est;
        s_ftm_report_num_entries = event->ftm_report_num_entries;

        bool continuous_active = get_continuous_mode();

        if (event->status == FTM_STATUS_SUCCESS) {
            /* Extract RSSI directly from event data (bypasses failing API) */
            if (event->ftm_report_data && event->ftm_report_num_entries > 0) {
                int32_t rssi_sum = 0;
                uint16_t rssi_count = 0;

                for (int i = 0; i < event->ftm_report_num_entries; i++) {
                    if (event->ftm_report_data[i].rssi != 0) {  // Skip invalid
                        rssi_sum += event->ftm_report_data[i].rssi;
                        rssi_count++;
                    }
                }

                if (rssi_count > 0) {
                    int8_t avg_rssi = (int8_t)(rssi_sum / rssi_count);
                    ESP_LOGI(TAG_STA, "Direct RSSI from event: %d dBm (%u samples)",
                             avg_rssi, rssi_count);

                    /* Update target AP structure */
                    wifi_ap_record_t *target_ap = continuous_active ?
                                                   &s_continuous_target_ap :
                                                   &s_last_ftm_target_ap;
                    target_ap->rssi = avg_rssi;

                    /* Update display immediately */
                    ftm_display_update_ap_info((const char *)target_ap->ssid,
                                                avg_rssi,
                                                target_ap->primary);
                }
            }

            /* Update display with FTM results */
            ESP_LOGI(TAG_STA, "FTM Success - RTT: %lu ns, Distance: %lu.%02lu m",
                     s_rtt_est, s_dist_est / 100, s_dist_est % 100);
            ftm_display_update_distance(s_dist_est, s_rtt_est);

            /* Signal command handler to process FTM report and extract RSSI */
            /* WiFi driver needs task switch before report is available */
            if (continuous_active) {
                ftm_display_update_status(FTM_DISPLAY_CONTINUOUS);
            } else {
                ftm_display_update_status(FTM_DISPLAY_SUCCESS);
            }
            xEventGroupSetBits(s_ftm_event_group, FTM_REPORT_BIT);

            /* Continuous mode handled by dedicated task */
            if (continuous_active) {
                /* Reset failure counter on success */
                s_continuous_failure_count = 0;
                /* Keep FTM in progress for continuous mode */
            } else {
                /* Normal single-shot mode */
                set_ftm_in_progress(false);
            }

        } else if (event->status == FTM_STATUS_USER_TERM) {
            /* User terminated */
            ESP_LOGI(TAG_STA, "User terminated FTM procedure");
            set_ftm_in_progress(false);

        } else {
            /* FTM failure */
            ESP_LOGI(TAG_STA, "FTM procedure with Peer("MACSTR") failed! (Status - %d)",
                     MAC2STR(event->peer_mac), event->status);

            if (continuous_active) {
                /* Continuous mode: Let dedicated task handle failures */
                /* Keep FTM in progress for continuous mode */
                xEventGroupSetBits(s_ftm_event_group, FTM_FAILURE_BIT);
            } else {
                /* Normal single-shot failure */
                ftm_display_update_status(FTM_DISPLAY_FAILED);
                xEventGroupSetBits(s_ftm_event_group, FTM_FAILURE_BIT);
                set_ftm_in_progress(false);
            }
        }

        /* Free FTM report buffer - required when use_get_report_api=false
         * ESP-IDF docs: "Pointer to FTM Report, should be freed after use" */
        if (event->ftm_report_data != NULL) {
            free(event->ftm_report_data);
        }
    } else if (event_id == WIFI_EVENT_AP_START) {
        s_ap_started = true;
        ESP_LOGI(TAG_AP, "FTM Responder AP started - ready to respond to FTM requests");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        s_ap_started = false;
        ESP_LOGI(TAG_AP, "FTM Responder AP stopped");
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station connected to FTM Responder: MAC=" MACSTR " AID=%d",
                 MAC2STR(event->mac), event->aid);
        ESP_LOGI(TAG_AP, "FTM Responder ready to handle FTM requests from this station");
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station disconnected from FTM Responder: MAC=" MACSTR " AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

/* Continuous mode task - runs independently and handles all FTM operations */
static void continuous_mode_task(void *pvParameters)
{
    EventBits_t bits;

    ESP_LOGI(TAG_STA, "Continuous mode task started");

    /* Initiate first measurement from THIS task */
    if (initiate_ftm_to_target(&s_continuous_target_ap) != ESP_OK) {
        ESP_LOGE(TAG_STA, "Failed to start first continuous measurement");
        set_continuous_mode(false, NULL);
        ftm_display_set_continuous_indicator(false);
        set_ftm_in_progress(false);
        s_continuous_mode_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (get_continuous_mode()) {
        /* Wait for FTM report or failure with timeout to prevent infinite hang */
        bits = xEventGroupWaitBits(s_ftm_event_group,
                                   FTM_REPORT_BIT | FTM_FAILURE_BIT,
                                   pdTRUE,  /* Clear bits on exit */
                                   pdFALSE, /* Wait for any bit */
                                   pdMS_TO_TICKS(FTM_WAIT_TIMEOUT_MS));

        /* Handle timeout - prevents infinite hang if WiFi disconnects */
        if (bits == 0) {
            ESP_LOGW(TAG_STA, "FTM wait timeout after %d ms - forcing session end", FTM_WAIT_TIMEOUT_MS);
            esp_wifi_ftm_end_session();
            s_continuous_failure_count++;

            /* Update display to show timeout */
            ftm_display_update_status(FTM_DISPLAY_FAILED);

            /* Check if too many failures */
            if (s_continuous_failure_count >= 10) {
                ESP_LOGE(TAG_STA, "Too many timeouts (%d), stopping continuous mode", s_continuous_failure_count);
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (bits & FTM_REPORT_BIT) {
            /* SUCCESS: RSSI already extracted in event handler */
            /* Buffer auto-freed by WiFi driver (use_get_report_api=false) */

            /* Increment cycle counter */
            s_ftm_cycle_count++;
            ESP_LOGI(TAG_STA, "Continuous cycle %d/%d", s_ftm_cycle_count, FTM_REINIT_THRESHOLD);

            /* Component memory breakdown - diagnostic for leak detection */
            if (s_ftm_cycle_count % 10 == 0) {  // Every 10 cycles
                ESP_LOGI(TAG_STA, "=== Memory Breakdown (Cycle %d) ===", s_ftm_cycle_count);

                multi_heap_info_t info;
                heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
                ESP_LOGI(TAG_STA, "Default heap: total=%zu, free=%zu, min_free=%zu",
                         info.total_free_bytes + info.total_allocated_bytes,
                         info.total_free_bytes,
                         info.minimum_free_bytes);

                heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
                ESP_LOGI(TAG_STA, "Internal RAM: total=%zu, free=%zu, min_free=%zu",
                         info.total_free_bytes + info.total_allocated_bytes,
                         info.total_free_bytes,
                         info.minimum_free_bytes);

                UBaseType_t stack_high_water;
                TaskHandle_t wifi_task = xTaskGetHandle("wifi");
                if (wifi_task != NULL) {
                    stack_high_water = uxTaskGetStackHighWaterMark(wifi_task);
                    ESP_LOGI(TAG_STA, "WiFi task stack: %u bytes free", stack_high_water * sizeof(StackType_t));
                }

                TaskHandle_t event_task = xTaskGetHandle("esp_event");
                if (event_task != NULL) {
                    stack_high_water = uxTaskGetStackHighWaterMark(event_task);
                    ESP_LOGI(TAG_STA, "Event task stack: %u bytes free", stack_high_water * sizeof(StackType_t));
                }
            }

            /* End current FTM session */
            esp_wifi_ftm_end_session();

            /* Delay before next measurement */
            vTaskDelay(pdMS_TO_TICKS(2000));

            /* Check if still in continuous mode before restarting */
            if (!get_continuous_mode()) {
                break;
            }

            /* Start next measurement */
            initiate_ftm_to_target(&s_continuous_target_ap);

        } else if (bits & FTM_FAILURE_BIT) {
            /* FAILURE: Handle errors in continuous mode */
            s_continuous_failure_count++;
            ESP_LOGW(TAG_STA, "Continuous mode failure count: %d", s_continuous_failure_count);

            if (s_continuous_failure_count > 10) {
                /* Too many failures - stop continuous mode */
                ESP_LOGE(TAG_STA, "Too many consecutive failures - stopping continuous mode");
                set_continuous_mode(false, NULL);
                ftm_display_set_continuous_indicator(false);
                ftm_display_update_status(FTM_DISPLAY_FAILED);
                set_ftm_in_progress(false);
                break;
            } else {
                /* Show error but keep measuring */
                ftm_display_update_distance_error();
                ftm_display_update_status(FTM_DISPLAY_CONTINUOUS);
                esp_wifi_ftm_end_session();
                vTaskDelay(pdMS_TO_TICKS(2000));

                if (!get_continuous_mode()) {
                    break;
                }

                ESP_LOGI(TAG_STA, "Continuous mode: retrying FTM measurement");
                initiate_ftm_to_target(&s_continuous_target_ap);
            }
        }
    }

    ESP_LOGI(TAG_STA, "Continuous mode task exiting");

    /* End any active FTM session and clear the busy flag */
    esp_wifi_ftm_end_session();
    set_ftm_in_progress(false);

    s_continuous_mode_task = NULL;
    vTaskDelete(NULL);
}

void initialise_wifi(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);
    static bool initialized = false;

    if (initialized) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    s_ftm_event_group = xEventGroupCreate();
    s_continuous_mode_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK( esp_event_loop_create_default() );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &s_wifi_event_handler_instance));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    initialized = true;
}

esp_err_t wifi_add_mode(wifi_mode_t mode)
{
    wifi_mode_t cur_mode, new_mode = mode;

    if (esp_wifi_get_mode(&cur_mode)) {
        ESP_LOGE(TAG_STA, "Failed to get mode!");
        return ESP_FAIL;
    }

    if (mode == WIFI_MODE_STA) {
        if (cur_mode == WIFI_MODE_STA || cur_mode == WIFI_MODE_APSTA) {
            wifi_ap_record_t ap_info;
            esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
            if (ret == ESP_OK) {
                s_reconnect = false;
                xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
                ESP_ERROR_CHECK( esp_wifi_disconnect() );
            }
            return ESP_OK;
        } else if (cur_mode == WIFI_MODE_AP) {
            new_mode = WIFI_MODE_APSTA;
        } else {
            new_mode = WIFI_MODE_STA;
        }
    } else if (mode == WIFI_MODE_AP) {
        if (cur_mode == WIFI_MODE_AP || cur_mode == WIFI_MODE_APSTA) {
            /* Do nothing */
            return ESP_OK;
        } else if (cur_mode == WIFI_MODE_STA) {
            new_mode = WIFI_MODE_APSTA;
        } else {
            new_mode = WIFI_MODE_AP;
        }
    }

    ESP_ERROR_CHECK( esp_wifi_set_mode(new_mode) );
    return ESP_OK;
}

static bool wifi_cmd_sta_join(const char *ssid, const char *pass)
{

    if (ESP_OK != wifi_add_mode(WIFI_MODE_STA)) {
        return false;
    }

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) {
        strlcpy((char *) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    }
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_connect() );
    s_reconnect = true;
    s_retry_num = 0;

    xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT, pdTRUE, pdTRUE, 5000 / portTICK_PERIOD_MS);

    return true;
}

static int wifi_cmd_sta(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &sta_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, sta_args.end, argv[0]);
        return 1;
    }
    if (sta_args.disconnect->count) {
        s_reconnect = false;
        wifi_ap_record_t ap_info;
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_STA, "Sta Already disconnected");
            return 0;
        }
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        esp_wifi_disconnect();
        return 0;
    }

    if (sta_args.ssid->count == 0) {
        ESP_LOGE(TAG_STA, "SSID is required. Usage: sta <ssid> [password]");
        return 1;
    }

    ESP_LOGI(TAG_STA, "sta connecting to '%s'", sta_args.ssid->sval[0]);
    wifi_cmd_sta_join(sta_args.ssid->sval[0], sta_args.password->sval[0]);
    return 0;
}

/* Forward declaration for FTM responder check */
bool is_known_ftm_responder(const char *ssid);

static bool wifi_perform_scan(const char *ssid, bool internal)
{
    wifi_scan_config_t scan_config = { 0 };
    scan_config.ssid = (uint8_t *) ssid;
    scan_config.show_hidden = true;  // Enable to get full AP capabilities including FTM
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;  // Active scan for better results
    scan_config.scan_time.active.min = 120;  // Min scan time per channel (ms)
    scan_config.scan_time.active.max = 150;  // Max scan time per channel (ms)
    wifi_mode_t mode;
    uint8_t i;

    ESP_ERROR_CHECK( esp_wifi_get_mode(&mode) );
    if ((mode != WIFI_MODE_STA) && (mode != WIFI_MODE_APSTA)) {
        if (ESP_OK != wifi_add_mode(WIFI_MODE_STA)) {
            return false;
        }
    }
    if (ESP_OK != esp_wifi_scan_start(&scan_config, true)) {
        ESP_LOGI(TAG_STA, "Failed to perform scan");
        return false;
    }

    esp_wifi_scan_get_ap_num(&g_scan_ap_num);
    if (g_scan_ap_num == 0) {
        ESP_LOGI(TAG_STA, "No matching AP found");
        return false;
    }

    if (g_ap_list_buffer) {
        free(g_ap_list_buffer);
        g_ap_list_buffer = NULL;
    }

    /* Check heap before allocation */
    size_t required_size = g_scan_ap_num * sizeof(wifi_ap_record_t);
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG_STA, "Scan found %d APs, need %zu bytes, free heap: %zu bytes",
             g_scan_ap_num, required_size, free_heap);

    /* Safety check: limit AP list size if memory is tight */
    if (required_size > (free_heap / 2) || g_scan_ap_num > 100) {
        uint16_t max_aps = (free_heap / 2) / sizeof(wifi_ap_record_t);
        if (max_aps > 100) max_aps = 100;
        if (g_scan_ap_num > max_aps) {
            ESP_LOGW(TAG_STA, "Limiting AP list from %d to %d due to memory constraints",
                     g_scan_ap_num, max_aps);
            g_scan_ap_num = max_aps;
            required_size = g_scan_ap_num * sizeof(wifi_ap_record_t);
        }
    }

    g_ap_list_buffer = malloc(required_size);
    if (g_ap_list_buffer == NULL) {
        ESP_LOGE(TAG_STA, "Failed to malloc %zu bytes for scan results (free heap: %zu)",
                 required_size, esp_get_free_heap_size());
        esp_wifi_clear_ap_list();
        g_scan_ap_num = 0;
        return false;
    }

    if (esp_wifi_scan_get_ap_records(&g_scan_ap_num, (wifi_ap_record_t *)g_ap_list_buffer) == ESP_OK) {
        if (!internal) {
            for (i = 0; i < g_scan_ap_num; i++) {
                /* Debug: Print FTM capability fields to diagnose detection issue */
                ESP_LOGI(TAG_STA, "[%s][rssi=%d][ftm_responder=%d][ftm_initiator=%d]%s",
                         g_ap_list_buffer[i].ssid,
                         g_ap_list_buffer[i].rssi,
                         g_ap_list_buffer[i].ftm_responder,
                         g_ap_list_buffer[i].ftm_initiator,
                         g_ap_list_buffer[i].ftm_responder ? "[FTM Responder]" : "");
            }
            /* Note about ESP32-C6 FTM detection limitation */
            ESP_LOGW(TAG_STA, "Note: ESP32-C6 may not detect FTM capability in scans");
            ESP_LOGW(TAG_STA, "FTM will still work if AP supports it - try 'ftm -I -s <ssid>'");
        }
    }

    ESP_LOGI(TAG_STA, "sta scan done");

    /* Update display with FTM responder count */
    if (!internal) {
        int ftm_count = 0;

        /* Count APs with ftm_responder flag set */
        for (i = 0; i < g_scan_ap_num; i++) {
            if (g_ap_list_buffer[i].ftm_responder) {
                ftm_count++;
            }
        }

        /* Count known FTM responders from whitelist */
        int known_count = 0;
        for (i = 0; i < g_scan_ap_num; i++) {
            if (is_known_ftm_responder((const char *)g_ap_list_buffer[i].ssid)) {
                known_count++;
            }
        }

        /* Update display status */
        char status_msg[64];
        int total_ftm = ftm_count + known_count;
        if (total_ftm == 0) {
            snprintf(status_msg, sizeof(status_msg), "No Responders");
        } else if (total_ftm == 1) {
            snprintf(status_msg, sizeof(status_msg), "1 Responder");
        } else {
            snprintf(status_msg, sizeof(status_msg), "%d Responders", total_ftm);
        }

        ESP_LOGI(TAG_STA, "Display status: %s (flagged=%d, known=%d)", status_msg, ftm_count, known_count);

        /* Update display with custom status text */
        lv_color_t color = (total_ftm > 0) ? lv_color_make(100, 200, 100) : lv_color_make(255, 150, 100);
        ftm_display_update_status_text(status_msg, color);

        /* Show FTM button now that we have scan results */
        ftm_display_show_ftm_button();
    }

    return true;
}

static int wifi_cmd_scan(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &scan_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, scan_args.end, argv[0]);
        return 1;
    }

    ESP_LOGI(TAG_STA, "sta start to scan");
    if ( scan_args.ssid->count == 1 ) {
        wifi_perform_scan(scan_args.ssid->sval[0], false);
    } else {
        wifi_perform_scan(NULL, false);
    }
    return 0;
}

static bool wifi_cmd_ap_set(const char* ssid, const char* pass, uint8_t channel, uint8_t bw)
{
    s_reconnect = false;
    strlcpy((char*) g_ap_config.ap.ssid, ssid, MAX_SSID_LEN);
    if (pass) {
        if (strlen(pass) != 0 && strlen(pass) < 8) {
            s_reconnect = true;
            ESP_LOGE(TAG_AP, "password cannot be less than 8 characters long");
            return false;
        }
        strlcpy((char*) g_ap_config.ap.password, pass, MAX_PASSPHRASE_LEN);
    }
#if !CONFIG_SOC_WIFI_SUPPORT_5G
    if (!(channel >=1 && channel <= 14)) {
        ESP_LOGE(TAG_AP, "Channel cannot be %d!", channel);
        return false;
    }
#endif
    if (bw != 20 && bw != 40) {
        ESP_LOGE(TAG_AP, "Cannot set %d MHz bandwidth!", bw);
        return false;
    }

    if (ESP_OK != wifi_add_mode(WIFI_MODE_AP)) {
        return false;
    }
    // ESP-IDF v5.5.1 uses single-band APIs: esp_wifi_set_protocol() and esp_wifi_set_bandwidth()
    if (channel <= 14) {
        // 2.4 GHz configuration
        if (bw == 40) {
            uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
            esp_wifi_set_protocol(WIFI_IF_AP, protocol);
            esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
        } else {
            esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        }
    } else {
#if CONFIG_SOC_WIFI_SUPPORT_5G
        // 5 GHz configuration
        if (bw == 40) {
            uint8_t protocol = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
            esp_wifi_set_protocol(WIFI_IF_AP, protocol);
            esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
        } else {
            uint8_t protocol = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
            esp_wifi_set_protocol(WIFI_IF_AP, protocol);
            esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        }
#endif
    }
    if (strlen(pass) == 0) {
        g_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    g_ap_config.ap.channel = channel;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &g_ap_config));
    ESP_LOGI(TAG_AP, "Starting SoftAP with FTM Responder support, SSID - %s, Password - %s, Primary Channel - %d, Bandwidth - %dMHz",
                        ap_args.ssid->sval[0], ap_args.password->sval[0], channel, bw);

    return true;
}

static int wifi_cmd_ap(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &ap_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, ap_args.end, argv[0]);
        return 1;
    }

    if (!wifi_cmd_ap_set(ap_args.ssid->sval[0], ap_args.password->sval[0],
                                ap_args.channel->count != 0 ? ap_args.channel->ival[0] : DEFAULT_AP_CHANNEL,
                                ap_args.bandwidth->count != 0 ? ap_args.bandwidth->ival[0] : DEFAULT_AP_BANDWIDTH)) {
        ESP_LOGE(TAG_AP, "Failed to start SoftAP!");
        return 1;
    }

    return 0;
}

static int wifi_cmd_query(int argc, char **argv)
{
    wifi_config_t cfg;
    wifi_mode_t mode;
    wifi_ap_record_t ap_info;

    esp_wifi_get_mode(&mode);
    if (WIFI_MODE_AP == mode) {
        esp_wifi_get_config(WIFI_IF_AP, &cfg);
        ESP_LOGI(TAG_AP, "AP mode, %s %s", cfg.ap.ssid, cfg.ap.password);
    } else if (WIFI_MODE_STA == mode) {
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG_STA, "sta mode, connected %s", ap_info.ssid);
        } else {
            ESP_LOGI(TAG_STA, "sta mode, disconnected");
        }
    } else if (WIFI_MODE_APSTA == mode) {
        ESP_LOGI(TAG_STA, "AP-Sta mode");
        esp_wifi_get_config(WIFI_IF_AP, &cfg);
        ESP_LOGI(TAG_AP, "AP mode, %s %s", cfg.ap.ssid, cfg.ap.password);
        esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG_STA, "sta mode, connected %s", ap_info.ssid);
        } else {
            ESP_LOGI(TAG_STA, "sta mode, disconnected");
        }

    } else {
        ESP_LOGI(TAG_STA, "NULL mode");
        return 0;
    }

    return 0;
}

wifi_ap_record_t *find_ftm_responder_ap(const char *ssid)
{
    bool retry_scan = false;
    uint8_t i;

    if (!ssid)
        return NULL;

retry:
    if (!g_ap_list_buffer || (g_scan_ap_num == 0)) {
        ESP_LOGI(TAG_STA, "Scanning for %s", ssid);
        if (false == wifi_perform_scan(ssid, true)) {
            return NULL;
        }
    }

    for (i = 0; i < g_scan_ap_num; i++) {
        if (strcmp((const char *)g_ap_list_buffer[i].ssid, ssid) == 0) {
            /* WORKAROUND: ESP32-C6 scan often fails to detect ftm_responder flag
             * even when AP properly advertises FTM capability. Since FTM will fail
             * quickly if AP doesn't support it, we proceed anyway and let the user know.
             */
            if (!g_ap_list_buffer[i].ftm_responder) {
                ESP_LOGW(TAG_STA, "AP '%s' not detected as FTM Responder in scan", ssid);
                ESP_LOGW(TAG_STA, "This may be an ESP32-C6 scan limitation - attempting FTM anyway");
                ESP_LOGW(TAG_STA, "If FTM fails, verify AP has FTM Responder enabled");
            }
            return &g_ap_list_buffer[i];
        }
    }

    if (!retry_scan) {
        retry_scan = true;
        if (g_ap_list_buffer) {
            free(g_ap_list_buffer);
            g_ap_list_buffer = NULL;
        }
        goto retry;
    }

    ESP_LOGI(TAG_STA, "No matching AP found");

    return NULL;
}

static int wifi_cmd_ftm(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &ftm_args);
    wifi_ap_record_t *ap_record, ap_info;
    uint32_t wait_time_ms = DEFAULT_WAIT_TIME_MS;
    EventBits_t bits;

    /* FTM Initiator Configuration
     * - frm_count: 0 enables ASAP mode (single burst) which is more reliable
     * - burst_period: 2 = 200ms between bursts (only used in non-ASAP mode)
     *
     * ASAP mode (frm_count=0): Single burst with multiple frames, fastest and most reliable
     * Non-ASAP mode (frm_count>0): Multiple bursts spread over time, needs longer timeout
     */
    wifi_ftm_initiator_cfg_t ftmi_cfg = {
        .frm_count = 0,      // Use ASAP mode by default (changed from 32)
        .burst_period = 2,   // 200ms burst period (for non-ASAP mode)
        .use_get_report_api = false,  // Use direct event access (driver manages buffer automatically)
    };

    if (nerrors != 0) {
        arg_print_errors(stderr, ftm_args.end, argv[0]);
        return 0;
    }

    if (ftm_args.initiator->count != 0 && ftm_args.responder->count != 0) {
        ESP_LOGE(TAG_STA, "Invalid FTM cmd argument");
        return 0;
    }

    if (ftm_args.responder->count != 0)
        goto ftm_responder;

    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_OK && !ftm_args.ssid->count) {
        memcpy(ftmi_cfg.resp_mac, ap_info.bssid, ETH_ALEN);
        ftmi_cfg.channel = ap_info.primary;
    } else if (ftm_args.ssid->count == 1) {
        ap_record = find_ftm_responder_ap(ftm_args.ssid->sval[0]);
        if (ap_record) {
            memcpy(ftmi_cfg.resp_mac, ap_record->bssid, 6);
            ftmi_cfg.channel = ap_record->primary;
        } else {
            return 0;
        }
    } else {
        ESP_LOGE(TAG_STA, "Provide SSID of the AP in disconnected state!");
        return 0;
    }

    if (ftm_args.frm_count->count != 0) {
        uint8_t count = ftm_args.frm_count->ival[0];
        if (count != 0 && count != 8 && count != 16 &&
            count != 24 && count != 32 && count != 64) {
            ESP_LOGE(TAG_STA, "Invalid Frame Count! Valid options are 0/8/16/24/32/64");
            return 0;
        }
        ftmi_cfg.frm_count = count;
    }

    if (ftm_args.burst_period->count != 0) {
        if (ftm_args.burst_period->ival[0] >= 0 &&
                ftm_args.burst_period->ival[0] <= 100) {
            ftmi_cfg.burst_period = ftm_args.burst_period->ival[0];
        } else {
            ESP_LOGE(TAG_STA, "Invalid Burst Period! Valid range is 0-100");
            return 0;
        }
    }

    if (ftmi_cfg.frm_count == 0) {
        ESP_LOGI(TAG_STA, "Requesting FTM session in ASAP mode (fastest, most reliable)");
    } else {
        ESP_LOGI(TAG_STA, "Requesting FTM session with Frm Count - %d, Burst Period - %dmSec (0: No Preference)",
                 ftmi_cfg.frm_count, ftmi_cfg.burst_period*100);
    }

    /* Clear any stale event bits from previous measurements */
    xEventGroupClearBits(s_ftm_event_group, FTM_REPORT_BIT | FTM_FAILURE_BIT);

    if (ESP_OK != esp_wifi_ftm_initiate_session(&ftmi_cfg)) {
        ESP_LOGE(TAG_STA, "Failed to start FTM session");
        return 0;
    }

    /* Calculate appropriate timeout based on FTM mode
     * ASAP mode (frm_count=0): Quick single burst, use default timeout
     * Non-ASAP mode (frm_count>0): Multiple bursts need longer timeout
     */
    if (ftmi_cfg.frm_count == 0) {
        /* ASAP mode - single burst, short timeout is fine */
        wait_time_ms = DEFAULT_WAIT_TIME_MS;
    } else if (ftmi_cfg.burst_period > 0) {
        /* Non-ASAP mode with defined burst period
         * Calculate number of bursts: frm_count / 8 (max 8 frames per burst)
         * Add extra time margin for processing
         */
        uint8_t num_bursts = (ftmi_cfg.frm_count + 7) / 8;  // Round up
        if (num_bursts > MAX_FTM_BURSTS) {
            num_bursts = MAX_FTM_BURSTS;
        }
        /* Total time = (bursts * period) + 50% margin + initial delay */
        wait_time_ms = (ftmi_cfg.burst_period * 100 * num_bursts * 3 / 2) + 5000;
    } else {
        /* Non-ASAP mode with no preference - use extended timeout */
        wait_time_ms = DEFAULT_WAIT_TIME_MS * 3;
    }

    ESP_LOGI(TAG_STA, "Waiting up to %lu ms for FTM session to complete", wait_time_ms);
    bits = xEventGroupWaitBits(s_ftm_event_group, FTM_REPORT_BIT | FTM_FAILURE_BIT,
                                           pdTRUE, pdFALSE, wait_time_ms / portTICK_PERIOD_MS);
    if (bits & FTM_REPORT_BIT) {
        /* RSSI already extracted in event handler */
        /* Buffer auto-freed by WiFi driver (use_get_report_api=false) */

        ESP_LOGI(TAG_STA, "Estimated RTT - %" PRId32 " nSec, Estimated Distance - %" PRId32 ".%02" PRId32 " meters",
                          s_rtt_est, s_dist_est / 100, s_dist_est % 100);
        /* Log heap after single-shot measurement (to detect leaks) */
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG_STA, "Heap after single-shot FTM: %zu bytes free", free_heap);
    } else if (bits & FTM_FAILURE_BIT) {
        /* FTM Failure case */
        ESP_LOGE(TAG_STA, "FTM procedure failed!");
    } else {
        /* Timeout, end session gracefully */
        ESP_LOGE(TAG_STA, "FTM procedure timed out!");
        esp_wifi_ftm_end_session();
    }

    return 0;

ftm_responder:
    if (ftm_args.offset->count != 0) {
        int16_t offset_cm = ftm_args.offset->ival[0];

        esp_wifi_ftm_resp_set_offset(offset_cm);
    }

    if (ftm_args.enable->count != 0) {
        if (!s_ap_started) {
            ESP_LOGE(TAG_AP, "Start the SoftAP first with 'ap' command");
            return 0;
        }
        g_ap_config.ap.ftm_responder = true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &g_ap_config));
        ESP_LOGI(TAG_AP, "Re-starting SoftAP with FTM Responder enabled");

        return 0;
    }

    if (ftm_args.disable->count != 0) {
        if (!s_ap_started) {
            ESP_LOGE(TAG_AP, "Start the SoftAP first with 'ap' command");
            return 0;
        }
        g_ap_config.ap.ftm_responder = false;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &g_ap_config));
        ESP_LOGI(TAG_AP, "Re-starting SoftAP with FTM Responder disabled");
    }

    return 0;
}

void register_wifi(void)
{
    sta_args.ssid = arg_str0(NULL, NULL, "<ssid>", "SSID of AP");
    sta_args.password = arg_str0(NULL, NULL, "<pass>", "password of AP");
    sta_args.disconnect = arg_lit0("d", "disconnect", "Disconnect from the connected AP");
    sta_args.end = arg_end(2);

    const esp_console_cmd_t sta_cmd = {
        .command = "sta",
        .help = "WiFi is station mode, join specified soft-AP",
        .hint = NULL,
        .func = &wifi_cmd_sta,
        .argtable = &sta_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&sta_cmd) );

    ap_args.ssid = arg_str0(NULL, NULL, "<ssid>", "SSID of AP");
    ap_args.password = arg_str0(NULL, NULL, "<pass>", "password of AP");
    ap_args.channel = arg_int0("c", "channel", "<1-11>", "Primary channel of AP");
    ap_args.bandwidth = arg_int0("b", "bandwidth", "<20/40>", "Channel bandwidth");
    ap_args.end = arg_end(2);

    const esp_console_cmd_t ap_cmd = {
        .command = "ap",
        .help = "AP mode, configure ssid and password",
        .hint = NULL,
        .func = &wifi_cmd_ap,
        .argtable = &ap_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&ap_cmd) );

    scan_args.ssid = arg_str0(NULL, NULL, "<ssid>", "SSID of AP want to be scanned");
    scan_args.end = arg_end(1);

    const esp_console_cmd_t scan_cmd = {
        .command = "scan",
        .help = "WiFi is station mode, start scan ap",
        .hint = NULL,
        .func = &wifi_cmd_scan,
        .argtable = &scan_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&scan_cmd) );

    const esp_console_cmd_t query_cmd = {
        .command = "query",
        .help = "query WiFi info",
        .hint = NULL,
        .func = &wifi_cmd_query,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&query_cmd) );

    /* FTM Initiator commands */
    ftm_args.initiator = arg_lit0("I", "ftm_initiator", "FTM Initiator mode");
    ftm_args.ssid = arg_str0("s", "ssid", "SSID", "SSID of AP");
    ftm_args.frm_count = arg_int0("c", "frm_count", "<0/8/16/24/32/64>", "FTM frames to be exchanged (0: No preference)");
    ftm_args.burst_period = arg_int0("p", "burst_period", "<0-100 (x 100 mSec)>", "Periodicity of FTM bursts in 100's of milliseconds (0: No preference)");
    /* FTM Responder commands */
    ftm_args.responder = arg_lit0("R", "ftm_responder", "FTM Responder mode");
    ftm_args.enable = arg_lit0("e", "enable", "Restart SoftAP with FTM enabled");
    ftm_args.disable = arg_lit0("d", "disable", "Restart SoftAP with FTM disabled");
    ftm_args.offset = arg_int0("o", "offset", "Offset in cm", "T1 offset in cm for FTM Responder");
    ftm_args.end = arg_end(1);

    const esp_console_cmd_t ftm_cmd = {
        .command = "ftm",
        .help = "FTM command",
        .hint = NULL,
        .func = &wifi_cmd_ftm,
        .argtable = &ftm_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&ftm_cmd) );
}

/* Screen Navigation Console Commands */
static struct {
    struct arg_int *screen_id;
    struct arg_end *end;
} screen_args;

static int screen_cmd_goto(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&screen_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, screen_args.end, argv[0]);
        return 1;
    }

    if (screen_args.screen_id->count > 0) {
        int screen_id = screen_args.screen_id->ival[0];
        const char *screen_names[] = {"HOME", "AP_SCANNER", "MEASUREMENT", "SETTINGS", "RESPONDER", "SYSTEM_INFO"};

        if (screen_id < 0 || screen_id >= 6) {
            printf("Invalid screen ID. Valid range: 0-5\n");
            printf("  0: HOME\n  1: AP_SCANNER\n  2: MEASUREMENT\n  3: SETTINGS\n  4: RESPONDER\n  5: SYSTEM_INFO\n");
            return 1;
        }

        esp_err_t ret = ftm_screen_navigate_to(screen_id);
        if (ret == ESP_OK) {
            printf("Navigating to screen %d (%s)\n", screen_id, screen_names[screen_id]);
        } else {
            printf("Failed to navigate to screen %d: %s\n", screen_id, esp_err_to_name(ret));
            return 1;
        }
    }

    return 0;
}

static int screen_cmd_next(int argc, char **argv)
{
    esp_err_t ret = ftm_screen_navigate_next();
    if (ret == ESP_OK) {
        int current = ftm_screen_get_current();
        const char *screen_names[] = {"HOME", "AP_SCANNER", "MEASUREMENT", "SETTINGS", "RESPONDER", "SYSTEM_INFO"};
        printf("Navigated to next screen: %d (%s)\n", current, screen_names[current]);
    } else {
        printf("Failed to navigate to next screen: %s\n", esp_err_to_name(ret));
        return 1;
    }
    return 0;
}

static int screen_cmd_prev(int argc, char **argv)
{
    esp_err_t ret = ftm_screen_navigate_prev();
    if (ret == ESP_OK) {
        int current = ftm_screen_get_current();
        const char *screen_names[] = {"HOME", "AP_SCANNER", "MEASUREMENT", "SETTINGS", "RESPONDER", "SYSTEM_INFO"};
        printf("Navigated to previous screen: %d (%s)\n", current, screen_names[current]);
    } else {
        printf("Failed to navigate to previous screen: %s\n", esp_err_to_name(ret));
        return 1;
    }
    return 0;
}

static int screen_cmd_current(int argc, char **argv)
{
    int current = ftm_screen_get_current();
    const char *screen_names[] = {"HOME", "AP_SCANNER", "MEASUREMENT", "SETTINGS", "RESPONDER", "SYSTEM_INFO"};
    printf("Current screen: %d (%s)\n", current, screen_names[current]);
    return 0;
}

void register_screen(void)
{
    screen_args.screen_id = arg_int0(NULL, NULL, "<0-5>", "Screen ID (0:HOME, 1:AP_SCANNER, 2:MEASUREMENT, 3:SETTINGS, 4:RESPONDER, 5:SYSTEM_INFO)");
    screen_args.end = arg_end(1);

    const esp_console_cmd_t screen_goto_cmd = {
        .command = "screen",
        .help = "Navigate to specific screen by ID",
        .hint = NULL,
        .func = &screen_cmd_goto,
        .argtable = &screen_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&screen_goto_cmd) );

    const esp_console_cmd_t screen_next_cmd = {
        .command = "screen_next",
        .help = "Navigate to next screen",
        .hint = NULL,
        .func = &screen_cmd_next,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&screen_next_cmd) );

    const esp_console_cmd_t screen_prev_cmd = {
        .command = "screen_prev",
        .help = "Navigate to previous screen",
        .hint = NULL,
        .func = &screen_cmd_prev,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&screen_prev_cmd) );

    const esp_console_cmd_t screen_current_cmd = {
        .command = "screen_current",
        .help = "Show current screen",
        .hint = NULL,
        .func = &screen_cmd_current,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&screen_current_cmd) );

    ESP_LOGI("screen_cmd", "Screen navigation commands registered");
}

/* Heap Trace Command - Memory Leak Diagnostics */
static int heap_trace_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: heap_trace <start|stop|dump>\n");
        printf("  start - Begin heap tracing\n");
        printf("  stop  - Stop heap tracing\n");
        printf("  dump  - Dump heap trace (shows leaked allocations)\n");
        return 1;
    }

    if (strcmp(argv[1], "start") == 0) {
        esp_err_t ret = heap_trace_init_standalone(s_heap_trace_records, HEAP_TRACE_RECORDS);
        if (ret != ESP_OK) {
            printf("Failed to init heap trace: %s\n", esp_err_to_name(ret));
            return 1;
        }

        ret = heap_trace_start(HEAP_TRACE_LEAKS);  // Only track unfreed allocations
        if (ret != ESP_OK) {
            printf("Failed to start heap trace: %s\n", esp_err_to_name(ret));
            return 1;
        }

        s_heap_tracing_enabled = true;
        printf("Heap tracing started (max %d records)\n", HEAP_TRACE_RECORDS);
        printf("Run continuous FTM, then use 'heap_trace dump' to see leaks\n");
    }
    else if (strcmp(argv[1], "stop") == 0) {
        esp_err_t ret = heap_trace_stop();
        if (ret != ESP_OK) {
            printf("Failed to stop heap trace: %s\n", esp_err_to_name(ret));
            return 1;
        }

        s_heap_tracing_enabled = false;
        printf("Heap tracing stopped\n");
    }
    else if (strcmp(argv[1], "dump") == 0) {
        printf("Dumping heap trace records:\n");
        printf("NOTE: Look for allocations without matching frees\n");
        printf("======================================\n");
        heap_trace_dump();
        printf("======================================\n");
    }
    else {
        printf("Unknown command: %s\n", argv[1]);
        printf("Usage: heap_trace <start|stop|dump>\n");
        return 1;
    }

    return 0;
}

void register_heap_trace(void)
{
    const esp_console_cmd_t heap_trace_command = {
        .command = "heap_trace",
        .help = "Control heap tracing for memory leak detection (start|stop|dump)",
        .hint = NULL,
        .func = &heap_trace_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&heap_trace_command));
    ESP_LOGI("heap_trace", "Heap trace command registered");
}

/* Known FTM Responder SSIDs - whitelist to work around ESP32-C6 scan limitation */
static const char *known_ftm_responders[] = {
    "arista_test_2g",
    "FTM",
    "FTM_TEST",
    "TaterTotWifi"
};
static const int num_known_ftm_responders = sizeof(known_ftm_responders) / sizeof(known_ftm_responders[0]);

/* Check if an SSID is a known FTM responder */
bool is_known_ftm_responder(const char *ssid) {
    /* Check for TatorTot_ prefix (any SSID starting with this) */
    if (strncmp(ssid, "TatorTot_", 9) == 0) {
        return true;
    }

    /* Check exact match whitelist for other known FTM responders */
    for (int i = 0; i < num_known_ftm_responders; i++) {
        if (strcmp(ssid, known_ftm_responders[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Battery status callback - updates display */
static void battery_status_callback(const battery_status_t *status)
{
    /* Battery callback logging disabled to reduce console noise */
    // ESP_LOGI("main", "Battery callback: %lu mV, %d%%", status->voltage_mv, status->percentage);

    /* Update the display with battery info */
    ftm_display_update_battery(status->voltage_mv, status->percentage);

    /* Show battery icon on first update */
    static bool first_update = true;
    if (first_update) {
        ESP_LOGI("main", "First battery update - showing icon");
        ftm_display_show_battery();
        first_update = false;
    }

    /* Stop continuous mode if battery is critical */
    if (status->percentage < 5 && get_continuous_mode()) {
        ESP_LOGW("main", "Battery critical (%d%%) - stopping continuous FTM mode", status->percentage);
        set_continuous_mode(false, NULL);
        ftm_display_set_continuous_indicator(false);
        esp_wifi_ftm_end_session();
        set_ftm_in_progress(false);
        ftm_display_update_status_text("Batt Low", lv_color_make(255, 100, 100));
    }
}

/* Continuous mode state management functions */
static void set_continuous_mode(bool active, wifi_ap_record_t *target) {
    if (s_continuous_mode_mutex == NULL) {
        ESP_LOGE("main", "Continuous mode mutex not initialized");
        return;
    }

    if (xSemaphoreTake(s_continuous_mode_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_continuous_mode_active = active;
        if (target && active) {
            memcpy(&s_continuous_target_ap, target, sizeof(wifi_ap_record_t));
            ESP_LOGI("main", "Continuous mode target set to: %s", s_continuous_target_ap.ssid);

            /* CRITICAL: Clear stale event bits from previous measurements */
            xEventGroupClearBits(s_ftm_event_group, FTM_REPORT_BIT | FTM_FAILURE_BIT);

            /* Start continuous mode task if not already running */
            if (s_continuous_mode_task == NULL) {
                /* Priority 3: Lower than event task to avoid preempting event handler */
                BaseType_t result = xTaskCreate(continuous_mode_task, "ftm_cont", 4096, NULL, 3, &s_continuous_mode_task);
                if (result != pdPASS) {
                    ESP_LOGE("main", "Failed to create continuous mode task");
                    s_continuous_mode_active = false;
                }
            }
        }
        if (!active) {
            s_continuous_failure_count = 0;  // Reset failure counter when stopping
            s_ftm_cycle_count = 0;  // Reset cycle counter when stopping

            /* Task will exit on its own when it sees continuous mode is false */
            /* Just wait a bit for it to clean up */
            if (s_continuous_mode_task != NULL) {
                ESP_LOGI("main", "Waiting for continuous mode task to exit...");
            }
        }
        xSemaphoreGive(s_continuous_mode_mutex);
    }
}

static bool get_continuous_mode(void) {
    bool active = false;
    if (s_continuous_mode_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_continuous_mode_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = s_continuous_mode_active;
        xSemaphoreGive(s_continuous_mode_mutex);
    }
    return active;
}

static esp_err_t initiate_ftm_to_target(wifi_ap_record_t *ap_record) {
    if (!ap_record) {
        ESP_LOGE("main", "Invalid AP record");
        return ESP_ERR_INVALID_ARG;
    }

    /* Update display */
    ftm_display_update_ap_info((const char *)ap_record->ssid, ap_record->rssi, ap_record->primary);
    ftm_display_update_ap_name((const char *)ap_record->ssid);
    ftm_display_update_status(FTM_DISPLAY_MEASURING);

    /* Prepare FTM configuration */
    wifi_ftm_initiator_cfg_t ftmi_cfg = {
        .frm_count = 0,      // ASAP mode (fastest)
        .burst_period = 2,
        .use_get_report_api = false,  // CRITICAL: Use direct event access (driver manages buffer)
    };
    memcpy(ftmi_cfg.resp_mac, ap_record->bssid, 6);
    ftmi_cfg.channel = ap_record->primary;

    /* Initiate FTM session */
    esp_err_t ret = esp_wifi_ftm_initiate_session(&ftmi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE("main", "Failed to start FTM session");
        ftm_display_update_status(FTM_DISPLAY_FAILED);
        return ret;
    }

    ESP_LOGI("main", "FTM session initiated with %s", ap_record->ssid);
    return ESP_OK;
}

/* Display button callback functions */
static void display_scan_button_cb(void) {
    ESP_LOGI("main", "Display SCAN button pressed - initiating WiFi scan");

    /* Check if scan is already in progress */
    if (get_scan_in_progress()) {
        ESP_LOGW("main", "Scan already in progress - ignoring button press");
        return;
    }

    /* Check if FTM is in progress - stop continuous mode if active */
    if (get_ftm_in_progress()) {
        if (get_continuous_mode()) {
            ESP_LOGI("main", "Stopping continuous mode to allow scan");
            set_continuous_mode(false, NULL);
            ftm_display_set_continuous_indicator(false);

            /* End current FTM session */
            esp_wifi_ftm_end_session();

            /* Wait for FTM to fully stop - reduced from 1000ms for better UI responsiveness */
            vTaskDelay(pdMS_TO_TICKS(300));

            /* Reset FTM flag */
            set_ftm_in_progress(false);
        } else {
            /* Non-continuous FTM in progress - show warning and return */
            ESP_LOGW("main", "FTM measurement in progress - cannot scan");
            ftm_display_update_status_text("FTM Active", lv_color_make(255, 200, 100));
            vTaskDelay(pdMS_TO_TICKS(500));  // Reduced from 1000ms - brief message display
            return;
        }
    }

    /* Set flag to prevent concurrent scans */
    set_scan_in_progress(true);

    /* Update status immediately to show scan is starting */
    ftm_display_update_status_text("Scanning...", lv_color_make(100, 150, 255));

    /* Brief delay to allow LVGL to render the "Scanning..." status before scan */
    vTaskDelay(pdMS_TO_TICKS(50));  // Reduced from 100ms - minimal render time

    /* Perform the scan */
    bool result = wifi_perform_scan(NULL, false);

    /* Clear flag when done */
    set_scan_in_progress(false);

    if (!result) {
        ESP_LOGW("main", "Scan failed");
    } else {
        /* Navigate to AP Scanner screen to show results */
        ESP_LOGI("main", "Scan complete - navigating to AP Scanner screen");
        ftm_screen_navigate_to(1);  // SCREEN_AP_SCANNER = 1
    }
}

static void display_ftm_button_cb(void) {
    ESP_LOGI("main", "Display FTM button pressed - checking for FTM responder");

    /* Check if FTM is already in progress */
    if (get_ftm_in_progress()) {
        ESP_LOGW("main", "FTM measurement already in progress - ignoring button press");
        return;
    }

    /* Try to initiate FTM with the last scanned AP that supports FTM */
    if (g_ap_list_buffer && g_scan_ap_num > 0) {
        wifi_ap_record_t *target_ap = NULL;

        /* Highest priority: Use AP explicitly selected from UI */
        if (s_selected_ap_index >= 0 && s_selected_ap_index < g_scan_ap_num) {
            ESP_LOGI("main", "Using AP selected from UI: index %d (%s)",
                     s_selected_ap_index, g_ap_list_buffer[s_selected_ap_index].ssid);
            target_ap = &g_ap_list_buffer[s_selected_ap_index];
            /* Clear selection after use */
            s_selected_ap_index = -1;
        }

        /* Second priority: Check whitelist of known FTM responders */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (is_known_ftm_responder((const char *)g_ap_list_buffer[i].ssid)) {
                    ESP_LOGI("main", "Found known FTM responder from whitelist: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Third priority: Check for FTM responder flag (may not work on C6) */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (g_ap_list_buffer[i].ftm_responder) {
                    ESP_LOGI("main", "Found FTM responder by flag: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Last resort: Use first AP (assume it might support FTM) */
        if (!target_ap) {
            ESP_LOGW("main", "No known FTM responder found - trying first AP: %s", g_ap_list_buffer[0].ssid);
            target_ap = &g_ap_list_buffer[0];
        }

        /* Store target AP for display info preservation */
        memcpy(&s_last_ftm_target_ap, target_ap, sizeof(wifi_ap_record_t));

        /* Update display status immediately to show measurement is starting */
        ftm_display_update_status_text("Measuring...", lv_color_make(255, 255, 100));

        /* Update display with AP details (unlocked: called from LVGL callback) */
        ftm_display_update_ap_info((const char *)target_ap->ssid, target_ap->rssi, target_ap->primary);

        /* Set flag to prevent concurrent FTM sessions */
        set_ftm_in_progress(true);

        /* Initiate FTM session */
        wifi_ftm_initiator_cfg_t ftmi_cfg = {
            .frm_count = 0,      // ASAP mode (fastest)
            .burst_period = 2,
            .use_get_report_api = false,  // CRITICAL: Use direct event access (driver manages buffer)
        };
        memcpy(ftmi_cfg.resp_mac, target_ap->bssid, 6);
        ftmi_cfg.channel = target_ap->primary;

        if (ESP_OK != esp_wifi_ftm_initiate_session(&ftmi_cfg)) {
            ESP_LOGE("main", "Failed to start FTM session");
            ftm_display_update_status_text("Failed", lv_color_make(255, 100, 100));
            set_ftm_in_progress(false);  // Clear flag on failure
        } else {
            ESP_LOGI("main", "FTM session initiated with %s", target_ap->ssid);
            /* Flag will be cleared when FTM event handler receives completion/failure */
        }
    } else {
        ESP_LOGW("main", "No scan results available - please scan first");
        ftm_display_update_status_text("No APs", lv_color_make(255, 100, 100));
    }
}

/* FTM long-press callback - toggles continuous mode */
static void display_ftm_long_press_cb(void) {
    ESP_LOGI("main", "FTM button long-press detected");

    /* Check if continuous mode is currently active */
    if (get_continuous_mode()) {
        /* Stop continuous mode */
        ESP_LOGI("main", "Stopping continuous FTM mode");
        set_continuous_mode(false, NULL);
        ftm_display_set_continuous_indicator(false);
        esp_wifi_ftm_end_session();
        set_ftm_in_progress(false);
        ftm_display_update_status(FTM_DISPLAY_IDLE);
    } else {
        /* Start continuous mode */
        if (get_ftm_in_progress()) {
            ESP_LOGW("main", "Cannot start continuous - FTM already in progress");
            return;
        }

        if (!g_ap_list_buffer || g_scan_ap_num == 0) {
            ESP_LOGW("main", "Cannot start continuous - no scan results available");
            ftm_display_update_status_text("Scan first", lv_color_make(255, 100, 100));
            return;
        }

        /* Use same target selection logic as single-shot */
        wifi_ap_record_t *target_ap = NULL;

        /* Highest priority: Use last FTM target if available (keeps same AP for continuous) */
        if (s_last_ftm_target_ap.ssid[0] != '\0') {
            /* Find the last target in current scan results */
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (strcmp((const char *)g_ap_list_buffer[i].ssid, (const char *)s_last_ftm_target_ap.ssid) == 0) {
                    ESP_LOGI("main", "Reusing last FTM target for continuous: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Second priority: Check whitelist of known FTM responders */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (is_known_ftm_responder((const char *)g_ap_list_buffer[i].ssid)) {
                    ESP_LOGI("main", "Found known FTM responder from whitelist: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Third priority: Check for FTM responder flag (may not work on C6) */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (g_ap_list_buffer[i].ftm_responder) {
                    ESP_LOGI("main", "Found FTM responder by flag: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Last resort: Use first AP (assume it might support FTM) */
        if (!target_ap) {
            ESP_LOGW("main", "No known FTM responder found - trying first AP: %s", g_ap_list_buffer[0].ssid);
            target_ap = &g_ap_list_buffer[0];
        }

        ESP_LOGI("main", "Starting continuous FTM mode with %s", target_ap->ssid);

        /* Store target AP for display info preservation */
        memcpy(&s_last_ftm_target_ap, target_ap, sizeof(wifi_ap_record_t));

        /* Set continuous mode active and save target */
        set_continuous_mode(true, target_ap);
        ftm_display_set_continuous_indicator(true);  // Change to dark blue (unlocked: called from LVGL callback)

        /* Update display with continuous mode status and responder info */
        ftm_display_update_status_text("Continuous", lv_color_make(100, 200, 255));
        ftm_display_update_ap_info((const char *)target_ap->ssid, target_ap->rssi, target_ap->primary);

        /* Set busy flag - continuous task will initiate first measurement */
        set_ftm_in_progress(true);
        /* Note: First measurement now initiated by continuous task, not here */
    }
}

/* Power button GPIO number */
#define POWER_BUTTON_GPIO  1

/* Display/Battery power control GPIO (shared on Waveshare board) */
#define LCD_POWER_GPIO  GPIO_NUM_2

/* Shutdown callback - called from display button or power button */
static void display_shutdown_cb(void)
{
    ESP_LOGW("main", "=== SHUTDOWN REQUESTED ===");

    /* Stop continuous mode if active */
    if (get_continuous_mode()) {
        ESP_LOGI("main", "Stopping continuous mode before shutdown");
        set_continuous_mode(false, NULL);
        esp_wifi_ftm_end_session();
        set_ftm_in_progress(false);
    }

    /* Update display to show shutdown message */
    ftm_display_update_status_text("Powering off...", lv_color_make(255, 100, 100));
    vTaskDelay(pdMS_TO_TICKS(1500));  /* Show message */

    /* Stop WiFi to reduce power */
    ESP_LOGI("main", "Stopping WiFi...");
    esp_wifi_stop();

    /* Wait for power button to be fully released */
    ESP_LOGI("main", "Waiting for button release...");
    int release_wait = 0;
    while (gpio_get_level(POWER_BUTTON_GPIO) == 0 && release_wait < 60) {
        vTaskDelay(pdMS_TO_TICKS(50));
        release_wait++;
    }
    ESP_LOGI("main", "Button released after %d ms", release_wait * 50);
    vTaskDelay(pdMS_TO_TICKS(300));  /* Extra debounce */

    /* Disable GPIO hold that was set during battery init */
    ESP_LOGI("main", "Releasing GPIO hold on power control...");
    gpio_hold_dis(LCD_POWER_GPIO);

    /* Turn off display and battery power
     * On battery: This cuts power completely - device turns off
     * On USB: Device may continue running on USB power */
    ESP_LOGW("main", "Cutting power (GPIO %d LOW)...", LCD_POWER_GPIO);
    gpio_set_level(LCD_POWER_GPIO, 0);

    /* Brief delay to let power rail drop */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* If we're still running (USB power), enter deep sleep */
    ESP_LOGI("main", "Still running - entering deep sleep (USB power detected)");

    /* Configure wakeup on power button press */
    esp_sleep_enable_ext1_wakeup_io(1ULL << POWER_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);

    /* Isolate GPIO to minimize power in deep sleep */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    ESP_LOGW("main", "Entering deep sleep now...");
    esp_deep_sleep_start();

    /* Should never reach here */
    ESP_LOGE("main", "Deep sleep failed!");
}

/* Power button ISR handler */
static volatile uint32_t s_power_btn_press_time = 0;
static volatile bool s_power_btn_pressed = false;

static void IRAM_ATTR power_button_isr_handler(void *arg)
{
    static uint32_t last_isr_time = 0;
    uint32_t now = xTaskGetTickCountFromISR();

    /* Debounce: ignore if less than 50ms since last interrupt */
    if ((now - last_isr_time) < pdMS_TO_TICKS(50)) {
        return;
    }
    last_isr_time = now;

    int level = gpio_get_level(POWER_BUTTON_GPIO);
    if (level == 0) {
        /* Button pressed (active low) */
        s_power_btn_press_time = now;
        s_power_btn_pressed = true;
    } else {
        /* Button released */
        s_power_btn_pressed = false;
    }
}

/* Power button monitoring task */
static void power_button_task(void *pvParameters)
{
    const uint32_t long_press_ms = 1500;  /* 1.5 seconds for shutdown */

    while (1) {
        if (s_power_btn_pressed) {
            uint32_t now = xTaskGetTickCount();
            uint32_t press_duration = (now - s_power_btn_press_time) * portTICK_PERIOD_MS;

            if (press_duration >= long_press_ms) {
                ESP_LOGI("main", "Power button long press detected - shutting down");
                s_power_btn_pressed = false;  /* Reset to prevent multiple triggers */
                display_shutdown_cb();
                /* Never returns from deep sleep */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));  /* Check every 50ms */
    }
}

/* Initialize power button */
static void power_button_init(void)
{
    ESP_LOGI("main", "Initializing power button on GPIO %d", POWER_BUTTON_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << POWER_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    /* Install GPIO ISR service and add handler */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(POWER_BUTTON_GPIO, power_button_isr_handler, NULL);

    /* Start power button monitoring task */
    xTaskCreate(power_button_task, "pwr_btn", 2048, NULL, 5, NULL);

    ESP_LOGI("main", "Power button initialized - long press (1.5s) to shutdown");
}

/* FTM Responder API functions for display */
bool ftm_responder_is_running(void)
{
    return s_ap_started;
}

esp_err_t ftm_responder_start(const char *ssid, const char *password, uint8_t channel, uint8_t bandwidth)
{
    ESP_LOGI(TAG_AP, "Starting FTM Responder: SSID=%s, Channel=%d, BW=%dMHz", ssid, channel, bandwidth);

    /* Validate parameters */
    if (channel < 1 || channel > 14) {
        ESP_LOGE(TAG_AP, "Invalid channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    if (bandwidth != 20 && bandwidth != 40) {
        ESP_LOGE(TAG_AP, "Invalid bandwidth: %d", bandwidth);
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(password) > 0 && strlen(password) < 8) {
        ESP_LOGE(TAG_AP, "Password must be at least 8 characters");
        return ESP_ERR_INVALID_ARG;
    }

    /* Configure AP with FTM responder enabled */
    g_ap_config.ap.ftm_responder = true;

    /* Use the existing wifi_cmd_ap_set function */
    if (!wifi_cmd_ap_set(ssid, password, channel, bandwidth)) {
        ESP_LOGE(TAG_AP, "Failed to start FTM Responder");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ftm_responder_stop(void)
{
    ESP_LOGI(TAG_AP, "Stopping FTM Responder");

    if (!s_ap_started) {
        ESP_LOGW(TAG_AP, "FTM Responder not running");
        return ESP_ERR_INVALID_STATE;
    }

    /* Stop WiFi AP mode */
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_AP) {
        /* Pure AP mode - switch to NULL */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    } else if (mode == WIFI_MODE_APSTA) {
        /* AP+STA mode - switch to STA only */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    return ESP_OK;
}

const char* ftm_responder_get_ssid(void)
{
    return (const char*)g_ap_config.ap.ssid;
}

uint8_t ftm_responder_get_channel(void)
{
    return g_ap_config.ap.channel;
}

/* Continuous Mode API functions for display */
bool ftm_continuous_mode_is_running(void)
{
    return get_continuous_mode();
}

esp_err_t ftm_continuous_mode_toggle(void)
{
    /* Check if continuous mode is currently active */
    if (get_continuous_mode()) {
        /* Stop continuous mode */
        ESP_LOGI("main", "Stopping continuous FTM mode (from settings button)");
        set_continuous_mode(false, NULL);
        ftm_display_set_continuous_indicator(false);
        esp_wifi_ftm_end_session();
        set_ftm_in_progress(false);
        ftm_display_update_status(FTM_DISPLAY_IDLE);
        return ESP_OK;
    } else {
        /* Start continuous mode - find an FTM responder */
        ESP_LOGI("main", "Starting continuous FTM mode (from settings button)");

        /* Ensure we have scan results */
        if (g_scan_ap_num == 0 || g_ap_list_buffer == NULL) {
            ESP_LOGW("main", "No scan results available - cannot start continuous mode");
            return ESP_ERR_INVALID_STATE;
        }

        /* Find FTM responder - prioritize last used target */
        wifi_ap_record_t *target_ap = NULL;

        /* Highest priority: Use last FTM target if available (keeps same AP for continuous) */
        if (s_last_ftm_target_ap.ssid[0] != '\0') {
            /* Find the last target in current scan results */
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (strcmp((const char *)g_ap_list_buffer[i].ssid, (const char *)s_last_ftm_target_ap.ssid) == 0) {
                    ESP_LOGI("main", "Reusing last FTM target for continuous: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Second priority: Check whitelist of known FTM responders */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (is_known_ftm_responder((const char *)g_ap_list_buffer[i].ssid)) {
                    ESP_LOGI("main", "Found known FTM responder from whitelist: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Third priority: Check ftm_responder flag */
        if (!target_ap) {
            for (uint8_t i = 0; i < g_scan_ap_num; i++) {
                if (g_ap_list_buffer[i].ftm_responder) {
                    ESP_LOGI("main", "Found FTM responder by flag: %s", g_ap_list_buffer[i].ssid);
                    target_ap = &g_ap_list_buffer[i];
                    break;
                }
            }
        }

        /* Last resort: Use first AP (assume it might support FTM) */
        if (!target_ap) {
            ESP_LOGW("main", "No known FTM responder found - trying first AP: %s", g_ap_list_buffer[0].ssid);
            target_ap = &g_ap_list_buffer[0];
        }

        /* Store target AP for display info preservation */
        memcpy(&s_last_ftm_target_ap, target_ap, sizeof(wifi_ap_record_t));

        /* Set continuous mode active and save target */
        set_continuous_mode(true, target_ap);
        ftm_display_set_continuous_indicator(true);

        /* Update display with continuous mode status and responder info */
        ftm_display_update_status_text("Continuous", lv_color_make(100, 200, 255));
        ftm_display_update_ap_info((const char *)target_ap->ssid, target_ap->rssi, target_ap->primary);

        /* Set busy flag before starting */
        set_ftm_in_progress(true);

        /* Start first measurement (rest will auto-loop in event handler) */
        if (initiate_ftm_to_target(target_ap) != ESP_OK) {
            /* Failed to start - clean up */
            ESP_LOGE("main", "Failed to start continuous mode");
            set_continuous_mode(false, NULL);
            ftm_display_set_continuous_indicator(false);
            set_ftm_in_progress(false);
            return ESP_FAIL;
        }

        return ESP_OK;
    }
}

/* Set the selected AP index from UI (for AP scanner list clicks) */
void ftm_set_selected_ap_index(int16_t index)
{
    s_selected_ap_index = index;
    if (index >= 0) {
        ESP_LOGI("main", "AP index %d selected from UI", index);
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    /* Initialize the display*/
    ESP_LOGI("main", "Initializing display...");
    ret = ftm_display_init();
    if (ret == ESP_OK) {
        ESP_LOGI("main", "Display initialized successfully");
        ftm_display_task_start();

        /* Initialize screen manager and navigate to HOME screen */
        ret = ftm_screen_manager_init();
        if (ret == ESP_OK) {
            ESP_LOGI("main", "Screen manager initialized");
            ret = ftm_screen_navigate_to(0);  // Navigate to SCREEN_HOME (id=0)
            if (ret == ESP_OK) {
                ESP_LOGI("main", "Navigated to HOME screen");
            } else {
                ESP_LOGE("main", "Failed to navigate to HOME screen");
            }
        } else {
            ESP_LOGE("main", "Screen manager initialization failed");
        }

        ftm_display_update_status(FTM_DISPLAY_IDLE);

        /* Register button callbacks */
        ftm_display_set_scan_callback(display_scan_button_cb);
        ftm_display_set_ftm_callback(display_ftm_button_cb);
        ftm_display_set_center_click_callback(display_ftm_long_press_cb);  // Center-click toggles continuous mode
        ftm_display_set_shutdown_callback(display_shutdown_cb);  // Shutdown button on system info screen
        ESP_LOGI("main", "Display button callbacks registered (center-click for continuous mode)");
    } else {
        ESP_LOGE("main", "Display initialization failed, continuing without display");
    }

    /* Initialize power button for shutdown */
    power_button_init();

    initialise_wifi();

    /* Initialize and start battery monitoring */
    ESP_LOGI("main", "Initializing battery monitor...");
    ret = battery_monitor_init();
    if (ret == ESP_OK) {
        ESP_LOGI("main", "Battery monitor initialized successfully");

        /* Register callback to update display */
        battery_register_callback(battery_status_callback);

        /* Start monitoring task */
        ret = battery_monitor_start();
        if (ret == ESP_OK) {
            ESP_LOGI("main", "Battery monitoring started");
        } else {
            ESP_LOGW("main", "Failed to start battery monitoring task");
        }
    } else {
        ESP_LOGW("main", "Battery monitor initialization failed, continuing without battery monitoring");
        /* Show a test battery icon anyway */
        ESP_LOGI("main", "Showing test battery display (simulated 75%%)");
        vTaskDelay(pdMS_TO_TICKS(2000)); // Wait for display to be ready
        ftm_display_update_battery(3700, 75);
        ftm_display_show_battery();
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    repl_config.prompt = "ftm> ";

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif

    /* Register commands */
    register_system();
    register_wifi();
    register_screen();
    register_heap_trace();

    printf("\n ===============================================================\n");
    printf(" |                  Steps to test FTM Initiator                 |\n");
    printf(" |                                                              |\n");
    printf(" |  1. Use 'scan' command to scan AP's. In results, check AP's  |\n");
    printf(" |     with tag '[FTM Responder]' for supported AP's            |\n");
    printf(" |  2. Optionally connect to the AP with 'sta <SSID> <password>'|\n");
    printf(" |  3. Initiate FTM with 'ftm -I -s <SSID>'                     |\n");
    printf(" |______________________________________________________________|\n");
    printf(" |                  Steps to test FTM Responder                 |\n");
    printf(" |                                                              |\n");
    printf(" |  1. Start SoftAP with command 'ap <SSID> <password>'         |\n");
    printf(" |  2. Optionally add '-c <ch>' to set channel, '-b 40' for 40M |\n");
    printf(" |  3. Use 'help' for detailed information for all parameters   |\n");
    printf(" ================================================================\n\n");

    printf("Console is ready on USB Serial/JTAG interface.\n");
    printf("You can use 'idf.py monitor' or 'screen /dev/cu.usbmodem1101 115200'\n\n");

    // start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
