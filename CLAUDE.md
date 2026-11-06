# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C6 WiFi FTM (Fine Timing Measurement) distance measurement application with LVGL touch UI. Uses WiFi RTT to calculate distance between devices. Target hardware: Waveshare ESP32-C6-Touch-AMOLED-1.32 (466x466 display, SH8601 controller, CST820 touch).

## Build Commands

```bash
# Setup ESP-IDF environment (required before building)
source ~/esp/v5.5.1/esp-idf/export.sh

# Build
idf.py build

# Flash and monitor (adjust port as needed)
idf.py -p /dev/ttyACM0 flash monitor

# Configuration menu
idf.py menuconfig

# Clean build
idf.py fullclean
```

## Architecture

**Core Modules (main/):**

| Module | Purpose |
|--------|---------|
| `ftm_main.c` | WiFi/FTM control, CLI console commands, state machine, event handling |
| `ftm_display.c` | LVGL UI, multi-screen navigation (Home, Scanner, Settings, Responder, System Info), touch/gesture handling |
| `ftm_battery.c` | ADC-based battery monitoring with charging detection |

**Key Patterns:**
- FreeRTOS event groups for WiFi/FTM synchronization (`s_wifi_event_group`, `s_ftm_event_group`)
- LVGL mutex protection for all display operations
- Screen navigation via `ftm_screen_navigate_to()`, gesture detection (80px min swipe, 800ms max)
- Battery monitoring callback system for status change notifications

**Hardware Pin Configuration:**
- QSPI Display: CS=22, SCLK=18, SDIO0-3=19,20,10,11, Reset=21, Power=2
- Touch I2C: SDA=8, SCL=7, Address=0x15 (CST820)
- Battery ADC: GPIO0 (ADC1)

## Dependencies

- ESP-IDF v5.5.1 (target: esp32c6)
- LVGL ^8.3.0
- esp_lcd_sh8601 (managed component)
- Console components from ESP-IDF examples (cmd_system, cmd_nvs, cmd_wifi)

## FTM Console Commands

```bash
scan                    # Scan for FTM-capable APs
ftm -I                  # Auto-scan and measure all FTM APs
ftm -I -s <SSID>        # Measure distance to specific AP
ftm -I -c <8/16/24/32/64>  # Custom frame count (more = accurate)
ap <SSID> <password>    # Start as FTM Responder
sta <SSID> <password>   # Connect to AP
```

## Configuration

- `main/Kconfig.projbuild`: FTM report logging options
- `main/lv_conf.h`: LVGL configuration
- `sdkconfig.defaults`: Minimal project config (target esp32c6, 16MB flash, custom partitions, USB-JTAG console, FTM, LVGL fonts). Regenerate with `idf.py save-defconfig` after menuconfig changes.
- `partitions.csv`: Flash partition layout (NVS, PHY, 4MB factory app)
- `docs/`: design notes and troubleshooting; `docs/archive/` holds notes from the earlier ESP32-S3 prototype

## Custom Agents

Three specialized agents available in `.claude/agents/`:
- **esp32-architect**: ESP-IDF framework, crash debugging, peripheral configuration
- **embedded-c-lvgl-developer**: LVGL UI components, touch handling, display drivers
- **embedded-architect**: Memory allocation strategies, FreeRTOS task design, thread safety
