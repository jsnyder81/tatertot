# FTM Troubleshooting Guide

This guide helps diagnose and resolve issues with FTM (Fine Timing Measurement) functionality.

## Issue: No FTM Responders Detected

### Symptoms
- `scan` command shows APs but none marked as `[FTM Responder]`
- `ftm -I` reports "No FTM-capable APs found"
- Display shows "Found 0 FTM APs"

### Enhanced Diagnostic Output

The scan command now shows detailed information:

```
ftm> scan
I (123) ftm_station: Starting active scan...
I (456) ftm_station: === Scan Results: 5 APs found ===
I (457) ftm_station: [MyAP] RSSI:-45dBm Ch:1 11n ftm_resp=0
I (458) ftm_station: [Office] RSSI:-52dBm Ch:6 11n ftm_resp=0
I (459) ftm_station: [TestAP] RSSI:-38dBm Ch:11 11n [FTM RESPONDER] ***
I (460) ftm_station: === FTM-capable APs: 1/5 ===
```

Key information:
- **RSSI**: Signal strength in dBm
- **Ch**: WiFi channel
- **11n/11g/11b**: PHY mode
- **ftm_resp=0/1**: Raw FTM responder flag value
- **[FTM RESPONDER]**: Confirmed FTM capability

### Diagnostic Checklist

#### 1. Verify AP FTM Support

**Check your AP configuration:**
- Enable "Fine Timing Measurement" or "RTT" in AP settings
- Enable "802.11mc" support (if available)
- Ensure AP is using 802.11n or newer (not 802.11b/g only)

**Compatible AP types:**
- ESP32 devices running FTM responder mode
- Modern routers with 802.11mc support
- Enterprise APs with FTM/RTT enabled

#### 2. Check ESP32 WiFi Configuration

Run in console to verify FTM is enabled:

```bash
ftm> query
```

Should show WiFi mode is active (not NULL).

#### 3. Verify Scan Type

The code now uses **active scanning** by default which is better for FTM detection:

```c
scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
scan_config.show_hidden = true;
```

#### 4. Signal Strength Requirements

FTM works best with:
- **RSSI > -70 dBm**: Good signal
- **RSSI -70 to -80 dBm**: Marginal
- **RSSI < -80 dBm**: Usually fails

Check scan results for RSSI values.

#### 5. Channel and Band

ESP32-S3 supports:
- **2.4 GHz**: Channels 1-14
- **5 GHz**: Not supported on ESP32-S3 (only on ESP32-S2/S3 with specific config)

Ensure your FTM responder is on a 2.4 GHz channel.

### Testing with ESP32 FTM Responder

The most reliable way to test is with another ESP32 device:

**Device 1 (Responder):**
```bash
ftm> ap FTM_TEST password123
```

Display should show: "AP Mode" (purple)

**Device 2 (Initiator):**
```bash
ftm> scan
# Should see: [FTM_TEST] ... [FTM RESPONDER] ***
ftm> ftm -I -s FTM_TEST
```

### Common Issues and Solutions

#### Issue: AP advertises FTM but not detected

**Possible causes:**
1. **Beacon interval timing**: Try scanning multiple times
2. **Channel congestion**: Move AP to less crowded channel
3. **Hidden SSID**: Code now shows hidden SSIDs

**Solutions:**
```bash
# Scan multiple times
ftm> scan
ftm> scan
ftm> scan
```

#### Issue: FTM measurement fails even when AP detected

**Error messages:**
```
E (xxx) ftm_station: Failed to start FTM session with AP_NAME
E (xxx) ftm_station: [AP_NAME] FTM procedure failed!
E (xxx) ftm_station: [AP_NAME] FTM procedure timed out!
```

**Common causes:**
1. **Weak signal** (RSSI < -70 dBm)
2. **Obstacles** between devices
3. **AP not properly configured**
4. **Incompatible AP firmware**

**Solutions:**
- Move closer to AP
- Ensure line of sight
- Use higher frame count: `ftm -I -s AP_NAME -c 64`
- Check AP FTM configuration

#### Issue: Display stuck on "Scanning..."

**Causes:**
- Scan failed
- WiFi not initialized properly

