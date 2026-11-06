# TatorTot: Wi-Fi FTM Distance Meter for ESP32-C6

A handheld Wi-Fi **Fine Timing Measurement (FTM / 802.11mc RTT)** tool built on the
Waveshare **ESP32-C6-Touch-AMOLED-1.32** board. It scans for FTM-capable access
points, measures distance using round-trip time, and shows the results on a
466×466 round AMOLED touch display. It can also act as an FTM **responder** for
another device to measure against.

The project started from the ESP-IDF `wifi/ftm` example and adds a full LVGL
touch UI, continuous measurement mode, and battery monitoring.

## Features

- **FTM initiator**: measures RTT and distance to any FTM-responder AP, either a
  specific SSID or every responder found in a scan.
- **FTM responder**: starts a SoftAP (`TatorTot_XXXX`) that other devices can range against.
- **Continuous mode**: long-press the FTM button to repeat measurements to a target.
- **Multi-screen touch UI** (LVGL 8, swipe to navigate):
  - Home: live distance, RTT, and target AP
  - AP Scanner: browse scan results and FTM capability
  - Settings: FTM frame count and burst period
  - Responder: SoftAP responder mode
  - System Info: battery and system status
- **Battery monitoring**: ADC-based voltage readout with charge-state detection.
- **Serial console** (USB-Serial/JTAG) with commands for scripting and debugging.

## How FTM works

The initiator and responder exchange a burst of FTM action frames. Both sides
record hardware timestamps (T1–T4) for each frame and ACK, and the initiator works out:

```
RTT      = (T4 - T1) - (T3 - T2)
distance = RTT * c / 2
```

Accuracy depends on line of sight, multipath, and signal strength. For best
results use 20 MHz channels and keep RSSI above about −70 dBm.

## Hardware

| Item | Detail |
|------|--------|
| Board | [Waveshare ESP32-C6-Touch-AMOLED-1.32](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32) |
| SoC | ESP32-C6, 16 MB flash |
| Display | 466×466 AMOLED, SH8601 over QSPI |
| Touch | CST820 over I²C (addr `0x15`) |
| Battery | Li-ion via onboard charger, sensed on ADC1 / GPIO0 |

**Pin map**

| Function | GPIO |
|----------|------|
| QSPI CS / SCLK | 22 / 18 |
| QSPI D0–D3 | 19, 20, 10, 11 |
| LCD reset / power | 21 / 2 |
| Touch SDA / SCL | 8 / 7 |
| Battery ADC | 0 |

See [docs/hardware-waveshare-c6-amoled.md](docs/hardware-waveshare-c6-amoled.md) for details.

## Getting started

### Prerequisites

- [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c6/get-started/)
- Alternatively, open the repo in the included **Dev Container** (`.devcontainer/`),
  which uses the official `espressif/idf` image.

### Build and flash

```bash
source ~/esp/v5.5.1/esp-idf/export.sh   # adjust to your ESP-IDF install

idf.py build
idf.py -p <PORT> flash monitor          # e.g. /dev/cu.usbmodem1101 or /dev/ttyACM0
```

`sdkconfig.defaults` already selects the `esp32c6` target, 16 MB flash, the custom
partition table, USB-Serial/JTAG console, FTM, and the LVGL fonts in use, so you
don't need to run `set-target` or `menuconfig` for a normal build. The first build
downloads managed components (LVGL, `esp_lcd_sh8601`) through the IDF Component Manager.

If flashing fails, hold **BOOT**, tap **RESET**, release **BOOT**, and try again.
`tools/troubleshoot.sh` checks common environment problems.

## Console commands

| Command | Description |
|---------|-------------|
| `scan [ssid]` | Scan for APs and flag FTM responders |
| `ftm -I` | Initiate FTM with every responder found |
| `ftm -I -s <ssid> [-c <0/8/16/24/32/64>] [-p <0-100>]` | Initiate FTM with one AP (frame count, burst period ×100 ms) |
| `ftm -R [-e \| -d] [-o <cm>]` | Responder control: enable/disable, T1 offset calibration |
| `ap <ssid> <pass> [-c <ch>] [-b <20/40>]` | Start a SoftAP with FTM responder |
| `sta <ssid> [pass]` | Join an AP as a station |
| `query` | Show current Wi-Fi info |
| `screen <id>` / `screen_next` / `screen_prev` / `screen_current` | Drive the UI from the console |
| `heap_trace` | Heap tracing for leak diagnostics |

Type `help` on the device for the full list of options.

**Calibration tip:** with a responder at zero distance, read the measured offset in cm
and apply it with `ftm -R -o <cm>`.

## Project layout

```
.
├── CMakeLists.txt          # ESP-IDF project definition
├── sdkconfig.defaults      # Minimal project config (target, flash, FTM, LVGL)
├── partitions.csv          # Flash partition table
├── main/
│   ├── ftm_main.c          # Wi-Fi/FTM control, console commands, state machine
│   ├── ftm_display.c/.h    # LVGL UI, screens, touch and gesture handling
│   ├── ftm_battery.c/.h    # Battery ADC monitoring
│   ├── Kconfig.projbuild   # FTM report logging options
│   ├── idf_component.yml   # Managed component dependencies
│   └── lv_conf.h
├── docs/                   # Design notes and troubleshooting
└── tools/troubleshoot.sh   # Environment/flash diagnostics
```

## Documentation

- [Automatic FTM scanning](docs/auto-ftm-scanning.md)
- [Battery monitoring](docs/battery-monitoring.md)
- [FTM troubleshooting](docs/ftm-troubleshooting.md)
- [Hardware notes: Waveshare C6 AMOLED](docs/hardware-waveshare-c6-amoled.md)
- [ESP32-C6 FTM responder scan bug report](docs/esp32c6-ftm-responder-bug-report.md)
- [Archive](docs/archive/): notes from the earlier ESP32-S3 / 1.28" LCD prototype

## Known issues

- ESP32-C6 does not always set the `FTM Responder` flag in scan results, so known
  responders are matched by SSID as a workaround. See the
  [bug report](docs/esp32c6-ftm-responder-bug-report.md).
- The responder SoftAP uses a fixed default password (`ftm12345`). Change it in
  `main/ftm_display.c` before using it anywhere other than a test bench.

## Acknowledgements

- Based on the Espressif ESP-IDF [`wifi/ftm`](https://github.com/espressif/esp-idf/tree/master/examples/wifi/ftm) example (Public Domain / CC0).
- Display bring-up follows Waveshare's official ESP-IDF examples.
