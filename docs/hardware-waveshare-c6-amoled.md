# Waveshare ESP32-C6-Touch-AMOLED-1.32 Compatibility Summary

## ⚠️ Hardware Configuration Updated

The display implementation has been **updated** for the Waveshare ESP32-C6-Touch-AMOLED-1.32 development board.

## Pin Configuration

### ⚠️ IMPORTANT: Verify These Pin Assignments

**These pins are based on typical Waveshare ESP32-C6 AMOLED boards. You MUST verify against your schematic!**

#### QSPI Display Interface (CO5300)

| Function | GPIO Pin | Status |
|----------|----------|--------|
| LCD_CS   | GPIO 9   | ⚠️ Verify with schematic |
| LCD_SCLK | GPIO 10  | ⚠️ Verify with schematic |
| LCD_SDIO0| GPIO 11  | ⚠️ Verify with schematic |
| LCD_SDIO1| GPIO 12  | ⚠️ Verify with schematic |
| LCD_SDIO2| GPIO 13  | ⚠️ Verify with schematic |
| LCD_SDIO3| GPIO 14  | ⚠️ Verify with schematic |
| LCD_RST  | GPIO 2   | ⚠️ Verify with schematic |
| LCD_EN   | GPIO 3   | ⚠️ Verify with schematic |

#### I2C Touch Interface (CST820)

| Function | GPIO Pin | Status |
|----------|----------|--------|
| TOUCH_SDA| GPIO 6   | ⚠️ Verify with schematic |
| TOUCH_SCL| GPIO 7   | ⚠️ Verify with schematic |
| TOUCH_RST| GPIO 1   | ⚠️ Verify with schematic |
| TOUCH_INT| GPIO 0   | ⚠️ Verify with schematic |

### Hardware Specifications

| Setting | Value | Notes |
|---------|-------|-------|
| QSPI Clock | 80 MHz | ✅ Optimized for QSPI interface |
| Display Resolution | 466×466 | ✅ Updated for 1.32" AMOLED |
| Display Driver | CO5300 | ⚠️ Driver needs implementation |
| Touch Controller | CST820 | ⚠️ Driver needs implementation |
| Color Depth | 16-bit (16.7M colors) | ✅ AMOLED capability |
| Interface | QSPI (Quad SPI) | ✅ 4-wire high-speed mode |
| DMA Enabled | Yes | ✅ For fast transfers |
| Dual Buffering | Yes | ✅ For smooth rendering |

## Hardware Match Summary

### Display Specifications
- **Board**: Waveshare ESP32-C6-Touch-AMOLED-1.32
- **Display**: 1.32" Round AMOLED, 466×466 pixels, 16.7M colors
- **Driver Chip**: CO5300 ⚠️ (needs driver implementation)
- **Touch Chip**: CST820 ⚠️ (needs driver implementation)
- **Interface**: QSPI (Quad SPI) ⚠️ (configured but needs testing)
- **Max QSPI Speed**: 80 MHz ✅ (configured)
- **MCU**: ESP32-C6 (RISC-V, 160MHz) ✅

### What's Configured
⚠️ QSPI pins mapped (NEEDS SCHEMATIC VERIFICATION!)
⚠️ QSPI bus initialized (needs CO5300 driver)
⚠️ Touch I2C pins mapped (NEEDS SCHEMATIC VERIFICATION!)
⚠️ Touch hardware initialized (needs CST820 driver)
✅ Display resolution updated to 466×466
✅ LVGL graphics library integration
✅ DMA channel auto-allocation

### What's Available on Board
- Audio codec: ES8311 (I2C, needs configuration)
- Digital microphone (for voice input)
- MX1.25 battery connector (3.7V lithium battery support)
- MX1.25 speaker connector (audio output)
- SH1.0 12-pin expansion header
- PWR and BOOT programmable buttons
- WiFi 6, Bluetooth 5, Zigbee 3.0/Thread support

## Files Updated for ESP32-C6-Touch-AMOLED-1.32

1. **[main/ftm_display.h](main/ftm_display.h)**
   - ⚠️ Pin definitions updated (VERIFY WITH SCHEMATIC!)
   - Display resolution changed from 240×240 to 466×466
   - Added QSPI pin definitions (CS, SCLK, SDIO0-3, RST, EN)
   - Updated touch pin definitions for CST820
   - 80MHz QSPI clock configured

2. **[main/ftm_display.c](main/ftm_display.c)**
   - QSPI bus initialization (SPI2_HOST with quad mode)
   - ⚠️ CO5300 driver placeholder (needs implementation)
   - ⚠️ CST820 touch controller placeholder (needs implementation)
   - Display enable/reset GPIO handling
   - Touch reset and interrupt GPIO configuration
   - DMA enabled for fast transfers
   - LVGL rendering configured for 466×466

