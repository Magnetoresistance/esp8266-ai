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

// ---- Music spectrum (bridge captures system audio, device draws bars) ----
// Type 0..2: bars / wave / radial — each has its own effect variants:
//   bars   effect 0..6: classic / mirror / peak-hold / twin / dotted / glow / fire
//   wave   effect 0..2: line / mirror / aurora
//   radial effect 0..1: ring / starry
// Color 0..7: green / cyan / yellow / orange / red / magenta / white / gradient
#define SPECTRUM_STYLE_FILE "/spectrum_style.txt"
#define SPECTRUM_DEFAULT_TYPE 0
#define SPECTRUM_DEFAULT_EFFECT 0
#define SPECTRUM_DEFAULT_COLOR 0
#define SPECTRUM_DEFAULT_COLOR2 1   // secondary color (combo styles: wave/bars)
#define SPECTRUM_DEFAULT_PEAK 1       // peak-hold dots on bars
#define SPECTRUM_DEFAULT_SMOOTH 3     // time-smoothing 0..10
#define SPECTRUM_DEFAULT_WIDTH 3      // bar width 1..5 (5 = full slot)
#define SPECTRUM_DEFAULT_RAINBOW 0    // 0=solid palette 1=spectrum by value
#define SPECTRUM_BARS 24
#define SPECTRUM_POLL_INTERVAL_MS 80    // ~12 fps on the device
// 色谱行 (color 11): the gradient is screen-anchored and fixed. The hue
// sweep spans [0 .. gradRange% of the screen height]; rows beyond that
// threshold are painted with the sweep's final hue. gradReverse flips the
// sweep direction (正序/倒序).
#define SPECTRUM_DEFAULT_GRAD_RANGE 100  // 0-100% of screen height
#define SPECTRUM_DEFAULT_GRAD_REVERSE 0  // 0=正序 1=倒序
// bars-type dynamic range: normalize each bar to the live [min,max] envelope
// so the spectrum always fills the panel (offset shifts it up/down).
#define SPECTRUM_DEFAULT_AUTORANGE 0     // 0=off 1=normalize to live min/max
#define SPECTRUM_DEFAULT_OFFSET 0        // -100..100 height offset (px-scale)
// silence gate: if the loudest bar is below this threshold the frame is
// treated as silence and zeroed (user-adjustable, 0-50).
#define SPECTRUM_DEFAULT_SILENCE 6
// vertical mirror for bars/wave: bars & wave drawn mirrored across the
// horizontal center line of the screen (the axis itself is not drawn).
#define SPECTRUM_DEFAULT_MIRROR 0
// dual-ring for 环形/扇形 radial styles: each slice drawn as an inner and an
// outer ring segment (r1 + r2), like the dedicated 双环 style.
#define SPECTRUM_DEFAULT_DUALRING 0
// dual-ring fine-tuning: 内环幅度 (0-100%, how far r1 sweeps) and 外环增量
// (0-100%, the extra reach of r2 beyond r1).
#define SPECTRUM_DEFAULT_DUAL_INNER 100
#define SPECTRUM_DEFAULT_DUAL_OUTER 100
