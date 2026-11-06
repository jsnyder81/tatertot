# Automatic FTM Scanning Feature

This document describes the automatic FTM (Fine Timing Measurement) scanning feature that allows the device to automatically detect and measure distance to all FTM-capable access points.

## Overview

The FTM command has been enhanced to automatically scan for and measure distance to **all FTM-capable APs** when invoked without specifying an SSID and when not connected to any AP.

## Usage

### Auto-Scan All FTM APs

Simply run the FTM command without any SSID when disconnected:

```bash
ftm> ftm -I
```

This will:
1. **Scan** for all available WiFi networks
2. **Identify** which APs support FTM/RTT
3. **Measure** distance to each FTM-capable AP sequentially
4. **Display** results on both console and LCD screen

### With Custom Parameters

You can also specify frame count and burst period:

```bash
ftm> ftm -I -c 64 -p 5
```

This scans all FTM APs using:
- 64 frames per measurement (more accurate)
- 500ms burst period (5 × 100ms)

## Command Modes

The `ftm -I` command now supports three modes:

### Mode 1: Auto-Scan (NEW)
**When**: Not connected to AP + No SSID specified
**Command**: `ftm -I`
**Behavior**: Scans and measures ALL FTM-capable APs

### Mode 2: Specific SSID
**When**: SSID specified
**Command**: `ftm -I -s AP_NAME`
**Behavior**: Measures only the specified AP

### Mode 3: Connected AP
**When**: Already connected to an AP
**Command**: `ftm -I`
**Behavior**: Measures the currently connected AP

## Example Output

### Console Output

```
ftm> ftm -I
I (12345) ftm_station: Auto-scanning for all FTM-capable APs...
I (12500) ftm_station: sta start to scan
I (14500) ftm_station: sta scan done
I (14501) ftm_station: Found 3 FTM-capable APs
I (14502) ftm_station: --- FTM Measurement 1/3 ---
I (14503) ftm_station: Measuring FTM to 'FTM_Office' (RSSI: -45, Channel: 1)
I (16234) ftm_station: [FTM_Office] RTT: 33 nSec, Distance: 5.07 m
I (17234) ftm_station: --- FTM Measurement 2/3 ---
I (17235) ftm_station: Measuring FTM to 'FTM_Lab' (RSSI: -62, Channel: 6)
I (18956) ftm_station: [FTM_Lab] RTT: 125 nSec, Distance: 18.75 m
I (19956) ftm_station: --- FTM Measurement 3/3 ---
I (19957) ftm_station: Measuring FTM to 'ESP32_AP' (RSSI: -78, Channel: 11)
E (21678) ftm_station: [ESP32_AP] FTM procedure failed!
I (21679) ftm_station: === FTM Complete: 2/3 successful ===
```

### Display Behavior

The LCD screen will show real-time updates:

1. **Scanning Phase**:
   - Status: "Scanning..." (blue)

2. **For Each AP**:
   - Status: "Measuring FTM..." (yellow)
   - AP Info: Shows current AP name, channel, RSSI

3. **Success**:
   - Status: "Success" (green)
   - Distance: Shows measured distance
   - RTT: Shows round-trip time

4. **Between Measurements**:
   - 1-second delay before next AP

5. **Complete**:
   - Status: Returns to "Idle" (green)

## Technical Details

### How It Works

1. **Scan Phase**:
   - Performs a WiFi scan for all available networks
   - Filters results to find APs with `ftm_responder` flag set

2. **Measurement Phase**:
   - Iterates through each FTM-capable AP
   - Updates display with current AP information
   - Performs FTM measurement
   - Logs results to console
   - Waits 1 second between measurements

3. **Completion**:
   - Reports total success count
   - Returns to idle state

### FTM Responder Detection

An AP is considered FTM-capable if:
- The `ftm_responder` field in `wifi_ap_record_t` is `true`
- This indicates the AP advertises FTM capability in its beacons

### Parameters

| Parameter | Flag | Default | Description |
|-----------|------|---------|-------------|
| Frame Count | `-c` | 32 | Number of FTM frames (0, 8, 16, 24, 32, 64) |
| Burst Period | `-p` | 2 | Period between bursts in 100ms units (0-100) |

## Use Cases

### 1. Site Survey
Quickly measure distances to all FTM-capable infrastructure:
```bash
ftm> ftm -I -c 64
```

### 2. Indoor Positioning
Periodically scan to determine device location based on multiple AP distances:
```bash
# Run every 5 seconds (implement in your own code)
ftm> ftm -I
```

### 3. Network Planning
Identify which APs support FTM and their signal strength:
```bash
ftm> scan
# Look for [FTM Responder] tags
ftm> ftm -I
# Measure all FTM APs
```

### 4. Troubleshooting
Test FTM with all available APs to find working ones:
```bash
ftm> ftm -I -c 64 -p 5
```

## Advantages

1. **Convenience**: No need to specify SSIDs manually
2. **Discovery**: Automatically finds all FTM-capable APs
3. **Batch Testing**: Measures multiple APs in one command
4. **Visual Feedback**: Real-time updates on LCD display
5. **Comprehensive**: Provides complete FTM infrastructure map

## Limitations

1. **Sequential**: Measures one AP at a time (1 second delay between)
2. **No Connection**: Requires being disconnected from AP
3. **Range**: Limited by WiFi scan range (~-80 dBm RSSI)
4. **Accuracy**: Depends on signal strength and environment

## Tips for Best Results

1. **Good Signal**: Works best with APs having RSSI > -70 dBm
2. **Line of Sight**: Clear path to APs improves accuracy
3. **Higher Frame Count**: Use `-c 64` for more accurate measurements
4. **Stable Environment**: Avoid moving objects during measurement
5. **Channel Selection**: Less interference on less crowded channels

## Error Handling

The system handles various error conditions:

- **No FTM APs Found**: Returns immediately with message
- **Scan Failure**: Aborts and returns to idle
- **FTM Timeout**: Logs error and continues to next AP
- **FTM Failure**: Logs error and continues to next AP

## Example Workflow

```bash
# 1. Disconnect from any AP
ftm> sta -d

# 2. Run auto-scan
ftm> ftm -I

# 3. Review results on display and console

# 4. Optional: Measure specific AP again with more frames
ftm> ftm -I -s FTM_Office -c 64
```

## Future Enhancements

Possible improvements:
- Sort APs by signal strength before measuring
- Filter by minimum RSSI threshold
- Export results to file/database
- Continuous monitoring mode
- Triangulation calculation from multiple APs
- Statistical analysis of multiple measurements
