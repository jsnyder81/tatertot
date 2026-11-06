/**
 * LVGL Configuration for ESP32-C6-Touch-AMOLED-1.32
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color settings */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

/* Memory settings */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (32U * 1024U)  // Reduced from 48KB to save space

/* Display settings */
#define LV_DPI_DEF 130

/* Compiler settings */
#define LV_ATTRIBUTE_TICK_INC

/* Feature usage */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

/* Input device settings */
#define LV_INDEV_DEF_READ_PERIOD 30

/* Logging */
#define LV_USE_LOG 1
#if LV_USE_LOG
  #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
  #define LV_LOG_PRINTF 1
#endif

/* Asserts */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* Built-in fonts - only enable fonts actually used to save ~24KB */
#define LV_FONT_MONTSERRAT_10  0  /* Unused */
#define LV_FONT_MONTSERRAT_12  0  /* Unused */
#define LV_FONT_MONTSERRAT_14  1  /* Used: hints, battery */
#define LV_FONT_MONTSERRAT_16  1  /* Used: info labels, settings */
#define LV_FONT_MONTSERRAT_18  0  /* Unused */
#define LV_FONT_MONTSERRAT_20  1  /* Used: default, many labels */
#define LV_FONT_MONTSERRAT_24  1  /* Used: buttons, titles */
#define LV_FONT_MONTSERRAT_28  1  /* Used: main title */
#define LV_FONT_MONTSERRAT_32  0  /* Unused */
#define LV_FONT_MONTSERRAT_36  0  /* Unused */
#define LV_FONT_MONTSERRAT_40  0  /* Unused */
#define LV_FONT_MONTSERRAT_48  1  /* Used: distance display */

/* Default font */
#define LV_FONT_DEFAULT &lv_font_montserrat_20

/* Widgets */
#define LV_USE_BTN      1
#define LV_USE_LABEL    1

/* Themes */
#define LV_USE_THEME_DEFAULT 1

/* Others */
#define LV_TICK_CUSTOM 1

#endif /*LV_CONF_H*/
