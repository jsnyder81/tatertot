idf_component_register(SRCS "ftm_main.c" "ftm_display.c" "ftm_battery.c"
                       INCLUDE_DIRS "."
                       REQUIRES lvgl esp_lcd driver esp_adc)
