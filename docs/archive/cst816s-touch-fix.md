# CST816S Touch Controller Fix

## Problem

You were experiencing continuous I2C errors:
```
E (24500) lcd_panel.io.i2c: panel_io_i2c_rx_buffer(145): i2c transaction failed
E (24500) CST816S: read_data(112): I2C read failed
```

## Root Cause

The CST816S touch controller has a **sleep mode behavior** that caused these errors:

1. **Touch-activated I2C**: The CST816S **only responds to I2C after a touch event**
2. **Auto-sleep**: After 2 seconds of inactivity, the chip enters standby mode
3. **Wake-up**: The chip only wakes up when physically touched

The original implementation was polling the touch controller every 10ms via LVGL, but the CST816S was in sleep mode and couldn't respond, causing I2C transaction failures.

## Solution

Changed from **polling mode** to **interrupt-driven mode**:

### Before (Polling - Causes Errors)
```c
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    // Always try to read, even when chip is asleep
    esp_lcd_touch_read_data(touch_handle);
    esp_lcd_touch_get_coordinates(...);
}
```

### After (Interrupt-Driven - No Errors)
```c
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    // Only read when interrupt pin is asserted (touch occurred)
    if (gpio_get_level(PIN_NUM_TOUCH_INT) == 0) {
        esp_lcd_touch_read_data(touch_handle);
        esp_lcd_touch_get_coordinates(...);
    }
}
```

**Key change**: Check GPIO5 (INT pin) before attempting I2C reads. The CST816S asserts this pin (active low) when a touch event occurs.

## Additional Improvements

### Graceful Degradation
If touch initialization fails, the system now:
- Disables touch input device registration
- Skips creating touch buttons on display
- Logs warnings instead of crashing
- Console commands still work normally

### Error Handling
```c
esp_err_t touch_err = esp_lcd_touch_new_i2c_cst816s(...);
if (touch_err != ESP_OK) {
    ESP_LOGW(TAG, "CST816S initialization failed, touch will be disabled");
    touch_handle = NULL;  // Disable touch
}
```

## Expected Behavior Now

### Normal Operation (Touch Working)
✅ **No I2C errors** - Touch is only read when interrupt fires
✅ **Touch buttons visible** - SCAN and FTM ALL buttons appear on display
✅ **Touch responsive** - Buttons respond when tapped
✅ **Low power** - Touch chip stays in sleep mode when idle

Console output on successful initialization:
```
I (123) ftm_display: Initializing I2C for touch controller
I (124) ftm_display: Initializing CST816S touch controller
I (125) ftm_display: CST816S touch controller initialized successfully
I (126) ftm_display: Touch input device registered
I (127) ftm_display: Touch buttons created
```

### Fallback Mode (Touch Not Available)
⚠️ **Touch disabled** - If CST816S can't be initialized
⚠️ **No touch buttons** - Display shows labels only
✅ **Console still works** - Use `scan` and `ftm -I` commands

Console output if touch fails:
```
W (123) ftm_display: Touch panel IO initialization failed (0x107), touch will be disabled
W (124) ftm_display: Touch input device not registered - touch unavailable
W (125) ftm_display: Touch disabled - buttons not created. Use console commands.
```

## Testing

After flashing the new firmware:

1. **Watch boot logs** - Should see "CST816S touch controller initialized successfully"
2. **Verify no I2C errors** - The continuous error spam should be gone
3. **Test touch** - Tap the SCAN button (bottom-left) or FTM ALL button (bottom-right)
4. **Check console** - Should see "Scan button pressed" or "FTM button pressed"

## Technical Details

### CST816S Interrupt Behavior
- **INT pin (GPIO5)**: Active-low interrupt
- **Trigger**: Asserted when touch detected
- **Duration**: Stays low until I2C read clears the interrupt
- **Sleep wake**: Touch wakes chip from sleep automatically

### Pin Configuration
| Pin | GPIO | Function |
|-----|------|----------|
| SDA | 6 | I2C Data |
| SCL | 7 | I2C Clock |
| RST | 13 | Touch Reset |
| INT | 5 | Touch Interrupt (active-low) |

### I2C Address
- **7-bit address**: 0x15
- **Write address**: 0x2A
- **Read address**: 0x2B

## References

The fix was based on research showing CST816S behavior:

- [CST816S Datasheet](https://files.waveshare.com/upload/5/51/CST816S_Datasheet_EN.pdf)
- [ESP-IDF CST816S Component](https://components.espressif.com/components/espressif/esp_lcd_touch_cst816s)
- [CST816S Sleep Mode Discussion](https://github.com/esphome/issues/issues/6293)
- [Zephyr CST816S Issues](https://github.com/zephyrproject-rtos/zephyr/discussions/74678)

## Troubleshooting

### If you still get I2C errors:
1. Check that you're running the latest firmware build
2. Verify GPIO5 is configured as input (INT pin)
3. Try touching the screen - errors should only occur if interrupt is stuck low
4. Check for hardware issues (loose connections, damaged touch controller)

### If touch doesn't work:
1. Verify "CST816S touch controller initialized successfully" in logs
2. Check that buttons are visible on display
3. Try tapping different areas of the screen
4. Monitor GPIO5 level - should pulse low on touch
5. Fallback to console commands: `scan` and `ftm -I`

## Summary

✅ **Fixed**: I2C errors eliminated by checking interrupt pin before reads
✅ **Improved**: Graceful degradation if touch not available
✅ **Optimized**: Touch chip stays in low-power sleep mode
✅ **Robust**: System works with or without functioning touch

The touch screen should now work reliably without flooding the console with I2C errors!
