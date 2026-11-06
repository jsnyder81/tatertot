# Touch Screen Guide

This document describes the touch screen functionality added to the FTM project for the Waveshare ESP32-S3-Touch-LCD-1.28 board.

## Overview

The touch screen integration allows you to manually trigger WiFi scans and FTM measurements by tapping buttons on the display, eliminating the need to use the console for basic operations.

## Hardware

- **Touch Controller**: CST816S (I2C capacitive touch controller)
- **Touch Pin Configuration**:
  - SDA: GPIO 6
  - SCL: GPIO 7
  - RST: GPIO 13
  - INT: GPIO 5

## Display Layout

The display now includes two interactive buttons at the bottom:

```
┌─────────────────┐
│   WiFi FTM      │  ← Title
│   Mode: Idle    │  ← Mode status
│                 │
│     Ready       │  ← Status (colored)
│                 │
│ Distance: -- m  │  ← Distance reading
│   RTT: -- ns    │  ← Round-trip time
│                 │
│ AP: ...         │  ← AP information
│                 │
│ [SCAN] [FTM ALL]│  ← Touch buttons
└─────────────────┘
```

## Touch Buttons

### SCAN Button (Blue, Left)
- **Location**: Bottom-left corner (100×40 pixels)
- **Function**: Triggers a WiFi scan to discover all available access points
- **Action**: Same as console command `scan`
- **What it does**:
  1. Scans for all WiFi networks
  2. Lists them in the console
  3. Identifies which APs support FTM/RTT
  4. Updates display status to "Scanning..." (blue) during scan
  5. Returns to "Ready" (green) when complete

### FTM ALL Button (Orange, Right)
- **Location**: Bottom-right corner (100×40 pixels)
- **Function**: Automatically measures distance to ALL FTM-capable access points
- **Action**: Same as console command `ftm -I`
- **What it does**:
  1. Scans for all WiFi networks
  2. Filters for FTM-capable APs
  3. Measures distance to each one sequentially
  4. Updates display with each measurement
  5. Shows "Measuring FTM..." (yellow) during measurement
  6. Shows "Success" (green) after each successful measurement

## Usage Examples

### Example 1: Quick Scan
1. **Tap the SCAN button** (blue, left)
2. Watch display status change to "Scanning..." (blue)
3. View results in console
4. Status returns to "Ready" (green)

### Example 2: Measure All FTM Access Points
1. Make sure you're not connected to any AP (disconnect if needed)
2. **Tap the FTM ALL button** (orange, right)
3. Watch the display as it:
   - Scans for APs (blue)
   - Measures each FTM AP (yellow)
   - Shows distance for each (green)
4. Check console for detailed results

### Example 3: Monitor Multiple APs Over Time
1. Tap FTM ALL button
2. Wait for all measurements to complete
3. Move your device
4. Tap FTM ALL again to see updated distances
5. Compare results in console

## Technical Details

### Touch Controller Initialization
- I2C bus configured at 400 kHz
- Uses new ESP-IDF I2C master driver API
- Internal pullups enabled on I2C lines
- Touch coordinates: 0-239 (x and y)

### LVGL Integration
- Touch input registered as `LV_INDEV_TYPE_POINTER`
- Coordinates read every 10ms by LVGL task
- Button events use `LV_EVENT_CLICKED`
- Thread-safe updates via semaphore

### Callbacks
The touch buttons trigger these internal functions:
- **SCAN button**: `touch_scan_callback()` → `wifi_perform_scan(NULL, false)`
- **FTM ALL button**: `touch_ftm_all_callback()` → `scan_and_measure_all_ftm_aps(32, 2)`

Default FTM parameters when using touch button:
- Frame count: 32 frames
- Burst period: 200ms (2 × 100ms units)

## Display Status Colors

| Color | Status | Meaning |
|-------|--------|---------|
| 🟢 Green | "Ready" / "Success" | Idle or operation successful |
| 🔵 Blue | "Scanning..." | WiFi scan in progress |
| 🟡 Yellow | "Measuring FTM..." | FTM measurement in progress |
| 🟢 Green | "Success" | Measurement completed |
| 🔴 Red | "Failed" | Operation failed |

## Troubleshooting

### Touch not responding
1. Check I2C connections (GPIO 6/7 for SDA/SCL)
2. Verify CST816S controller is detected at boot
3. Look for "Initializing CST816S touch controller" in console
4. Check for I2C errors in console output

### Buttons not visible
1. Verify display initialized successfully
2. Check LVGL task is running
3. Look for "GC9A01 display initialized successfully" message
4. Verify buttons created (check debug logs)

### Callbacks not firing
1. Check console for "Touch SCAN triggered" or "Touch FTM ALL triggered"
2. Verify callbacks registered during initialization
3. Look for "Scan callback registered" and "FTM callback registered"

### Touch coordinates wrong
1. Display might need rotation or mirroring adjustment
2. Check touch config `swap_xy`, `mirror_x`, `mirror_y` flags
3. Test by watching console logs for touch coordinates

## Console Output

When you press a touch button, you'll see console logs:

**SCAN Button:**
```
I (12345) ftm_display: Scan button pressed
I (12346) ftm_station: Touch SCAN triggered
I (12347) ftm_station: sta start to scan
...
```

**FTM ALL Button:**
```
I (23456) ftm_display: FTM button pressed
I (23457) ftm_station: Touch FTM ALL triggered
I (23458) ftm_station: Auto-scanning for all FTM-capable APs...
...
```

## Console Commands Still Available

All console commands still work as before:
- `scan` - Manual WiFi scan
- `ftm -I` - Measure all FTM APs
- `ftm -I -s SSID` - Measure specific AP
- `sta SSID password` - Connect to AP
- `ap SSID password` - Start SoftAP mode

The touch buttons are just a convenient alternative!

## Source Files

Touch functionality is implemented in:
- **[ftm_display.h](main/ftm_display.h)** - Touch pin definitions and callback API
- **[ftm_display.c](main/ftm_display.c)** - Touch controller initialization and button handling
- **[ftm_main.c](main/ftm_main.c)** - Touch callback implementations (lines 920-932, 950-951)

## Dependencies

Touch support requires this component (already added to [idf_component.yml](main/idf_component.yml)):
```yaml
espressif/esp_lcd_touch_cst816s:
  version: "^1.0.0"
```

## Future Enhancements

Potential improvements:
- Add settings button to adjust FTM parameters (frame count, burst period)
- Add gesture support (swipe to navigate between APs)
- Long-press for advanced options
- Touch-and-hold to cancel operations
- Virtual keyboard for entering AP credentials
- Touch calibration routine
- Multi-touch support for zoom/pan of results
