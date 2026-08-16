#pragma once

// ---- Firmware version (shown on the first-time WiFi setup screen & /api/info) ----
#define FW_VERSION "0.4.11"

// ---- Bridge polling ----
#define BRIDGE_DEFAULT_PORT 8765
#define BRIDGE_DEFAULT_PATH "/status"
#define BRIDGE_POLL_INTERVAL_MS 5000
#define BRIDGE_HTTP_TIMEOUT_MS 3000

// ---- WiFiManager ----
#define WIFI_PORTAL_AP_NAME "AI-Clock-Setup"
#define WIFI_CONFIG_FILE "/bridge_host.txt"

// ---- Backlight ----
#define BRIGHTNESS_FILE "/brightness.txt"
#define BRIGHTNESS_INVERT_FILE "/brightness_invert.txt"
#define BRIGHTNESS_DEFAULT 100
#define BRIGHTNESS_PWM_FREQ 2000 // Hz; default. Kept at 2kHz — user confirmed no camera-shutter banding impact.

// ---- Display layout (128x128 GC9107) ----
#define SCREEN_W 128
#define SCREEN_H 128

// ---- Display rotation (persisted, 0-7, applied at boot + on change) ----
// Values 0-3: standard rotations (0°/90°CW/180°/270°CW)
// Values 4-7: corresponding mirrored variants
#define DISPLAY_CONFIG_FILE "/display.txt"
#define DISPLAY_ROTATION_DEFAULT 0
