#!/bin/bash
# Troubleshooting script for ESP32-C6 build/flash issues

echo "=== ESP32-C6 Troubleshooting Script ==="
echo ""

# Check if ESP-IDF is sourced
echo "1. Checking ESP-IDF environment..."
if command -v idf.py &> /dev/null; then
    echo "   ✓ idf.py found"
    idf.py --version
else
    echo "   ✗ idf.py not found - you need to source ESP-IDF:"
    echo "     . \$HOME/esp/esp-idf/export.sh"
    exit 1
fi
echo ""

# Check Python environment
echo "2. Checking Python environment..."
python --version 2>/dev/null || python3 --version
echo ""

# Check connected devices
echo "3. Checking connected USB devices..."
ls -la /dev/cu.* 2>/dev/null | grep -i usb
ls -la /dev/tty.* 2>/dev/null | grep -i usb
echo ""

# Check current target
echo "4. Checking current ESP-IDF target..."
grep "CONFIG_IDF_TARGET=" sdkconfig | head -1
echo ""

# Check console configuration
echo "5. Checking console configuration..."
echo "   Primary console:"
if grep -q "^CONFIG_ESP_CONSOLE_UART_DEFAULT=y" sdkconfig; then
    echo "   - UART (may cause input issues on ESP32-C6)"
elif grep -q "^CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y" sdkconfig; then
    echo "   - USB Serial/JTAG ✓ (correct for ESP32-C6)"
else
    echo "   - Unknown/None"
fi
echo ""

# Provide recommendations
echo "6. Recommended steps:"
echo "   a) Clean and reconfigure:"
echo "      idf.py fullclean"
echo "      idf.py set-target esp32c6"
echo "      idf.py reconfigure"
echo ""
echo "   b) Build:"
echo "      idf.py build"
echo ""
echo "   c) Flash (use your actual port):"
echo "      idf.py -p /dev/cu.usbmodem1101 flash monitor"
echo ""
echo "   d) If flash fails, try putting board in download mode:"
echo "      - Hold BOOT button"
echo "      - Press and release RESET button"
echo "      - Release BOOT button"
echo "      - Try flash command again"
echo ""

echo "=== End of Troubleshooting ==="