**Solutions:**
1. Check console for error messages
2. Restart device
3. Verify WiFi is in STA or APSTA mode:
   ```bash
   ftm> query
   ```

### Display Status Indicators

The LCD display shows real-time status:

| Display Text | Color | Meaning |
|-------------|-------|---------|
| "Ready" | Green | Idle, ready for commands |
| "Scanning..." | Blue | WiFi scan in progress |
| "Measuring FTM..." | Yellow | FTM measurement in progress |
| "Success" | Green | Measurement succeeded |
| "Failed" | Red | Measurement failed |
| "AP Mode" | Purple | Acting as FTM Responder |

**Mode line shows:**
- "Idle" - No active operation
- "Station" - Connected to AP
- "SoftAP" - Running as access point
- "Found X FTM APs" - After scan completes

### Enabling Debug Logs

For detailed debugging, you can modify log levels:

In `ftm_main.c`, the logging is set to:
```c
esp_log_level_set("wifi", ESP_LOG_WARN);
```

For more verbose output, change to:
```c
esp_log_level_set("wifi", ESP_LOG_DEBUG);
```

This will show detailed WiFi scan information.

### FTM Requirements Summary

**For FTM to work, you need:**

✅ **Access Point:**
- 802.11mc / FTM / RTT support enabled
- Operating on 2.4 GHz (channels 1-14)
- Signal strength > -70 dBm (closer is better)
- Line of sight to initiator

✅ **ESP32 Device:**
- FTM initiator support enabled (default in this project)
- In STA or APSTA mode
- Active WiFi scan capability

✅ **Environment:**
- Minimal obstacles between devices
- Low RF interference
- Stable conditions (no moving objects during measurement)

### Testing Procedure

**Step 1: Verify Basic WiFi**
```bash
ftm> scan
# Should see some APs
```

**Step 2: Check for FTM APs**
Look for `[FTM RESPONDER]` tags in scan results. Console shows `ftm_resp=1` for FTM-capable APs.

**Step 3: Test with Known FTM AP**
If you have another ESP32:
```bash
# On Device 1
ftm> ap FTM_TEST password123

# On Device 2
ftm> scan
ftm> ftm -I -s FTM_TEST
```

**Step 4: Check Display**
- Should show "Scanning..." during scan (blue)
- Should show "Measuring FTM..." during measurement (yellow)
- Should show distance if successful (green)

### Known Limitations

1. **ESP32 FTM is experimental** - Not all features may work perfectly
2. **Limited AP support** - Many consumer APs don't support 802.11mc
3. **2.4 GHz only** - 5 GHz not supported on most ESP32 variants
4. **Accuracy varies** - Typically ±0.5-2 meters under good conditions
5. **Environment sensitive** - Works best with line of sight

### Getting Help

If FTM still doesn't work:

1. **Capture full console output** during scan and FTM attempt
2. **Note your AP model** and firmware version
3. **Verify AP FTM configuration** (screenshots if possible)
4. **Check signal strength** (RSSI values)
5. **Test with ESP32 FTM responder** to isolate AP issues

### Example: Working FTM Session

```bash
ftm> scan
I (1234) ftm_station: Starting active scan...
I (2345) ftm_station: === Scan Results: 3 APs found ===
I (2346) ftm_station: [Office] RSSI:-55dBm Ch:1 11n ftm_resp=0
I (2347) ftm_station: [FTM_TEST] RSSI:-42dBm Ch:6 11n [FTM RESPONDER] ***
I (2348) ftm_station: [Guest] RSSI:-68dBm Ch:11 11g ftm_resp=0
I (2349) ftm_station: === FTM-capable APs: 1/3 ===

ftm> ftm -I -s FTM_TEST
I (3456) ftm_station: Measuring FTM to 'FTM_TEST' (RSSI: -42, Channel: 6)
I (5678) ftm_station: [FTM_TEST] RTT: 33 nSec, Distance: 5.07 m
```

Display shows:
- "Found 1 FTM APs" in mode line
- "Measuring FTM..." during measurement
- "Distance: 5.07 m" when complete
- "RTT: 33 ns"
- AP info showing FTM_TEST details
