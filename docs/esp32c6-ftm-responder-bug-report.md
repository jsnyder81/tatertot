# ESP32-C6 Does Not Report FTM_RESPONDER Capability in Wi-Fi Scan Results

## Environment

- **Development Kit**: ESP32-C6 (QFN56) revision v0.2
- **Module or chip used**: ESP32-C6 with 16MB flash
- **IDF version**: v6.1 (please confirm your exact version)
- **Build System**: CMake
- **Compiler version**: (please add your compiler version)
- **Operating System**: macOS
- **Power Supply**: USB

## Problem Description

The ESP32-C6 fails to detect the `ftm_responder` capability flag during Wi-Fi scans, even when scanning Access Points that properly advertise FTM (Fine Timing Measurement) support. The same AP, when scanned by an ESP32-S3, correctly shows the FTM Responder capability.

This affects the ability to identify FTM-capable Access Points during network discovery, though FTM sessions can still be initiated if the AP SSID is known.

## Expected Behavior

When performing a Wi-Fi scan with `esp_wifi_scan_start()` and retrieving results with `esp_wifi_scan_get_ap_records()`, Access Points that advertise FTM Responder capability should have the `ftm_responder` field set to `true` in the `wifi_ap_record_t` structure.

## Actual Behavior

On ESP32-C6, the `ftm_responder` field in `wifi_ap_record_t` remains `false` (0) for all scanned APs, even when:
- The AP is an ESP32-S3 configured with `ftm_responder = true`
- The same AP is correctly detected as FTM-capable when scanned by another ESP32-S3
- The AP properly advertises FTM capability in beacon frames
- Scan is configured with `show_hidden = true` and active scanning

## Works Correctly On

- **ESP32-S3** (QFN56) revision v0.2
  - Chip type: ESP32-S3
  - Features: Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 2MB
  - Flash: 4MB

The ESP32-S3 correctly populates the `ftm_responder` field when scanning the same Access Points.

## Steps to Reproduce

### Setup
1. Configure an ESP32-S3 as an FTM Responder AP:
   ```c
   wifi_config_t g_ap_config = {
       .ap.ssid = "FTM_TEST_AP",
       .ap.password = "password123",
       .ap.max_connection = 4,
       .ap.authmode = WIFI_AUTH_WPA2_PSK,
       .ap.ftm_responder = true
   };
   esp_wifi_set_config(WIFI_IF_AP, &g_ap_config);
   ```

2. On ESP32-C6, perform a scan:
   ```c
   wifi_scan_config_t scan_config = {
       .ssid = NULL,  // Scan all APs
       .show_hidden = true,
       .scan_type = WIFI_SCAN_TYPE_ACTIVE,
       .scan_time.active.min = 120,
       .scan_time.active.max = 150
   };
   esp_wifi_scan_start(&scan_config, true);

   uint16_t ap_count;
   esp_wifi_scan_get_ap_num(&ap_count);
   wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
   esp_wifi_scan_get_ap_records(&ap_count, ap_list);

   // Check ftm_responder field
   for (int i = 0; i < ap_count; i++) {
       printf("SSID: %s, FTM Responder: %d\n",
              ap_list[i].ssid, ap_list[i].ftm_responder);
   }
   ```

3. Repeat the same scan from an ESP32-S3 for comparison

### Results
- **ESP32-C6**: `ftm_responder = 0` for all APs (incorrect)
- **ESP32-S3**: `ftm_responder = 1` for FTM-capable APs (correct)

## Debug Logs

### ESP32-C6 Scan Output (Incorrect)
```
I (12345) ftm_station: [FTM_TEST_AP][rssi=-45][ftm_responder=0][ftm_initiator=0]
W (12346) ftm_station: Note: ESP32-C6 may not detect FTM capability in scans
```

### ESP32-S3 Scan Output (Correct)
```
I (12345) ftm_station: [FTM_TEST_AP][rssi=-43][ftm_responder=1][ftm_initiator=0][FTM Responder]
```

## Impact

- **Severity**: Medium
- **Workaround exists**: Yes - FTM sessions can still be initiated if the AP SSID is known
- **User Impact**: Applications cannot automatically discover FTM-capable APs on ESP32-C6

## Workaround

While waiting for a fix, the following workaround can be used:

```c
wifi_ap_record_t *find_ftm_responder_ap(const char *ssid)
{
    // Perform scan...

    for (i = 0; i < ap_count; i++) {
        if (strcmp((const char *)ap_list[i].ssid, ssid) == 0) {
            if (!ap_list[i].ftm_responder) {
                ESP_LOGW(TAG, "AP '%s' not detected as FTM Responder in scan", ssid);
                ESP_LOGW(TAG, "This is an ESP32-C6 limitation - attempting FTM anyway");
            }
            return &ap_list[i];  // Return AP even if ftm_responder flag not set
        }
    }
    return NULL;
}
```

The FTM session will fail quickly if the AP doesn't actually support FTM, so proceeding without the flag is safe.

## Additional Context

- The `ftm_initiator` field also appears to always be 0 on ESP32-C6 scans
- FTM functionality itself works correctly on ESP32-C6 when used as an initiator
- The issue appears to be isolated to the scan result parsing/reporting
- This may be related to beacon frame parsing or the Wi-Fi driver's capability detection logic

## Suggested Investigation Areas

1. Check ESP32-C6 Wi-Fi driver beacon frame parsing
2. Compare how ESP32-S3 and ESP32-C6 handle the WLAN_EID_MEASURE_REQUEST and WLAN_EID_RRM_ENABLED_CAPABILITIES information elements
3. Verify that the ESP32-C6 Wi-Fi firmware is correctly extracting FTM capability bits from Extended Capabilities IE (bit 70 for FTM Responder)

## Code References

Full example code demonstrating this issue is available in the ESP-IDF FTM example, modified to add detailed logging of the ftm_responder field.

---

**Is this a known limitation of ESP32-C6, or is this a bug that should be fixed?**
