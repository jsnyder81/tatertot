# Quick Start Guide - FTM with Display

This guide shows you how to use the FTM distance measurement with the Waveshare ESP32-S3-Touch-LCD-1.28 display.

## Hardware Required

- Waveshare ESP32-S3-Touch-LCD-1.28 development board
- USB-C cable for power and programming
- Two boards for testing (one as initiator, one as responder)
  OR one board + external FTM-capable AP

## First Time Setup

### 1. Build and Flash

```bash
cd /path/to/ftm
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### 2. Expected Display on Boot

```
┌─────────────────┐
│   WiFi FTM      │  <- Title (white)
│   Mode: Idle    │  <- Mode (gray)
│                 │
│     Ready       │  <- Status (green)
│                 │
│ Distance: -- m  │  <- Distance (white)
│   RTT: -- ns    │  <- RTT (gray)
│                 │
│                 │  <- AP info (blue)
└─────────────────┘
```

## Usage Examples

### Example 1: Scan for FTM Access Points

**Console Command:**
```
ftm> scan
```

**Display Changes:**
1. Status → "Scanning..." (blue)
2. After scan completes → "Ready" (green)

**Console Output:**
```
I (123) ftm_station: sta start to scan
I (456) ftm_station: [MyAP][rssi=-45][FTM Responder]
I (456) ftm_station: sta scan done
```

---

### Example 2: Setup as FTM Responder (SoftAP)

**Console Command:**
```
ftm> ap FTM_AP password
```

**Display Changes:**
1. Status → "AP Mode" (purple)
2. Mode → "SoftAP"

**What it means:** Your board is now acting as an access point that other devices can measure distance to.

---

### Example 3: Auto-Scan and Measure ALL FTM Access Points (NEW!)

**Console Command:**
```
ftm> ftm -I
```

**What it does:** Automatically scans for ALL FTM-capable APs and measures distance to each one!

**Display Changes (in sequence):**

1. **Scanning Phase**
   ```
   Status: "Scanning..." (blue)
   ```

2. **For Each FTM AP Found** (repeats for all APs)
   ```
   Status: "Measuring FTM..." (yellow)
   AP Info: Shows current AP being measured
   Distance: Updates with measured distance
   RTT: Updates with round-trip time
   ```

3. **Complete**
   ```
   Status: "Idle" (green)
   ```

**Console Output Example:**
```
ftm> ftm -I
I (123) ftm_station: Auto-scanning for all FTM-capable APs...
I (456) ftm_station: Found 3 FTM-capable APs
I (789) ftm_station: --- FTM Measurement 1/3 ---
I (790) ftm_station: [FTM_Office] RTT: 33 nSec, Distance: 5.07 m
I (1500) ftm_station: --- FTM Measurement 2/3 ---
I (1501) ftm_station: [FTM_Lab] RTT: 125 nSec, Distance: 18.75 m
I (2200) ftm_station: --- FTM Measurement 3/3 ---
I (2201) ftm_station: [ESP32_AP] RTT: 50 nSec, Distance: 7.50 m
I (2202) ftm_station: === FTM Complete: 3/3 successful ===
```

---

### Example 4: Measure Distance to Specific Access Point

**Console Command:**
```
ftm> ftm -I -s FTM_AP
```

**Display Changes (in sequence):**

1. **Measuring Phase** (2-5 seconds)
   ```
   Status: "Measuring FTM..." (yellow)
   ```

2. **Success**
   ```
   Status: "Success" (green)
   Distance: 5.07 m
   RTT: 33 ns
   AP Info: FTM_AP | Ch:1 | RSSI:-45dBm
   ```

**Console Output:**
```
I (789) ftm_station: Requesting FTM session with Frm Count - 32, Burst Period - 200mSec
I (890) ftm_station: Estimated RTT - 33 nSec, Estimated Distance - 5.07 meters
```

---

### Example 5: Connect to AP then Measure

**Console Commands:**
```
ftm> sta FTM_AP password
ftm> ftm -I
```

**Display Changes:**

1. **Connecting Phase**
   ```
   Status: "Connecting..." (yellow)
   ```

2. **Connected**
   ```
   Status: "Success" (green)
   Mode: "Station"
   AP Info: FTM_AP | Ch:1 | RSSI:-45dBm
   ```

3. **Measuring FTM**
   ```
   Status: "Measuring FTM..." (yellow)
   ```

4. **Measurement Complete**
   ```
   Status: "Success" (green)
   Distance: 5.07 m
   RTT: 33 ns
   ```

---

## Display Status Colors

| Color | Status | Meaning |
|-------|--------|---------|
| 🟢 Green | "Ready" / "Success" | System idle or operation successful |
| 🔵 Blue | "Scanning..." | Searching for WiFi networks |
| 🟡 Yellow | "Connecting..." / "Measuring FTM..." | Operation in progress |
| 🔴 Red | "Failed" | Operation failed |
| 🟣 Purple | "AP Mode" | Acting as FTM Responder |

## Two-Device Testing Procedure

### Device 1 (Responder)
```bash
ftm> ap FTM_TEST password123
```
Display shows: **"AP Mode"** (purple)

### Device 2 (Initiator)
```bash
ftm> scan
# Look for [FTM_TEST][FTM Responder] in results
ftm> ftm -I -s FTM_TEST
```
Display shows distance measurement!

### Move the devices apart and re-measure
```bash
ftm> ftm -I -s FTM_TEST
```
Display updates with new distance.

## Advanced Commands

### Scan for specific SSID
```bash
ftm> scan MyAP
```

### FTM with custom frame count (more frames = more accurate)
```bash
ftm> ftm -I -s FTM_AP -c 64
```
Options: 0, 8, 16, 24, 32, 64 frames

### FTM with custom burst period
```bash
ftm> ftm -I -s FTM_AP -p 5
```
Period in 100ms units (5 = 500ms between bursts)

### Disconnect from AP
```bash
ftm> sta -d
```

### Check current WiFi status
```bash
ftm> query
```

## Understanding the Results

### Distance Accuracy
- **Good conditions** (< -70dBm RSSI, line of sight): ±0.5-1 meter
- **Poor conditions** (> -70dBm, obstacles): ±2-5 meters
- **Best results**: Keep devices in line of sight, no moving objects

### RTT (Round Trip Time)
- Speed of light: ~300,000 km/s
- Formula: Distance = RTT × c / 2
- Example: 33 ns RTT ≈ 5 meters distance

### RSSI (Signal Strength)
- **Excellent**: > -50 dBm
- **Good**: -50 to -70 dBm
- **Fair**: -70 to -80 dBm
- **Poor**: < -80 dBm (avoid for FTM)

## Troubleshooting

### Display shows "Failed" (red)
1. Check that target AP supports FTM Responder
2. Verify signal strength (scan first, check RSSI)
3. Ensure no obstacles between devices
4. Try reducing distance

### Display blank or not updating
1. Check backlight (should be on)
2. Look for initialization errors in console
3. Verify SPI connections (should auto-work on Waveshare board)
4. Check available heap memory

### Distance reading seems wrong
1. Verify line of sight
2. Reduce obstacles and interference
3. Use higher frame count: `ftm -I -s AP -c 64`
4. Check RSSI is better than -70dBm

### Can't find FTM Responder in scan
1. Ensure responder device has `ap` command running
2. Check both devices are on same channel (use `-c` option)
3. Verify responder has FTM enabled (default on ESP devices)

## Tips for Best Results

1. **Line of Sight**: Keep clear path between devices
2. **Good RSSI**: Keep devices within reasonable range (< -70dBm)
3. **Stable Environment**: Avoid moving objects during measurement
4. **Higher Frame Count**: Use `-c 64` for more accurate results
5. **20MHz Bandwidth**: Use `-b 20` on AP for better accuracy
6. **Calibration**: Note zero-distance reading and calibrate with `-o` command

## Console Reference

| Command | Description |
|---------|-------------|
| `scan` | Scan for WiFi networks |
| `scan SSID` | Scan for specific SSID |
| `sta SSID password` | Connect to AP |
| `sta -d` | Disconnect from AP |
| `ap SSID password` | Start SoftAP |
| `ap SSID password -c CH` | Start SoftAP on channel CH |
| `ftm -I` | Measure FTM (must be connected) |
| `ftm -I -s SSID` | Measure FTM to SSID |
| `ftm -I -s SSID -c FRAMES` | Custom frame count |
| `ftm -I -s SSID -p PERIOD` | Custom burst period |
| `ftm -R -o CM` | Set responder offset (cm) |
| `query` | Check WiFi status |
| `help` | Show all commands |

## Next Steps

- Test at various distances (1m, 5m, 10m, 20m)
- Experiment with different environments
- Try outdoor vs indoor measurements
- Add more visualization to the display
- Integrate touch screen for interactive control
- Log measurements over time