3. **Component Dependencies (needs update)**
   - ⚠️ Need CO5300 QSPI display driver component
   - ⚠️ Need CST820 touch controller driver component
   - LVGL v8.3.0 graphics library (existing)

## ⚠️ CRITICAL: Before Building or Flashing

### Step 1: Verify GPIO Pins from Schematic

**YOU MUST verify the following pins against the schematic at:**
https://files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32/ESP32-C6-Touch-AMOLED-1.32-schematic.pdf

1. Open the schematic PDF
2. Find the CO5300 AMOLED controller section
3. Verify each QSPI pin connection:
   - LCD_CS (currently set to GPIO9)
   - LCD_SCLK (currently set to GPIO10)
   - LCD_SDIO0/1/2/3 (currently set to GPIO11/12/13/14)
   - LCD_RST (currently set to GPIO2)
   - LCD_EN (currently set to GPIO3)

4. Find the CST820 touch controller section
5. Verify each I2C pin connection:
   - TOUCH_SDA (currently set to GPIO6)
   - TOUCH_SCL (currently set to GPIO7)
   - TOUCH_RST (currently set to GPIO1)
   - TOUCH_INT (currently set to GPIO0)

6. Update [main/ftm_display.h](main/ftm_display.h) with the CORRECT pins

### Step 2: Obtain Required Drivers

Before this code will work, you need:

- [ ] ⚠️ **CO5300 QSPI display driver** - Check Waveshare GitHub or ESP Component Registry
- [ ] ⚠️ **CST820 touch controller driver** - Check Waveshare GitHub or ESP Component Registry
- [ ] Update `main/idf_component.yml` with the correct components

### Step 3: Testing Checklist

- [ ] GPIO pins verified against schematic
- [ ] CO5300 driver component added
- [ ] CST820 driver component added
- [ ] LVGL graphics library v8.3.0 included
- [ ] Display initialization updated with proper CO5300 init sequence
- [ ] Touch initialization updated with proper CST820 init sequence
- [ ] Code compiles without errors for ESP32-C6 target

## Expected Behavior After Proper Configuration

1. **Power On**: Display enable pin activates (LCD_EN)
2. **Reset**: Display resets properly (LCD_RST)
3. **Initialization**: QSPI communication established with CO5300
4. **Display Output**: Screen shows "WiFi FTM" title at 466×466 resolution
5. **Touch Input**: CST820 responds to touch events via I2C
6. **Idle State**: Shows "Ready" in green, "Mode: Idle"
7. **FTM Operation**: Display updates during WiFi FTM measurements

## Build and Flash Commands

⚠️ **Only use these after completing ALL steps above!**

```bash
# Set target to ESP32-C6
idf.py set-target esp32c6

# Build the project
idf.py build

# Flash to Waveshare ESP32-C6-Touch-AMOLED-1.32 board
# On Linux: /dev/ttyACM0 or /dev/ttyUSB0
# On macOS: /dev/cu.usbmodem* or /dev/cu.usbserial*
# On Windows: COM3, COM4, etc.
idf.py -p /dev/ttyACM0 flash monitor
```

## Troubleshooting

If display doesn't work:

1. **Check Target**: Ensure you ran `idf.py set-target esp32c6`
2. **Check GPIO Pins**: Double-check all pins against the schematic
3. **Check USB Connection**: Ensure board is properly connected
4. **Check Port**: Verify correct port with `ls /dev/tty*` (Linux/macOS)
5. **Check Drivers**: Ensure CO5300 and CST820 drivers are properly integrated
6. **Monitor Output**: Look for initialization messages:
   ```
   I (xxx) ftm_display: Initializing GC9A01 display
   I (xxx) ftm_display: Display hardware initialized (CO5300 QSPI mode)
   W (xxx) ftm_display: CO5300 driver needs proper initialization - placeholder code active
   ```
7. **Check Heap Memory**: Ensure sufficient free heap (display buffers need ~176KB for 466×466)
8. **ESP32-C6 GPIO Restrictions**:
   - GPIO8, GPIO9 are strapping pins (use with caution)
   - GPIO24-30 are used for SPI flash (avoid using)
   - GPIO12, GPIO13 are used by USB-JTAG (may conflict)

## References

- [Waveshare ESP32-C6-Touch-AMOLED-1.32 Wiki](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32)
- [Waveshare Product Page](https://www.waveshare.com/esp32-c6-touch-amoled-1.32.htm)
- [Schematic PDF](https://files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32/ESP32-C6-Touch-AMOLED-1.32-schematic.pdf)
- [ESP32-C6 Technical Reference](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)
- [ESP-IDF LCD Documentation (ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/lcd.html)
- [ESP-IDF SPI Master Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/spi_master.html)
- [LVGL Documentation](https://docs.lvgl.io/)
