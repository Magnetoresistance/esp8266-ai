// ESP8266 WiFi clock: shows local time plus live Claude Code / Codex CLI
// working status and usage quota, polled from a small bridge service that
// runs on the developer's Mac (see ../bridge/bridge.py).
//
// Display: 240x240 SPI ST7789 (TFT_eSPI). Pin mapping is set via build_flags
// in platformio.ini - edit those if your wiring differs.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>

#include "config.h"
#include "img/claude_sprite.h"
#include "img/codex_sprite.h"
#include "img/claude_logo.h"
#include "img/codex_logo.h"

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer webServer(80);

// ---------- custom sprite storage (LittleFS) ----------
// Custom uploads replace the compiled-in default animation without needing a
// firmware rebuild. You POST a raw .gif straight to /sprite/claude or
// /sprite/codex (the device serves its own upload page at "/"); the ESP8266
// decodes and rescales the GIF *on-device* (AnimatedGIF, line-by-line so it
// never needs a full-canvas buffer) into the wire format below, which the
// display path then reads back frame-by-frame:
//   [1 byte frame count][frame0 bytes][frame1 bytes]...
// Each frame is exactly CLAUDE_SPRITE_W x H (or CODEX_SPRITE_W x H) RGB565
// pixels, byte order matching tools/convert_sprites.py's to_rgb565() so the
// compiled-in defaults and custom uploads share one draw path.
const char *CLAUDE_SPRITE_FILE = "/c.bin";
const char *CODEX_SPRITE_FILE = "/x.bin";
const char *CLAUDE_GIF_FILE = "/c.gif"; // raw upload, decoded then removed
const char *CODEX_GIF_FILE = "/x.gif";
const int MAX_CUSTOM_FRAMES = 8;
const size_t CLAUDE_FRAME_BYTES = (size_t)CLAUDE_SPRITE_W * CLAUDE_SPRITE_H * 2;
const size_t CODEX_FRAME_BYTES = (size_t)CODEX_SPRITE_W * CODEX_SPRITE_H * 2;

// We never hold a whole sprite frame in RAM. Decoding a GIF needs ~24KB of
// heap for AnimatedGIF's own buffers, which wouldn't fit alongside a static
// full-frame buffer (a 120x120 frame is ~28KB) on the ESP8266's ~80KB. So both
// the display path and the decoder work one screen-row at a time through these
// two small scratch rows. Sized for the widest row we ever read: the music
// title strip is 232px wide and the stock name strips 156px, both wider than
// the 128px panel, so a fixed 240-entry buffer is used instead of SCREEN_W.
uint16_t rowBuf[240];     // current row being drawn / decoded
uint16_t prevRowBuf[240]; // decode only: same row from the previous frame

bool claudeCustom = false;
int claudeCustomFrames = 0;
bool codexCustom = false;
int codexCustomFrames = 0;
uint32_t spriteRev = 0; // bumped on upload/reset so the Mac mirror re-fetches

const int SCREEN_CX = 64, SCREEN_CY = 64;
const int RING_MARGIN = 2;      // inset from screen edge
const int RING_THICKNESS = 6;   // ring bar thickness (thinner on the 128px panel)
const unsigned long ANIM_INTERVAL_MS = 120;  // sprite frame advance
const unsigned long FLASH_INTERVAL_MS = 400; // "urgent" flash speed
const unsigned long SWITCH_BOTH_MS = 2000;   // both apps working: alternate fast
const unsigned long SWITCH_IDLE_MS = 6000;   // neither working: alternate slow

enum ActiveApp { APP_CLAUDE, APP_CODEX };
ActiveApp currentApp = APP_CLAUDE;
unsigned long lastSwitchMs = 0;

// Display override, settable from the Mac app via POST /api/display:
// auto = follow working status, claude/codex = pin that app on screen,
// net/music = show Mac-side telemetry pages instead of the pet.
enum DisplayMode { MODE_AUTO, MODE_CLAUDE, MODE_CODEX, MODE_NET, MODE_MUSIC, MODE_STOCK, MODE_MIRROR, MODE_ALBUM, MODE_SPECTRUM };
DisplayMode displayMode = MODE_AUTO;

// When AUTO and the Mac reports audio playing, the screen auto-switches to the
// music page and back when it stops ?? same spirit as the Claude/Codex auto
// switch. Only AUTO does this; a pinned mode is always honored as-is.
bool statusMusicPlaying = false;
DisplayMode lastEffectiveMode = MODE_AUTO;

// ---------- net speed mode state ----------
// Rendering is decoupled from the network: pollNet() fetches every 2s and
// only refills a queue of 250ms samples (the bridge samples at 4Hz and tags
// them with a running seq, so nothing is drawn twice or skipped). The sweep
// itself consumes exactly one queued sample every NET_DRAW_INTERVAL_MS, so
// the trace advances at a constant rate no matter how long HTTP takes.
const unsigned long NET_POLL_INTERVAL_MS = 2000; // queue refill cadence
const unsigned long NET_DRAW_INTERVAL_MS = 250;  // one chart step per bridge sample
const int NET_QUEUE = 32;
long netQRx[NET_QUEUE], netQTx[NET_QUEUE]; // ring buffer of pending samples
int netQHead = 0, netQCount = 0;
long netSeq = -1;                          // last bridge sample seq consumed into the queue
long netCurRx = 0, netCurTx = 0;           // smoothed readout for the header
int netCpuPct = -1, netMemPct = -1;        // Mac CPU/MEM row; -1 = bridge sends none (hidden)
String netLastCpuVal, netLastMemVal;       // change detection for the CPU/MEM values
bool netSysLabelsDrawn = false;
unsigned long lastNetPollMs = 0;
unsigned long lastMirrorPollMs = 0;  // screen-mirror frame fetch cadence
unsigned long lastAlbumPollMs = 0;   // photo-album slide cadence
unsigned long lastNetDrawMs = 0;
bool netChromeDrawn = false;
bool netHeaderDirty = false;

// Chart layout (task-manager style scrolling area chart, newest at the right)
// - sized for the 128px panel.
// NET_CHART_Y is intentionally placed well below the scale label text zone
// (see constants below) so the 52-row pushImage loop never overlaps the
// label pixels. Previously NET_CHART_Y=34 overlapped the last 5 rows of
// the font1 scale text (Y=31..38) -> the user-visible defect was "max
// value display still covered by the scrolling net curve".
const int NET_CHART_X = 4, NET_CHART_Y = 41, NET_CHART_W = 120, NET_CHART_H = 52;
// Scale label (adaptive "full-scale speed" like "69k") sits in fixed canvas
// coordinates [rule #1 of XP 100025465 for side-legend UI text]. Previous
// Y=31 top-aligned exactly with the last row (Y=31) of the UP header
// value's 18-row-tall fillRect clear, which black-wiped 1 row of the top
// glyph pixels whenever the upload number changed -> user visible "UP
// number covers the max value display just a little bit". Moving it down
// by 2 rows gives a 1-row clear gap vs. UP header clear (Y=31) and still
// leaves 1 row vs. chart top (chart starts at 41, label bottom = 40).
// Zone: Y=33..40 (font1 cell height 8).
const int NET_SCALE_LABEL_Y = 33;
const int NET_SCALE_LABEL_H = 8;
// Safety check (compile-time would be nice, but we assert geometry at the
// definition site anyway): label bottom = 33+8 = 41, chart first pixel row = 41,
// so label occupies Y=[33,40] and chart starts writing at Y=[41]; no overlap
// even though they abut (1-row air gap between last label row and chart
// start is actually label Y=40 vs chart Y=41 - they are adjacent).
long netHistRx[NET_CHART_W], netHistTx[NET_CHART_W]; // one 250ms sample per column
long netScale = 10240;    // current "nice" full-scale value (whole chart shares it)
String netLastDl, netLastUl, netLastScaleText; // change detection for partial redraws

// ---------- music mode state ----------
// Bridge sends a fixed 128x128 cover and a 232x44 title strip; the 128px
// panel shows them at half size (2x downsample below).
const int MUSIC_COVER_W = 128;
const int MUSIC_COVER_H = 128;
const int MUSIC_COVER_DW = 64, MUSIC_COVER_DH = 64; // displayed size on panel
const int MUSIC_COVER_X = (SCREEN_W - MUSIC_COVER_DW) / 2, MUSIC_COVER_Y = 4;
// Title/artist come as a Mac-rendered bitmap strip (232x44) because the
// panel fonts are ASCII-only and CJK titles would render as blanks.
const int MUSIC_TEXT_W = 232;
const int MUSIC_TEXT_H = 44;
const int MUSIC_TEXT_DW = 116, MUSIC_TEXT_DH = 22; // displayed size on panel
const int MUSIC_TEXT_X = (SCREEN_W - MUSIC_TEXT_DW) / 2, MUSIC_TEXT_Y = 72;
const unsigned long MUSIC_POLL_INTERVAL_MS = 2000;
// ---------- stock watchlist mode state ----------
// Rows come pre-formatted from the bridge (GET /stock or serial #STOCK):
// ASCII code + price/pct strings + up flag, so the firmware just paints.
const unsigned long STOCK_POLL_INTERVAL_MS = 5000;
const int MAX_STOCKS = 4;
struct StockRow {
  String code, price, pct;
  int up = 0; // 1 rising (red, CN convention) / -1 falling (green) / 0 flat
};
StockRow stocks[MAX_STOCKS];
int stockCount = 0;
bool stockEverLoaded = false;
bool stockDirty = false;
bool stockChromeDrawn = false;
String stockLastCode[MAX_STOCKS]; // top line (code + CJK name strip)
String stockLastVal[MAX_STOCKS];  // value line (price + pct)
unsigned long lastStockPollMs = 0;
// CJK names come as Mac-rendered RGB565 strips (GET /stock/names.raw, one
// 156x24 strip per row) - names_rev says when to re-fetch. -1 = not drawn.
// 24px source at 2x downsample = 12px on panel (1.5x the old 16px strip),
// must match the bridge's NameW/NameH (no size negotiation on the wire).
const int STOCK_NAME_W = 156, STOCK_NAME_H = 24;
int stockNamesRev = -1;
int stockNamesDrawnRev = -1;

String musicTitle, musicArtist, musicAlbum;
bool musicPlaying = false;
int musicElapsed = 0, musicDuration = 0;
int musicArtworkRev = -1;
int musicTextRev = -1;
bool musicHasArtwork = false;
bool musicChromeDrawn = false;
unsigned long lastMusicPollMs = 0;

// ---- music spectrum (bridge computes bars from system audio) ----
int spectrumType = SPECTRUM_DEFAULT_TYPE;     // 0 bars / 1 wave / 2 radial
int spectrumEffect = SPECTRUM_DEFAULT_EFFECT; // per-type effect index
int spectrumColor = SPECTRUM_DEFAULT_COLOR;   // 0..7 palette index
int spectrumColor2 = SPECTRUM_DEFAULT_COLOR2; // 0..7 secondary (combo styles)
int spectrumPeak = SPECTRUM_DEFAULT_PEAK;     // 0=off 1=on peak dots
int spectrumSmooth = SPECTRUM_DEFAULT_SMOOTH; // 0..10 time smoothing
int spectrumWidth = SPECTRUM_DEFAULT_WIDTH;   // 1..5 bar width
int spectrumRainbow = SPECTRUM_DEFAULT_RAINBOW; // 0=solid 1=spectrum by value
// per-type fine-tuning:
int spectrumGap = 1;        // 0..2 bar gap (bars type)
int spectrumDecay = 5;      // 1..10 peak-hold decay speed (higher = faster fall)
int spectrumLineW = 1;      // 1..3 wave line thickness (wave type)
int spectrumFill = 0;       // 0=line only 1=fill under wave (wave type)
int spectrumFillColor = 0;  // 0..7 palette for the wave fill area
// radial/ring type fine-tuning:
int spectrumRingW = 2;      // 1..4 ring line thickness
int spectrumRingGap = 2;    // 0..4 gap between ring spokes/bars
int spectrumRingInner = 12; // 4..40 inner circle radius (px)
int spectrumRingOuter = 58; // 20..64 outer circle radius (px)
int spectrumRingInColor = 1;  // 0..7 palette + 8=black + 9=off (no ring)
int spectrumRingFill = 1;     // 0=line only 1=fill ribbon (ring polyline)
// ????? (color 11) gradient tuning:
int spectrumGradRange = SPECTRUM_DEFAULT_GRAD_RANGE;   // 0..100% screen height
int spectrumGradReverse = SPECTRUM_DEFAULT_GRAD_REVERSE; // 0=???? 1=????
// bars-type dynamic range:
int spectrumAutoRange = SPECTRUM_DEFAULT_AUTORANGE; // 1=normalize to live min/max
int spectrumOffset = SPECTRUM_DEFAULT_OFFSET;       // -100..100 height offset
int spectrumSilence = SPECTRUM_DEFAULT_SILENCE;     // 0..50 silence gate
int spectrumMirror = SPECTRUM_DEFAULT_MIRROR;       // 1=mirror bars/wave vertically
int spectrumDualRing = SPECTRUM_DEFAULT_DUALRING;   // 1=dual-ring for ring/fan
int spectrumDualInner = SPECTRUM_DEFAULT_DUAL_INNER; // dual-ring inner sweep %
int spectrumDualOuter = SPECTRUM_DEFAULT_DUAL_OUTER; // dual-ring outer reach %
byte spectrumBars[SPECTRUM_BARS];             // 0..255 per bar
float spectrumSmoothed[SPECTRUM_BARS];        // smoothed values (spectrumSmooth)
// adaptive rainbow range: tracks the live min/max of the bars so the color
// sweep always spans the *actual* current range (long playback sessions keep
// vivid color variation instead of a near-constant hue).
float spectrumRainbowMin = 0;
float spectrumRainbowMax = 255;
byte spectrumPeaks[SPECTRUM_BARS];            // peak-hold state for style 3
bool spectrumData = false;                    // true once a frame arrived
unsigned long lastSpectrumPollMs = 0;
// incremental-draw state: previous frame's geometry so we only repaint the
// changed pixels instead of clearing the whole panel (removes flicker).
byte prevSpectrumH[SPECTRUM_BARS];            // last bar height (px) per slot
int prevWaveY[SPECTRUM_BARS];                 // last waveform y per slot
int prevWaveY2[SPECTRUM_BARS];                // last mirrored-wave y per slot
byte prevPeakY[SPECTRUM_BARS];                // last peak dot y per slot
byte prevRadialR[SPECTRUM_BARS];              // last radial length per slot
byte prevRadialInner[SPECTRUM_BARS];          // last dual-ring inner radius offset
bool spectrumFirstDraw = true;                // needs a full clear first
int lastSpectrumStyleDrawn = -1;              // style that is currently on screen
byte lastDrawnBars[SPECTRUM_BARS];            // bars of the last painted frame
int lastSpectrumParamKey = -1;                // param hash of the last frame

int claudeFrame = 0;
int codexFrame = 0;
unsigned long lastAnimMs = 0;

bool flashOn = true;
unsigned long lastFlashMs = 0;

// Bridge host is not asked for during first-time WiFi setup: the Mac/Windows
// bridge discovers the device and pairs automatically (or set via /api/bridge).
String bridgeHost;

struct ClaudeStatus {
  String status = "unknown";
  long tokensToday = 0;
  int sessionMin = 0;
  int sessionWindowMin = 300;
  float fiveHourPct = -1; // real OAuth quota from the bridge, -1 = unknown
  int fiveHourResetMin = -1; // minutes until the 5h window resets
  float sevenDayPct = -1;
  int sevenDayResetMin = -1; // minutes until the 7-day window resets
  bool needsInput = false; // waiting on a permission/approval prompt
};

struct CodexStatus {
  String status = "unknown";
  long tokensToday = 0;
  float primaryPct = -1;
  int primaryResetMin = -1;
  float weeklyPct = -1;
  int weeklyResetMin = -1;
  bool needsInput = false;
};

ClaudeStatus claudeStatus;
CodexStatus codexStatus;

unsigned long lastPollMs = 0;
unsigned long lastSuccessMs = 0;
bool everPolled = false;
bool mainUiShown = false;      // false while the config-portal screen is up
bool webServerStarted = false; // deferred: port 80 clashes with the portal

// ---------- backlight brightness ----------
// The panel backlight (TFT_BL, active LOW) is PWM-dimmable ?? the vendor's own
// firmware does the same. 0 = off, 100 = full. Persisted so it survives reboot.

int brightness = BRIGHTNESS_DEFAULT; // 0-100
bool brightnessInvert = false; // when true, PWM is inverted (for active-HIGH backlight panels wired opposite)
int displayRotation = DISPLAY_ROTATION_DEFAULT; // 0-7

void applyBrightness() {
  // TFT_BACKLIGHT_ON controls default polarity (set in platformio.ini):
  //   HIGH = active HIGH (pin HIGH = backlight on)
  //   LOW  = active LOW  (pin LOW  = backlight on)
  // brightnessInvert manually toggles the formula so users whose panel is
  // wired opposite can fix it without recompiling.
  // analogWrite() duty: 0 = always LOW, range = always HIGH.
#if TFT_BACKLIGHT_ON == HIGH
  bool normal = !brightnessInvert;
#else
  bool normal = brightnessInvert;
#endif
  if (normal) {
    // Active HIGH: higher duty = brighter
    analogWrite(TFT_BL, brightness);
  } else {
    // Active LOW: lower duty = brighter (inverted)
    analogWrite(TFT_BL, 100 - brightness);
  }
}

void loadBrightness() {
  if (!LittleFS.exists(BRIGHTNESS_FILE)) return;
  File f = LittleFS.open(BRIGHTNESS_FILE, "r");
  if (!f) return;
  int v = f.readStringUntil('\n').toInt();
  f.close();
  if (v >= 0 && v <= 100) brightness = v;
}

void saveBrightness() {
  File f = LittleFS.open(BRIGHTNESS_FILE, "w");
  if (!f) return;
  f.println(brightness);
  f.close();
}

// ---------- backlight polarity invert (for panels wired opposite) ----------

void loadBrightnessInvert() {
  if (!LittleFS.exists(BRIGHTNESS_INVERT_FILE)) return;
  File f = LittleFS.open(BRIGHTNESS_INVERT_FILE, "r");
  if (!f) return;
  int v = f.readStringUntil('\n').toInt();
  f.close();
  brightnessInvert = (v != 0);
}

void saveBrightnessInvert() {
  File f = LittleFS.open(BRIGHTNESS_INVERT_FILE, "w");
  if (!f) return;
  f.println(brightnessInvert ? 1 : 0);
  f.close();
}

// ---------- display rotation / mirror ----------

void applyDisplayRotation() {
  // Always call setRotation() first so TFT_eSPI's internal state (_width,
  // _height, addressing) stays consistent with the base orientation.
  int baseRot = displayRotation & 3;
  tft.setRotation(baseRot);

  // GC9107 is a 128x160 GRAM chip but this 0.85" panel only exposes 128x128
  // of it, at an offset inside the GRAM (Wisevision N085-1212TBWIG41-H12:
  // colstart=2, rowstart=1). The ST7789_2_Rotation.h CGRAM_OFFSET table has
  // been patched with a 128x128 entry, so setRotation() applies it.

  // For mirrored variants (values 4-7) we override MADCTL to toggle the MX
  // bit (column-address swap = horizontal mirror from the viewer's perspective
  // when the display is in portrait / MV=0).  The 8 values below cover all
  // MX/MY/MV combinations for a 128x128 square panel.
  if (displayRotation & 4) {
    tft.writecommand(TFT_MADCTL);
    switch (baseRot) {
      case 0: tft.writedata(TFT_MAD_MX | TFT_MAD_COLOR_ORDER);            break; // 0x48
      case 1: tft.writedata(TFT_MAD_MV | TFT_MAD_COLOR_ORDER);            break; // 0x28
      case 2: tft.writedata(TFT_MAD_MY | TFT_MAD_COLOR_ORDER);            break; // 0x88
      case 3: tft.writedata(TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV |
                            TFT_MAD_COLOR_ORDER);                          break; // 0xE8
    }
  }
  Serial.printf("[display] rotation=%d (base=%d, mirror=%d)\n",
                displayRotation, baseRot, !!(displayRotation & 4));
}

void loadDisplayConfig() {
  if (!LittleFS.exists(DISPLAY_CONFIG_FILE)) return;
  File f = LittleFS.open(DISPLAY_CONFIG_FILE, "r");
  if (!f) return;
  int v = f.readStringUntil('\n').toInt();
  f.close();
  if (v >= 0 && v <= 7) displayRotation = v;
}

void saveDisplayConfig() {
  File f = LittleFS.open(DISPLAY_CONFIG_FILE, "w");
  if (!f) return;
  f.println(displayRotation);
  f.close();
}

// ---------- music spectrum style persistence ----------

void loadSpectrumStyle() {
  if (!LittleFS.exists(SPECTRUM_STYLE_FILE)) return;
  File f = LittleFS.open(SPECTRUM_STYLE_FILE, "r");
  if (!f) return;
  // 28 lines: type / effect / color / color2 / peak / smooth / width /
  // rainbow / gap / decay / linew / fill / ringw / ringgap / ringinner /
  // ringouter / fillcolor / ringincolor / ringfill / gradrange / gradreverse
  // / autorange / offset / silence / mirror / dualring / dualinner / dualouter
  int lines[28] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
  int idx = 0;
  while (f.available() && idx < 28) {
    lines[idx++] = f.readStringUntil('\n').toInt();
  }
  f.close();
  if (lines[1] < 0) {
    // legacy flat-style file: single line 0-11 -> type+effect
    int old = lines[0];
    switch (old) {
      case 1: spectrumType = 0; spectrumEffect = 1; break;  // mirror bars
      case 2: spectrumType = 1; spectrumEffect = 0; break;  // wave line
      case 3: spectrumType = 0; spectrumEffect = 2; break;  // peak-hold
      case 4: spectrumType = 2; spectrumEffect = 0; break;  // radial ring
      case 5: spectrumType = 0; spectrumEffect = 3; break;  // twin
      case 6: spectrumType = 0; spectrumEffect = 4; break;  // dotted
      case 7: spectrumType = 0; spectrumEffect = 5; break;  // glow
      case 8: spectrumType = 1; spectrumEffect = 1; break;  // mirror wave
      case 9: spectrumType = 0; spectrumEffect = 6; break;  // fire
      case 10: spectrumType = 1; spectrumEffect = 2; break; // aurora
      case 11: spectrumType = 2; spectrumEffect = 1; break; // starry
      default: spectrumType = 0; spectrumEffect = 0; break; // classic
    }
  } else {
    if (lines[0] >= 0 && lines[0] <= 2) spectrumType = lines[0];
    if (lines[1] >= 0 && lines[1] <= 7) spectrumEffect = lines[1];
  }
  if (lines[2] >= 0 && lines[2] <= 11) spectrumColor = lines[2];
  if (lines[3] >= 0 && lines[3] <= 11) spectrumColor2 = lines[3];
  if (lines[4] == 0 || lines[4] == 1) spectrumPeak = lines[4];
  if (lines[5] >= 0 && lines[5] <= 10) spectrumSmooth = lines[5];
  if (lines[6] >= 1 && lines[6] <= 5) spectrumWidth = lines[6];
  if (lines[7] == 0 || lines[7] == 1) spectrumRainbow = lines[7];
  if (lines[8] >= 0 && lines[8] <= 4) spectrumGap = lines[8];
  if (lines[9] >= 1 && lines[9] <= 20) spectrumDecay = lines[9];
  if (lines[10] >= 1 && lines[10] <= 5) spectrumLineW = lines[10];
  if (lines[11] == 0 || lines[11] == 1) spectrumFill = lines[11];
  if (lines[12] >= 1 && lines[12] <= 8) spectrumRingW = lines[12];
  if (lines[13] >= 0 && lines[13] <= 10) spectrumRingGap = lines[13];
  if (lines[14] >= 2 && lines[14] <= 60) spectrumRingInner = lines[14];
  if (lines[15] >= 20 && lines[15] <= 64) spectrumRingOuter = lines[15];
  if (lines[16] >= 0 && lines[16] <= 11) spectrumFillColor = lines[16];
  if (lines[17] >= 0 && lines[17] <= 9) spectrumRingInColor = lines[17];
  if (lines[18] == 0 || lines[18] == 1) spectrumRingFill = lines[18];
  if (lines[19] >= 0 && lines[19] <= 100) spectrumGradRange = lines[19];
  if (lines[20] == 0 || lines[20] == 1) spectrumGradReverse = lines[20];
  if (lines[21] == 0 || lines[21] == 1) spectrumAutoRange = lines[21];
  if (lines[22] >= -100 && lines[22] <= 100) spectrumOffset = lines[22];
  if (lines[23] >= 0 && lines[23] <= 50) spectrumSilence = lines[23];
  if (lines[24] == 0 || lines[24] == 1) spectrumMirror = lines[24];
  if (lines[25] == 0 || lines[25] == 1) spectrumDualRing = lines[25];
  if (lines[26] >= 0 && lines[26] <= 100) spectrumDualInner = lines[26];
  if (lines[27] >= 0 && lines[27] <= 100) spectrumDualOuter = lines[27];
}

void saveSpectrumStyle() {
  File f = LittleFS.open(SPECTRUM_STYLE_FILE, "w");
  if (!f) return;
  f.println(spectrumType);
  f.println(spectrumEffect);
  f.println(spectrumColor);
  f.println(spectrumColor2);
  f.println(spectrumPeak);
  f.println(spectrumSmooth);
  f.println(spectrumWidth);
  f.println(spectrumRainbow);
  f.println(spectrumGap);
  f.println(spectrumDecay);
  f.println(spectrumLineW);
  f.println(spectrumFill);
  f.println(spectrumRingW);
  f.println(spectrumRingGap);
  f.println(spectrumRingInner);
  f.println(spectrumRingOuter);
  f.println(spectrumFillColor);
  f.println(spectrumRingInColor);
  f.println(spectrumRingFill);
  f.println(spectrumGradRange);
  f.println(spectrumGradReverse);
  f.println(spectrumAutoRange);
  f.println(spectrumOffset);
  f.println(spectrumSilence);
  f.println(spectrumMirror);
  f.println(spectrumDualRing);
  f.println(spectrumDualInner);
  f.println(spectrumDualOuter);
  f.close();
}

// ---------- persistence for the bridge host ----------

void loadBridgeHost() {
  if (LittleFS.exists(WIFI_CONFIG_FILE)) {
    File f = LittleFS.open(WIFI_CONFIG_FILE, "r");
    bridgeHost = f.readStringUntil('\n');
    bridgeHost.trim();
    f.close();
  }
}

void saveBridgeHost(const String &host) {
  File f = LittleFS.open(WIFI_CONFIG_FILE, "w");
  f.println(host);
  f.close();
}

// ---------- custom sprite loading ----------

// Checks LittleFS for a previously-uploaded custom sprite and validates its
// size before trusting it (frame count byte + exact expected byte length).
void loadCustomSpriteState() {
  claudeCustom = false;
  if (LittleFS.exists(CLAUDE_SPRITE_FILE)) {
    File f = LittleFS.open(CLAUDE_SPRITE_FILE, "r");
    if (f && f.size() >= 1) {
      uint8_t cnt = f.read();
      size_t expected = 1 + (size_t)cnt * CLAUDE_FRAME_BYTES;
      if (cnt > 0 && cnt <= MAX_CUSTOM_FRAMES && (size_t)f.size() == expected) {
        claudeCustom = true;
        claudeCustomFrames = cnt;
      }
    }
    if (f) f.close();
  }

  codexCustom = false;
  if (LittleFS.exists(CODEX_SPRITE_FILE)) {
    File f = LittleFS.open(CODEX_SPRITE_FILE, "r");
    if (f && f.size() >= 1) {
      uint8_t cnt = f.read();
      size_t expected = 1 + (size_t)cnt * CODEX_FRAME_BYTES;
      if (cnt > 0 && cnt <= MAX_CUSTOM_FRAMES && (size_t)f.size() == expected) {
        codexCustom = true;
        codexCustomFrames = cnt;
      }
    }
    if (f) f.close();
  }

  Serial.printf("[sprite] claude custom=%d frames=%d | codex custom=%d frames=%d\n", claudeCustom,
                claudeCustomFrames, codexCustom, codexCustomFrames);
}

int claudeFrameCount() { return claudeCustom ? claudeCustomFrames : CLAUDE_SPRITE_FRAMES; }
int codexFrameCount() { return codexCustom ? codexCustomFrames : CODEX_SPRITE_FRAMES; }

// Draws one sprite frame centered on screen, one row at a time so we never
// need a full-frame buffer: each row comes either from the custom LittleFS
// file (streamed) or the compiled-in PROGMEM default (copied row-by-row).
void drawSpriteFrame(bool custom, const char *file, const uint16_t *const *progmemFrames, int frameIdx, int w,
                     int h, size_t frameBytes) {
  // 128x128 panel: draw sprites at half native size (2x downsample) so the
  // ring + quota text still fit around them.
  const int S = 2;
  int dw = w / S, dh = h / S;
  int x0 = SCREEN_CX - dw / 2, y0 = SCREEN_CY - dh / 2;
  size_t rowBytes = (size_t)w * 2;
  if (custom) {
    File f = LittleFS.open(file, "r");
    if (!f) return;
    f.seek(1 + (size_t)frameIdx * frameBytes);
    for (int r = 0; r < dh; r++) {
      f.seek(1 + (size_t)frameIdx * frameBytes + (size_t)(r * S) * rowBytes);
      f.read((uint8_t *)rowBuf, rowBytes);
      for (int c = 0; c < dw; c++) rowBuf[c] = rowBuf[c * S];
      tft.pushImage(x0, y0 + r, dw, 1, rowBuf);
    }
    f.close();
  } else {
    const uint16_t *frame = progmemFrames[frameIdx];
    for (int r = 0; r < dh; r++) {
      memcpy_P(rowBuf, frame + (size_t)(r * S) * w, rowBytes);
      for (int c = 0; c < dw; c++) rowBuf[c] = rowBuf[c * S];
      tft.pushImage(x0, y0 + r, dw, 1, rowBuf);
    }
  }
}

// ---------- helpers ----------

String formatTokens(long tokens) {
  if (tokens >= 1000000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fM", tokens / 1000000.0);
    return String(buf);
  }
  if (tokens >= 1000) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fk", tokens / 1000.0);
    return String(buf);
  }
  return String(tokens);
}

// ---------- drawing ----------

void drawStaticChrome() {
  tft.fillScreen(TFT_BLACK);
}

// Bridge unreachable / data stale -> flashing red overrides everything else,
// matches the "urgent, look now" state from the reference signal-light design.
bool bridgeStale() {
  if (!everPolled) return true;
  return (millis() - lastSuccessMs) >= 2UL * BRIDGE_POLL_INTERVAL_MS;
}

// True when the app currently on screen is waiting on a permission/approval
// prompt ?? drives the red "look now, act" border flash.
bool currentAppNeedsInput() {
  return currentApp == APP_CLAUDE ? claudeStatus.needsInput : codexStatus.needsInput;
}

// Working vs idle is now conveyed by the sprite animation itself (moving vs
// still), not by ring color. The ring just stays steady green, except
// bridge-stale which flashes red ("check it now") and overrides everything.
uint16_t currentStatusColor() {
  if (bridgeStale()) return flashOn ? TFT_RED : TFT_BLACK;
  return TFT_GREEN;
}

// The ring is skipped when nothing changed (see drawSquareRing) so the 5s
// poll doesn't visibly blank-and-repaint it. Anything that paints over the
// ring area must invalidate this cache.
float ringLastPct = -1000;
uint16_t ringLastColor = 1;

// Paints the full square border in one color (all four sides), used for the
// attention flash so the whole edge blinks, not just the filled quota arc.
void drawFullBorder(uint16_t color) {
  ringLastPct = -1000; // ring got painted over; next ring draw must repaint
  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int side = SCREEN_W - 2 * RING_MARGIN;
  tft.fillRect(x0, y0, side, RING_THICKNESS, color);                              // top
  tft.fillRect(x0, SCREEN_H - RING_MARGIN - RING_THICKNESS, side, RING_THICKNESS, color); // bottom
  tft.fillRect(x0, y0, RING_THICKNESS, side, color);                              // left
  tft.fillRect(SCREEN_W - RING_MARGIN - RING_THICKNESS, y0, RING_THICKNESS, side, color); // right
}

// Square progress ring hugging the screen edge. `pct` of the perimeter
// (clockwise from top-left) is drawn in `color`, the rest in dark grey.
void drawSquareRing(float pct, uint16_t color) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (pct == ringLastPct && color == ringLastColor) return; // nothing changed
  ringLastPct = pct;
  ringLastColor = color;

  int x0 = RING_MARGIN, y0 = RING_MARGIN;
  int x1 = SCREEN_W - RING_MARGIN, y1 = SCREEN_H - RING_MARGIN;
  int side = x1 - x0;
  float perimeter = side * 4.0;

  // Unfilled track is drawn black (not grey) so it blends into the background
  // and only the active quota portion is visible - still needs to be actively
  // repainted each time though, to erase a previously longer fill if the
  // percentage drops (e.g. a quota window reset).
  tft.fillRect(x0, y0, side, RING_THICKNESS, TFT_BLACK);                  // top
  tft.fillRect(x1 - RING_THICKNESS, y0, RING_THICKNESS, side, TFT_BLACK); // right
  tft.fillRect(x0, y1 - RING_THICKNESS, side, RING_THICKNESS, TFT_BLACK); // bottom
  tft.fillRect(x0, y0, RING_THICKNESS, side, TFT_BLACK);                  // left

  // filled portion, clockwise: top -> right -> bottom -> left
  float remaining = perimeter * (pct / 100.0);
  if (remaining <= 0) return;

  float seg = min(remaining, (float)side);
  tft.fillRect(x0, y0, (int)seg, RING_THICKNESS, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x1 - RING_THICKNESS, y0, RING_THICKNESS, (int)seg, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x1 - (int)seg, y1 - RING_THICKNESS, (int)seg, RING_THICKNESS, color);
  remaining -= side;
  if (remaining <= 0) return;

  seg = min(remaining, (float)side);
  tft.fillRect(x0, y1 - (int)seg, RING_THICKNESS, (int)seg, color);
}

void drawClaudeSprite(int frameIdx) {
  drawSpriteFrame(claudeCustom, CLAUDE_SPRITE_FILE, claude_sprite_frames, frameIdx, CLAUDE_SPRITE_W,
                  CLAUDE_SPRITE_H, CLAUDE_FRAME_BYTES);
}

void drawCodexSprite(int frameIdx) {
  drawSpriteFrame(codexCustom, CODEX_SPRITE_FILE, codex_sprite_frames, frameIdx, CODEX_SPRITE_W, CODEX_SPRITE_H,
                  CODEX_FRAME_BYTES);
}

String pctText(float pct) {
  return pct >= 0 ? String((int)pct) + "%" : "-";
}

// Quota readout below the sprite: two columns ("5h" / "Wk"), small grey label
// over a big font-4 percentage. Values repaint only when their text changes
// (force = after a full-screen clear), so the 5s poll never flashes them.
const int QUOTA_LABEL_Y = 96, QUOTA_VALUE_Y = 104;
const int QUOTA_COL1_X = 32, QUOTA_COL2_X = 96;
String lastQuota5h, lastQuotaWk;

// pushImage() colors must be pre-byte-swapped (this firmware never enables
// setSwapBytes; see the sprite pipeline). Natural RGB565 -> wire order:
inline uint16_t swap565(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

// ---- Nothing-phone-style dot-matrix font (NDot look) ----
// Every piece of ASCII text on the device renders as round dots on a fixed
// grid with visible gaps. Proportional: each glyph is up to 5 columns wide
// (w), 7 rows tall; rows top->bottom, bit (w-1) = leftmost column.
// Sizes used: pitch 2 / r 0 = small labels (h13), pitch 3 / r 1 = values
// (h21), pitch 4 / r 1 = reset corner (h27), pitch 6 / r 2 = countdown (h41).
struct DotGlyph {
  char c;
  uint8_t w;
  uint8_t rows[7];
};

const DotGlyph dotGlyphs[] = {
    {'0', 5, {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', 5, {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', 5, {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}},
    {'3', 5, {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}},
    {'4', 5, {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', 5, {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', 5, {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', 5, {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', 5, {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}},
    {'A', 5, {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', 5, {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', 5, {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', 5, {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', 5, {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', 5, {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', 3, {0b111, 0b010, 0b010, 0b010, 0b010, 0b010, 0b111}},
    {'J', 5, {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}},
    {'K', 5, {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', 5, {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', 5, {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', 5, {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}},
    {'O', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', 5, {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', 5, {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', 5, {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', 5, {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', 5, {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', 5, {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}},
    {'X', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', 5, {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', 5, {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {'d', 5, {0b00001, 0b00001, 0b01101, 0b10011, 0b10001, 0b10011, 0b01101}},
    {'h', 5, {0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001}},
    {'k', 5, {0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010}},
    {'s', 5, {0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110}},
    {'%', 5, {0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011}},
    {':', 1, {0b0, 0b0, 0b1, 0b0, 0b1, 0b0, 0b0}},
    {'.', 1, {0b0, 0b0, 0b0, 0b0, 0b0, 0b0, 0b1}},
    {',', 1, {0b0, 0b0, 0b0, 0b0, 0b0, 0b1, 0b1}},
    {'\'', 1, {0b1, 0b1, 0b0, 0b0, 0b0, 0b0, 0b0}},
    {'!', 1, {0b1, 0b1, 0b1, 0b1, 0b1, 0b0, 0b1}},
    {'?', 5, {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100}},
    {'-', 3, {0b000, 0b000, 0b000, 0b111, 0b000, 0b000, 0b000}},
    {'+', 3, {0b000, 0b010, 0b010, 0b111, 0b010, 0b010, 0b000}},
    {'/', 3, {0b001, 0b001, 0b010, 0b010, 0b010, 0b100, 0b100}},
    {' ', 2, {0b0, 0b0, 0b0, 0b0, 0b0, 0b0, 0b0}},
};

const DotGlyph *dotGlyph(char c) {
  for (const DotGlyph &g : dotGlyphs)
    if (g.c == c) return &g;
  if (c >= 'a' && c <= 'z') return dotGlyph(c - 32); // no dedicated lowercase -> caps
  return nullptr;
}

// Advance from one glyph origin to the next (glyph width + one grid gap).
int dotCharAdv(char c, int pitch, int r) {
  const DotGlyph *g = dotGlyph(c);
  int w = g ? g->w : 3; // unknown chars advance like a 3-col blank
  return (w - 1) * pitch + 2 * r + 1 + pitch;
}

int dotTextWidth(const String &s, int pitch, int r) {
  if (s.length() == 0) return 0;
  int w = 0;
  for (unsigned int i = 0; i < s.length(); i++) w += dotCharAdv(s[i], pitch, r);
  return w - pitch; // no gap after the last glyph
}

int dotTextHeight(int pitch, int r) { return 6 * pitch + 2 * r + 1; }

// One glyph, (x, y) = top-left edge. r = dot radius (0 = single pixel).
void drawDotChar(char c, int x, int y, int pitch, int r, uint16_t color) {
  const DotGlyph *g = dotGlyph(c);
  if (!g) return;
  for (int ry = 0; ry < 7; ry++)
    for (int cx = 0; cx < g->w; cx++)
      if (g->rows[ry] & (1 << (g->w - 1 - cx))) {
        if (r <= 0) tft.drawPixel(x + r + cx * pitch, y + r + ry * pitch, color);
        else tft.fillCircle(x + r + cx * pitch, y + r + ry * pitch, r, color);
      }
}

void drawDotText(const String &s, int x, int y, int pitch, int r, uint16_t color) {
  for (unsigned int i = 0; i < s.length(); i++) {
    drawDotChar(s[i], x, y, pitch, r, color);
    x += dotCharAdv(s[i], pitch, r);
  }
}

void drawDotTextC(const String &s, int cx, int y, int pitch, int r, uint16_t color) {
  drawDotText(s, cx - dotTextWidth(s, pitch, r) / 2, y, pitch, r, color);
}

void drawDotTextR(const String &s, int xRight, int y, int pitch, int r, uint16_t color) {
  drawDotText(s, xRight - dotTextWidth(s, pitch, r), y, pitch, r, color);
}

String dotFitText(String s, int maxPx, int pitch, int r) {
  if (dotTextWidth(s, pitch, r) <= maxPx) return s;
  while (s.length() > 0 && dotTextWidth(s + "..", pitch, r) > maxPx) s.remove(s.length() - 1);
  return s + "..";
}

// Square-dot variant on the same 5x7 glyphs: d x d filled squares. With
// d == pitch the strokes fuse solid - bold mini caps to pair with the dotted
// numerals (Nothing does the same: dotted digits, solid small labels).
int sqCharAdv(char c, int pitch, int d) {
  const DotGlyph *g = dotGlyph(c);
  int w = g ? g->w : 3;
  return (w - 1) * pitch + d + pitch;
}

int sqTextWidth(const String &s, int pitch, int d) {
  if (s.length() == 0) return 0;
  int w = 0;
  for (unsigned int i = 0; i < s.length(); i++) w += sqCharAdv(s[i], pitch, d);
  return w - pitch;
}

void drawSqText(const String &s, int x, int y, int pitch, int d, uint16_t color) {
  for (unsigned int i = 0; i < s.length(); i++) {
    const DotGlyph *g = dotGlyph(s[i]);
    if (g)
      for (int ry = 0; ry < 7; ry++)
        for (int cx = 0; cx < g->w; cx++)
          if (g->rows[ry] & (1 << (g->w - 1 - cx)))
            tft.fillRect(x + cx * pitch, y + ry * pitch, d, d, color);
    x += sqCharAdv(s[i], pitch, d);
  }
}

void drawSqTextC(const String &s, int cx, int y, int pitch, int d, uint16_t color) {
  drawSqText(s, cx - sqTextWidth(s, pitch, d) / 2, y, pitch, d, color);
}

// Tiny-bold 3x5 caps (2x2 square dots): denser than the 5x7 face at the
// same height, used where a label must stay narrow but readable.
struct TinyGlyph {
  char c;
  uint8_t rows[5];
};

const TinyGlyph tinyGlyphs[] = {
    {'R', {0b110, 0b101, 0b110, 0b101, 0b101}},
    {'E', {0b111, 0b100, 0b110, 0b100, 0b111}},
    {'S', {0b011, 0b100, 0b010, 0b001, 0b110}},
    {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
};

void drawTinyBoldText(const String &s, int cx, int y, uint16_t color) {
  const int P = 2, D = 2;                                  // grid pitch / square dot size
  int x = cx - ((int)s.length() * (2 * P + D) + ((int)s.length() - 1) * 2) / 2;
  for (unsigned int i = 0; i < s.length(); i++) {
    for (const TinyGlyph &g : tinyGlyphs)
      if (g.c == s[i]) {
        for (int ry = 0; ry < 5; ry++)
          for (int gx = 0; gx < 3; gx++)
            if (g.rows[ry] & (0b100 >> gx)) tft.fillRect(x + gx * P, y + ry * P, D, D, color);
        break;
      }
    x += 2 * P + D + 2;
  }
}

void drawQuotaText(float hourPct, float weekPct, bool force) {
  // Codex dropped the 5h window (2026-07): the bridge then sends
  // primary_pct=null, so collapse to a single centered "Wk" column.
  bool single = hourPct < 0 && weekPct >= 0;
  static int8_t lastSingle = -1;

  // ---- Bounding box safety (per Experience 100010326): keep every
  // fillRect STRICTLY inside the ring. Ring columns:
  //   left   : [2, 8]  -> quota rects must start at X >= 9
  //   right  : [120,126] -> quota rects must end   at X <= 119
  // The previous 64px-wide column clears (X=0..64 / 64..128) overshot into
  // both side rings and erased the ~6 vertical pixels at the height of the
  // quota value row -> exactly the "both sides of the bottom % numbers are
  // clipped" symptom the user reported.
  //
  // Real text geometry (pitch=2 r=1 for values, pitch=1 d=1 for labels):
  //   Value "100%"  4 chars: 4 * 13 - 2 = 50 wide  -> half = 25 + 2 pad = 27
  //   Value "100"   3 chars: 3 * 13 - 2 = 37 wide  -> half = 19 + 2 pad = 21
  //   Label "Wk"               sqTextWidth ~11     -> half = 6 + 1 pad = 7
  // Use halfWidth = 27 for single column (covers worst case "100%"), = 21
  // for dual columns ("100"/"80"/... never more than 3 chars because
  // pctText caps at int percent + "%" = up to "100%"). But at QUOTA_COL1_X=32
  // half=27 would start at X=5, still inside the left ring [2..8]; so clamp
  // to safe half = 23 -> col1 X??[9,55] and col2 X??[73,119].
  const int VAL_HALF_WIDE = 27;   // single-col worst case
  const int VAL_HALF_NARROW = 23; // dual-col safe (stay clear of both rings)
  const int LBL_HALF = 9;        // "5h"/"Wk" labels + pad
  const int QUOTA_BAR_H = QUOTA_VALUE_Y + 15 - QUOTA_LABEL_Y; // label row + value row = 23

  if ((int8_t)single != lastSingle) {
    lastSingle = (int8_t)single;
    force = true;
    // Layout-swap clear: keep inside the ring (X??[9..119]) to avoid erasing
    // the side rails. Width = 119 - 9 = 110, centered on SCREEN_CX=64 -> X=9.
    tft.fillRect(9, QUOTA_LABEL_Y, 110, QUOTA_BAR_H, TFT_BLACK);
  }
  if (single) {
    if (force) drawSqTextC("Wk", SCREEN_CX, QUOTA_LABEL_Y, 1, 1, TFT_LIGHTGREY);
    String v = pctText(weekPct);
    if (force || v != lastQuotaWk) {
      lastQuotaWk = v;
      lastQuota5h = "";
      int half = min(VAL_HALF_WIDE, (int)dotTextWidth(v, 2, 1) / 2 + 3);
      // Clamp to ring-safe X??[9..119]: CX=64 half=27 -> [37..91] (well inside)
      tft.fillRect(SCREEN_CX - half, QUOTA_VALUE_Y, 2 * half, 15, TFT_BLACK);
      drawDotTextC(v, SCREEN_CX, QUOTA_VALUE_Y, 2, 1, TFT_WHITE);
    }
    return;
  }
  if (force) {
    drawSqTextC("5h", QUOTA_COL1_X, QUOTA_LABEL_Y, 1, 1, TFT_LIGHTGREY);
    drawSqTextC("Wk", QUOTA_COL2_X, QUOTA_LABEL_Y, 1, 1, TFT_LIGHTGREY);
  }
  String v1 = pctText(hourPct), v2 = pctText(weekPct);
  if (force || v1 != lastQuota5h) {
    lastQuota5h = v1;
    int half = min(VAL_HALF_NARROW, (int)dotTextWidth(v1, 2, 1) / 2 + 3);
    // COL1_X=32: left edge = 32 - 23 = 9 (abuts left ring inner edge)
    tft.fillRect(QUOTA_COL1_X - half, QUOTA_VALUE_Y, 2 * half, 15, TFT_BLACK);
    drawDotTextC(v1, QUOTA_COL1_X, QUOTA_VALUE_Y, 2, 1, TFT_WHITE);
  }
  if (force || v2 != lastQuotaWk) {
    lastQuotaWk = v2;
    int half = min(VAL_HALF_NARROW, (int)dotTextWidth(v2, 2, 1) / 2 + 3);
    // COL2_X=96: right edge = 96 + 23 = 119 (abuts right ring inner edge)
    tft.fillRect(QUOTA_COL2_X - half, QUOTA_VALUE_Y, 2 * half, 15, TFT_BLACK);
    drawDotTextC(v2, QUOTA_COL2_X, QUOTA_VALUE_Y, 2, 1, TFT_WHITE);
  }
}

// ---------- quota-exhausted countdown ----------
// When the current app's 5h or weekly window is used up, the pet is replaced
// by a countdown to that window's reset (bridge sends minutes-until-reset).
// A spent weekly window blocks usage even after the 5h one resets, so the
// weekly countdown takes priority when both are exhausted.

enum CdType { CD_NONE, CD_5H, CD_WEEK };

float currentHourPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourPct : codexStatus.primaryPct;
}

int currentHourResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.fiveHourResetMin : codexStatus.primaryResetMin;
}

float currentWeekPct() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayPct : codexStatus.weeklyPct;
}

int currentWeekResetMin() {
  return currentApp == APP_CLAUDE ? claudeStatus.sevenDayResetMin : codexStatus.weeklyResetMin;
}

CdType desiredCountdown() {
  if (currentWeekPct() >= 99.9f && currentWeekResetMin() >= 0) return CD_WEEK;
  if (currentHourPct() >= 99.9f && currentHourResetMin() >= 0) return CD_5H;
  return CD_NONE;
}

CdType showingCd = CD_NONE; // what's on screen now (vs desiredCountdown())
String lastCountdown;

// The bridge only reports whole minutes, so the seconds tick locally against
// a deadline anchored at millis(). Re-anchor only when the bridge disagrees
// by more than ~a minute (new window, big clock drift), otherwise a poll
// landing mid-minute would make the seconds jump around.
unsigned long cdDeadlineMs = 0; // 0 = not anchored
ActiveApp cdApp = APP_CLAUDE;   // which app/window the anchor belongs to
CdType cdAnchorType = CD_NONE;

void syncCountdownDeadline() {
  int m = showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin();
  if (m < 0) {
    cdDeadlineMs = 0;
    return;
  }
  long bridgeSec = (long)m * 60 + 30; // bridge floors to minutes: assume mid-minute
  long ourSec = (long)(cdDeadlineMs - millis()) / 1000;
  if (cdDeadlineMs == 0 || cdApp != currentApp || cdAnchorType != showingCd || ourSec < 0 ||
      labs(ourSec - bridgeSec) > 90) {
    cdDeadlineMs = millis() + (unsigned long)bridgeSec * 1000UL;
    cdApp = currentApp;
    cdAnchorType = showingCd;
  }
}

void drawCountdown(bool force) {
  long remain = cdDeadlineMs ? (long)(cdDeadlineMs - millis()) / 1000
                             : (long)(showingCd == CD_WEEK ? currentWeekResetMin() : currentHourResetMin()) * 60;
  if (remain < 0) remain = 0;
  char buf[16];
  long hours = remain / 3600;
  if (hours >= 100) // weekly can be up to 168h: h:mm:ss wouldn't fit the ring
    snprintf(buf, sizeof(buf), "%ld:%02ld", hours, (remain % 3600) / 60);
  else
    snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", hours, (remain % 3600) / 60, remain % 60);
  String t(buf);
  if (!force && t == lastCountdown) return;
  // A length change ("100:00" -> "99:59:59") shifts every glyph cell, so the
  // whole region must clear; otherwise same-length digits line up 1:1.
  if (t.length() != lastCountdown.length()) force = true;
  const int P = 3, R = 1, VAL_Y = 64; // countdown dot metrics (compact for 128px)
  int x = SCREEN_CX - dotTextWidth(t, P, R) / 2;
  if (force) {
    tft.fillRect(SCREEN_CX - 48, 40, 96, 56, TFT_BLACK);
    drawDotTextC(showingCd == CD_WEEK ? "Wk RESET IN" : "5h RESET IN", SCREEN_CX, 44, 1, 0,
                 TFT_LIGHTGREY);
    drawDotText(t, x, VAL_Y, P, R, TFT_ORANGE);
  } else {
    // Same length = identical cell layout: repaint only the digits that
    // changed, so the once-a-second tick never flashes the whole row.
    for (unsigned int i = 0; i < t.length(); i++) {
      int adv = dotCharAdv(t[i], P, R);
      if (t[i] != lastCountdown[i]) {
        tft.fillRect(x, VAL_Y, adv - P, dotTextHeight(P, R), TFT_BLACK);
        drawDotChar(t[i], x, VAL_Y, P, R, TFT_ORANGE);
      }
      x += adv;
    }
  }
  lastCountdown = t;
}

// App logo in the top-left corner (inside the quota ring) so a glance tells
// which app the screen is currently showing. Drawn row-by-row from PROGMEM
// through rowBuf, 2x downsampled to fit the 128px panel.
const int LOGO_X = 8, LOGO_Y = 10;

void drawAppLogo() {
  const uint16_t *logo = (currentApp == APP_CLAUDE) ? claude_logo_0 : codex_logo_0;
  int w = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_W : CODEX_LOGO_W;
  int h = (currentApp == APP_CLAUDE) ? CLAUDE_LOGO_H : CODEX_LOGO_H;
  const int S = 2;
  int dw = w / S, dh = h / S;
  for (int r = 0; r < dh; r++) {
    memcpy_P(rowBuf, logo + (size_t)(r * S) * w, (size_t)w * 2);
    for (int c = 0; c < dw; c++) rowBuf[c] = rowBuf[c * S];
    tft.pushImage(LOGO_X, LOGO_Y + r, dw, 1, rowBuf);
  }
}

// Days until the weekly window resets, top-right corner inside the ring
// (mirrors the app logo top-left). Weekly only - the 5h window is too short
// for a day count to say anything. Under a day it degrades to hours.
//
// Bounding-box-derived layout (see Experience 100010326): all geometry flows
// from a single right-aligned anchor so the clear rect never overshoots the
// 128px panel. The old RESET_CX=112 put the "RESET" label's right edge at
// ~131px (off-screen); here we compute a safe CX from the widest expected
// label/value pair and keep the clear rect inside that same box.
// Right progress ring (drawSquareRing line 640): x = 128-2-6 = 120..126.
// Reset text's right edge must land BEFORE 120 so it never overlaps the ring.
// Widest pair: half-width 19. Ring inner edge 120 minus 1px safety gap
// minus half-width 19 -> CX = 100. Margin from screen edge = 128-19-100 = 9.
// (= original margin 6 + half-ring-width 3, per user request "move in by
// half the progress bar width").
const int RESET_RIGHT_MARGIN = 9;                 // 128 -> CX: 9px gap to outer edge of text half-width
const int RESET_LABEL_Y = 10, RESET_VALUE_Y = 24;
const int RESET_CX = SCREEN_W - RESET_RIGHT_MARGIN - 19;  // = 100, right edge lands at 119 (inside ring)
// Clear rect sourced from the same anchor: stays inside x = [80..120], i.e.
// abuts the ring without touching it.
const int RESET_CLEAR_LPAD = 20;                  // >= max half-width (19) + 1
const int RESET_CLEAR_W = RESET_CLEAR_LPAD + 19 + 1;  // leftPad + rightHalf + padRight = 40; right edge = 120
const int RESET_CLEAR_H = (RESET_VALUE_Y + 21) - RESET_LABEL_Y + 1;  // labelTop to valueBottom
String lastResetDays;

String resetDaysText(int min) {
  if (min < 0) return "";
  if (min < 1440) return String((min + 59) / 60) + "h";
  return String((min + 1439) / 1440) + "d";
}

void drawResetDays(bool force) {
  String t = resetDaysText(currentWeekResetMin());
  if (!force && t == lastResetDays) return;
  lastResetDays = t;
  // Homogeneous clear rect: same anchor as draw calls below, guaranteed to
  // stay within the panel and only cover the RESET region (no stray erase of
  // the ring or right edge pixels).
  tft.fillRect(RESET_CX - RESET_CLEAR_LPAD, RESET_LABEL_Y,
               RESET_CLEAR_W, RESET_CLEAR_H, TFT_BLACK);
  if (t.length() == 0) return;
  drawTinyBoldText("RESET", RESET_CX, RESET_LABEL_Y, TFT_LIGHTGREY);
  // 2 chars ("3d") get big 3px dots; 3 chars ("18h") drop a size to fit.
  int pitch = t.length() <= 2 ? 3 : 2;
  drawDotTextC(t, RESET_CX, RESET_VALUE_Y, pitch, 1, TFT_WHITE);
}

// Codex's ring percentage: the 5h window when it exists, otherwise the
// weekly one (Codex removed the 5h limit in 2026-07).
float codexRingPct() {
  if (codexStatus.primaryPct >= 0) return codexStatus.primaryPct;
  return max(codexStatus.weeklyPct, 0.0f);
}

// Claude's ring percentage: real 5h OAuth quota from the bridge when known,
// otherwise fall back to elapsed session time as a rough stand-in.
float claudeRingPct() {
  if (claudeStatus.fiveHourPct >= 0) return claudeStatus.fiveHourPct;
  return claudeStatus.sessionWindowMin > 0
             ? (100.0 * claudeStatus.sessionMin / claudeStatus.sessionWindowMin)
             : 0;
}

// Redraws whichever app is currently active, full screen: quota ring +
// sprite (or the reset countdown while the 5h window is exhausted).
// Full clear + repaint - only for real transitions (app switch, mode return,
// sprite change); steady-state data updates go through refreshActiveApp().
void drawActiveApp() {
  tft.fillScreen(TFT_BLACK);
  ringLastPct = -1000; // screen was cleared: force the ring repaint
  showingCd = desiredCountdown();
  if (showingCd != CD_NONE) syncCountdownDeadline();
  else cdDeadlineMs = 0;
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
    if (showingCd == CD_NONE) drawClaudeSprite(claudeFrame);
    drawQuotaText(claudeRingPct(), claudeStatus.sevenDayPct, true);
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
    if (showingCd == CD_NONE) drawCodexSprite(codexFrame);
    drawQuotaText(codexStatus.primaryPct, codexStatus.weeklyPct, true);
  }
  if (showingCd != CD_NONE) drawCountdown(true);
  drawAppLogo();
  drawResetDays(true);
}

// In-place refresh after a bridge poll: ring repaint + only the text that
// actually changed. No fillScreen, so the 5s poll doesn't blank the screen.
void refreshActiveApp() {
  if (desiredCountdown() != showingCd) { // pet <-> countdown (or 5h <-> weekly) swap
    drawActiveApp();
    return;
  }
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
    drawQuotaText(claudeRingPct(), claudeStatus.sevenDayPct, false);
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
    drawQuotaText(codexStatus.primaryPct, codexStatus.weeklyPct, false);
  }
  drawResetDays(false);
  if (showingCd != CD_NONE) {
    syncCountdownDeadline();
    drawCountdown(false);
  }
}

// Redraws just the ring (cheap) - used for status color animation ticks
// between full redraws.
void redrawRingOnly() {
  if (currentApp == APP_CLAUDE) {
    drawSquareRing(claudeRingPct(), currentStatusColor());
  } else {
    drawSquareRing(codexRingPct(), currentStatusColor());
  }
}

// Who gets the screen:
//   - display mode pinned (Mac app) -> that app, always
//   - exactly one app working       -> that app, immediately
//   - both working                  -> alternate every SWITCH_BOTH_MS (2s)
//   - neither working               -> alternate slowly (SWITCH_IDLE_MS)
bool updateActiveApp() {
  ActiveApp desired = currentApp;

  if (displayMode == MODE_CLAUDE) {
    desired = APP_CLAUDE;
  } else if (displayMode == MODE_CODEX) {
    desired = APP_CODEX;
  } else if (claudeStatus.needsInput && !codexStatus.needsInput) {
    desired = APP_CLAUDE; // approval prompt wins the screen
  } else if (codexStatus.needsInput && !claudeStatus.needsInput) {
    desired = APP_CODEX;
  } else {
    bool claudeWorking = claudeStatus.status == "working";
    bool codexWorking = codexStatus.status == "working";
    if (claudeWorking && !codexWorking) {
      desired = APP_CLAUDE;
    } else if (codexWorking && !claudeWorking) {
      desired = APP_CODEX;
    } else {
      unsigned long interval = (claudeWorking && codexWorking) ? SWITCH_BOTH_MS : SWITCH_IDLE_MS;
      if (millis() - lastSwitchMs >= interval) {
        lastSwitchMs = millis();
        desired = (currentApp == APP_CLAUDE) ? APP_CODEX : APP_CLAUDE;
      }
    }
  }

  if (desired != currentApp) {
    currentApp = desired;
    lastSwitchMs = millis();
    // --- Invalidate all cross-module paint caches so the next refresh (or
    // the caller's drawActiveApp) redraws the ring/quota/reset against the
    // NEW app's values. Without this, when e.g. Claude's ring pct happened
    // to equal Codex's, drawSquareRing would short-circuit (see line 626)
    // and leave the ring painted with the old app's track arc -> "??".
    // Also covers the AUTO -> CODEX first-boot scenario where the user
    // reports the Codex progress bar stays partial until they manually pin
    // MODE_CODEX (which unconditionally calls drawActiveApp()).
    ringLastPct = -1000;
    ringLastColor = 1;              // sentinel != valid TFT color
    lastQuota5h = "";               // force drawQuotaText repaint
    lastQuotaWk = "";               //  "
    lastResetDays = "";             // force drawResetDays repaint
    showingCd = CD_NONE;            // force drawActiveApp's cd sync path
    cdDeadlineMs = 0;               //  "
    cdAnchorType = CD_NONE;         //  "
    lastCountdown = "";             //  "
    // Note: drawQuotaText's static lastSingle is internal to that function;
    // clearing lastQuota5h/lastQuotaWk above causes it to run the force=true
    // branch on its next invocation from refreshActiveApp(), which calls
    // fillRect to rebuild the single/dual column layout from scratch.
    return true;
  }
  return false;
}

// ---------- net speed screen ----------

String speedText(long bps) {
  char buf[16];
  if (bps >= 1000000) snprintf(buf, sizeof(buf), "%.1fM", bps / 1000000.0);
  else if (bps >= 1000) snprintf(buf, sizeof(buf), "%.0fK", bps / 1000.0);
  else snprintf(buf, sizeof(buf), "%ldB", bps);
  return String(buf);
}

void resetNetChart() {
  memset(netHistRx, 0, sizeof(netHistRx));
  memset(netHistTx, 0, sizeof(netHistTx));
  netScale = 10240;
  netLastDl = "";
  netLastUl = "";
  netLastScaleText = "";
  netLastCpuVal = "";
  netLastMemVal = "";
  netSysLabelsDrawn = false;
  netQHead = 0;
  netQCount = 0;
  netSeq = -1;
}

// Adaptive full scale: the window's peak always lands at ~87% of the chart
// height, so the undulation stays visible no matter the absolute speed.
// (The old 1/2/5 stepped scale could squash everything to under half height.)
long adaptiveNetScale(long maxV) {
  long s = maxV + maxV / 7; // ~1.15x headroom above the peak
  return s > 10240 ? s : 10240;
}

// Static chrome: labels that never change while in net mode.
void drawNetChrome() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.drawString("DOWN", 8, 4, 1);
  tft.drawString("UP", 68, 4, 1);
  // Scale label: initial full-scale text, drawn DIRECTLY ABOVE the chart so
  // rolling pushImage never covers it. Must match later dynamic repaint
  // gate in drawNetChart() for geometry and anchor.
  tft.setTextDatum(TR_DATUM);
  tft.fillRect(NET_CHART_X, NET_SCALE_LABEL_Y, NET_CHART_W, NET_SCALE_LABEL_H, TFT_BLACK);
  String sc = speedText(netScale);
  netLastScaleText = sc; // prime cache so dynamic gate won't re-paint until value changes
  tft.drawString(sc, NET_CHART_X + NET_CHART_W, NET_SCALE_LABEL_Y, 1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("PC NET  -  30s", SCREEN_CX, 120, 1); // below the CPU/MEM row
}

// Mac CPU / memory usage row between the chart and the footer: small grey
// labels at fixed positions, font-2 values left-aligned at fixed x so a
// width change (5% -> 30%) never shifts the rest of the row around.
// Hidden only if an old bridge doesn't send the fields yet.
const int NET_SYS_Y = 96;                           // row top (18px tall, font 2)
const int NET_CPU_LABEL_X = 8, NET_CPU_VAL_X = 32;  // value region 32..68 ("100%" fits)
const int NET_MEM_LABEL_X = 72, NET_MEM_VAL_X = 96;

void drawNetSysinfoIfChanged() {
  if (netCpuPct < 0) {
    if (netSysLabelsDrawn) { // bridge stopped sending: erase the whole row
      tft.fillRect(0, NET_SYS_Y, SCREEN_W, 18, TFT_BLACK);
      netSysLabelsDrawn = false;
      netLastCpuVal = "";
      netLastMemVal = "";
    }
    return;
  }
  tft.setTextDatum(TL_DATUM);
  if (!netSysLabelsDrawn) {
    netSysLabelsDrawn = true;
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString("CPU", NET_CPU_LABEL_X, NET_SYS_Y + 4, 1);
    tft.drawString("MEM", NET_MEM_LABEL_X, NET_SYS_Y + 4, 1);
  }
  String c = String(netCpuPct) + "%", m = String(netMemPct) + "%";
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (c != netLastCpuVal) {
    netLastCpuVal = c;
    tft.fillRect(NET_CPU_VAL_X, NET_SYS_Y, 40, 18, TFT_BLACK);
    tft.drawString(c, NET_CPU_VAL_X, NET_SYS_Y, 2);
  }
  if (m != netLastMemVal) {
    netLastMemVal = m;
    tft.fillRect(NET_MEM_VAL_X, NET_SYS_Y, 32, 18, TFT_BLACK);
    tft.drawString(m, NET_MEM_VAL_X, NET_SYS_Y, 2);
  }
}

// Header readouts (1s-averaged), each repainted only when its text changes.
// Per experience 100001838: treat overflow on the TEXT NODE (not container).
// Instead of fixed 60/56px clear rects, measure the OLD + NEW string width and
// take the bounding union width. TFT_eSPI font2 cell width = 12px so we can
// just compute width = max(oldLen,newLen)*12 + 2pad. Also right-clamp so the
// clear rect never goes beyond X=127, which was the root cause of the
// trailing "/s" unit character ("s") being left behind when a long upload
// value (e.g. "10.0M/s") was replaced by a shorter one whose fixed clear
// rect didn't reach X=124-128.
static int netFont2CellW() { return 12; }

void drawNetHeaderIfChanged() {
  String dl = speedText(netCurRx) + "/s";
  String ul = speedText(netCurTx) + "/s";
  tft.setTextDatum(TL_DATUM);
  const int CELL_W = 12;
  if (dl != netLastDl) {
    int w = max((int)dl.length(), (int)netLastDl.length()) * CELL_W + 2;
    int x = 8;
    if (x + w > SCREEN_W) w = SCREEN_W - x; // clamp to panel right edge
    tft.fillRect(x, 14, w, 18, TFT_BLACK);
    netLastDl = dl;
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(dl, x, 14, 2);
  }
  if (ul != netLastUl) {
    int w = max((int)ul.length(), (int)netLastUl.length()) * CELL_W + 2;
    int x = 68;
    if (x + w > SCREEN_W) w = SCREEN_W - x; // NEVER let clear overshoot X=127
    tft.fillRect(x, 14, w, 18, TFT_BLACK);
    netLastUl = ul;
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(ul, x, 14, 2);
  }
}

// Repaints the whole chart region from the sample ring, one row at a time
// through rowBuf (a single pushImage per row = no clear-then-draw flicker).
// Download is a dim-green filled area with a bright top edge; upload is a
// 2px yellow line on top; faint gridlines at 25/50/75%.
void drawNetChart() {
  static const uint16_t COL_GRID = swap565(0x2104);   // very dark grey
  static const uint16_t COL_FILL = swap565(0x02A0);   // dim green
  static const uint16_t COL_EDGE = swap565(TFT_GREEN);
  static const uint16_t COL_UL = swap565(TFT_YELLOW);
  static const uint16_t COL_BLACK = swap565(TFT_BLACK);

  long maxV = 0;
  for (int i = 0; i < NET_CHART_W; i++) {
    if (netHistRx[i] > maxV) maxV = netHistRx[i];
    if (netHistTx[i] > maxV) maxV = netHistTx[i];
  }
  netScale = adaptiveNetScale(maxV);

  // Per-column heights (3-tap smoothed), then per-column line "bands": each
  // band spans from the previous column's height to this one's, so steep
  // rises/falls render as connected vertical strokes instead of detached
  // stair-step dots ?? that's what makes the undulation read as a continuous
  // line, like the Mac mirror's stroked polyline.
  static uint8_t hRx[NET_CHART_W], hTx[NET_CHART_W];
  static uint8_t dlLo[NET_CHART_W], dlHi[NET_CHART_W]; // DL edge band, incl. 3px weight
  static uint8_t ulLo[NET_CHART_W], ulHi[NET_CHART_W]; // UL line band
  // The panel is physically tiny (2.7cm across), so the stroke must be much
  // thicker than the Mac mirror's to read at the same visual weight.
  const int LINE_T = 10; // stroke thickness in px
  for (int i = 0; i < NET_CHART_W; i++) {
    int lo = i > 0 ? i - 1 : 0, hi = i < NET_CHART_W - 1 ? i + 1 : NET_CHART_W - 1;
    long rx = (netHistRx[lo] + netHistRx[i] + netHistRx[hi]) / 3;
    long tx = (netHistTx[lo] + netHistTx[i] + netHistTx[hi]) / 3;
    int hr = (int)((float)rx / netScale * (NET_CHART_H - 2));
    int ht = (int)((float)tx / netScale * (NET_CHART_H - 2));
    hRx[i] = (uint8_t)constrain(hr, 0, NET_CHART_H - 1);
    hTx[i] = (uint8_t)constrain(ht, 0, NET_CHART_H - 1);
  }
  for (int i = 0; i < NET_CHART_W; i++) {
    int prevR = i > 0 ? hRx[i - 1] : hRx[0];
    int prevT = i > 0 ? hTx[i - 1] : hTx[0];
    dlHi[i] = (uint8_t)max((int)hRx[i], prevR);
    dlLo[i] = (uint8_t)max(0, min((int)hRx[i], prevR) - (LINE_T - 1));
    ulHi[i] = (uint8_t)max((int)hTx[i], prevT);
    ulLo[i] = (uint8_t)max(0, min((int)hTx[i], prevT) - (LINE_T - 1));
  }

  for (int row = 0; row < NET_CHART_H; row++) {
    int yFromBot = NET_CHART_H - 1 - row;
    bool gridRow = (row == NET_CHART_H / 4 || row == NET_CHART_H / 2 || row == 3 * NET_CHART_H / 4);
    for (int i = 0; i < NET_CHART_W; i++) {
      uint16_t c = gridRow ? COL_GRID : COL_BLACK;
      if (yFromBot <= dlHi[i] && yFromBot >= dlLo[i]) c = COL_EDGE;
      else if (yFromBot < dlLo[i]) c = COL_FILL;
      if (ulHi[i] > 0 && yFromBot <= ulHi[i] && yFromBot >= ulLo[i]) c = COL_UL;
      rowBuf[i] = c;
    }
    tft.pushImage(NET_CHART_X, NET_CHART_Y + row, NET_CHART_W, 1, rowBuf);
    if ((row & 31) == 31) yield();
  }

  // Axis label: adaptive full-scale speed (e.g. "69k") drawn right-aligned
  // ABOVE the chart (NET_SCALE_LABEL_Y = NET_CHART_Y - 3), so it is 100%
  // outside the pushImage row range (row 0 maps to NET_CHART_Y not below).
  // This fixes the user-reported bug where the label lived inside the chart
  // at y=48 and every 250ms the chart's row-14 pushImage overwrote it with
  // black/green -> only visible on mode-entry / scale-switch for ~250ms.
  String scaleText = speedText(netScale);
  if (scaleText != netLastScaleText) {
    netLastScaleText = scaleText;
    tft.fillRect(NET_CHART_X, NET_SCALE_LABEL_Y, NET_CHART_W, NET_SCALE_LABEL_H, TFT_BLACK);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString(scaleText, NET_CHART_X + NET_CHART_W, NET_SCALE_LABEL_Y, 1);
    tft.setTextDatum(TL_DATUM);
  }
}

// Chart tick, every NET_DRAW_INTERVAL_MS: shift in queued sample(s), then
// one atomic repaint. If the queue backs up after a slow poll, it works off
// up to three samples per tick until it's back in step.
void netDrawTick() {
  if (!netChromeDrawn) {
    resetNetChart();
    drawNetChrome();
    netChromeDrawn = true;
    netHeaderDirty = true;
  }
  if (netHeaderDirty) {
    drawNetHeaderIfChanged();
    drawNetSysinfoIfChanged();
    netHeaderDirty = false;
  }
  if (netQCount == 0) return;
  int steps = min(netQCount, netQCount > 16 ? 3 : 1);
  while (steps-- > 0 && netQCount > 0) {
    memmove(netHistRx, netHistRx + 1, sizeof(long) * (NET_CHART_W - 1));
    memmove(netHistTx, netHistTx + 1, sizeof(long) * (NET_CHART_W - 1));
    netHistRx[NET_CHART_W - 1] = netQRx[netQHead];
    netHistTx[NET_CHART_W - 1] = netQTx[netQHead];
    netQHead = (netQHead + 1) % NET_QUEUE;
    netQCount--;
  }
  drawNetChart();
}

// Ingests one /net payload (from HTTP polling or a serial #NET frame) into
// the sample queue. The seq field tells us which samples we've already
// queued, so overlapping tails are fine.
bool handleNetPayload(const String &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  netCurRx = doc["rx_bps"] | 0L;
  netCurTx = doc["tx_bps"] | 0L;
  netCpuPct = doc["cpu_pct"] | -1;
  netMemPct = doc["mem_pct"] | -1;
  netHeaderDirty = true;
  long seq = doc["seq"] | -1L;
  JsonArray rx = doc["rx"], tx = doc["tx"];
  int n = min(rx.size(), tx.size());
  // how many of the tail samples are new to us
  int fresh = (netSeq < 0) ? min(n, 8) : (int)min((long)n, seq - netSeq);
  if (fresh < 0) fresh = 0;
  for (int i = n - fresh; i < n; i++) {
    if (netQCount >= NET_QUEUE) break; // queue full: drop the excess
    int tail = (netQHead + netQCount) % NET_QUEUE;
    netQRx[tail] = rx[i].as<long>();
    netQTx[tail] = tx[i].as<long>();
    netQCount++;
  }
  if (seq >= 0) netSeq = seq;
  return true;
}

// Refills the sample queue from the bridge's /net endpoint.
void pollNet() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/net";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) handleNetPayload(http.getString());
  http.end();
}

String timeText(int sec) {
  if (sec < 0) sec = 0;
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d", sec / 60, sec % 60);
  return String(buf);
}

void drawMusicCoverPlaceholder() {
  const int x = MUSIC_COVER_X;
  const int y = MUSIC_COVER_Y;
  tft.fillRect(x, y, MUSIC_COVER_DW, MUSIC_COVER_DH, TFT_DARKGREY);
  tft.drawRect(x, y, MUSIC_COVER_DW, MUSIC_COVER_DH, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
  tft.drawString("No Art", SCREEN_CX, y + MUSIC_COVER_DH / 2, 1);
}

bool drawMusicCoverFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0 || !musicHasArtwork) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/cover.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const int x = MUSIC_COVER_X;
  const int y = MUSIC_COVER_Y;
  const size_t rowBytes = (size_t)MUSIC_COVER_W * 2;
  bool ok = true;
  // 2x downsample: read a full 128px row, keep every other pixel; display
  // every other row so the 64x64 cover fits the 128px panel.
  for (int r = 0; r < MUSIC_COVER_DH; r++) {
    for (int skip = 0; skip < 2; skip++) {
      int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
      if (got != (int)rowBytes) {
        ok = false;
        break;
      }
      if (skip == 0) {
        for (int c = 0; c < MUSIC_COVER_DW; c++) rowBuf[c] = rowBuf[c * 2];
        tft.pushImage(x, y + r, MUSIC_COVER_DW, 1, rowBuf);
      }
    }
    if (!ok) break;
    yield();
  }
  http.end();
  return ok;
}

// Streams the Mac-rendered 232x44 title/artist strip and blits it row by
// row ?? the only way to get CJK on screen without shipping a font.
bool drawMusicTextFromBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/text.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const size_t rowBytes = (size_t)MUSIC_TEXT_W * 2;
  bool ok = true;
  // 2x downsample: read a full 232px row, keep every other pixel; display
  // every other row so the 116x22 strip fits the 128px panel.
  for (int r = 0; r < MUSIC_TEXT_DH; r++) {
    for (int skip = 0; skip < 2; skip++) {
      int got = stream->readBytes((uint8_t *)rowBuf, rowBytes);
      if (got != (int)rowBytes) {
        ok = false;
        break;
      }
      if (skip == 0) {
        for (int c = 0; c < MUSIC_TEXT_DW; c++) rowBuf[c] = rowBuf[c * 2];
        tft.pushImage(MUSIC_TEXT_X, MUSIC_TEXT_Y + r, MUSIC_TEXT_DW, 1, rowBuf);
      }
    }
    if (!ok) break;
    yield();
  }
  http.end();
  return ok;
}

// ASCII-only fallback if the strip fetch fails (CJK will stay blank, but at
// least latin titles show something).
String fitText(String s, int maxPx, int font) {
  if (tft.textWidth(s, font) <= maxPx) return s;
  while (s.length() > 0 && tft.textWidth(s + "...", font) > maxPx) {
    s.remove(s.length() - 1);
  }
  return s + "...";
}

void drawMusicTextFallback() {
  tft.fillRect(MUSIC_TEXT_X, MUSIC_TEXT_Y, MUSIC_TEXT_DW, MUSIC_TEXT_DH, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String title = musicTitle.length() ? musicTitle : "No Music";
  tft.drawString(fitText(title, 110, 2), SCREEN_CX, MUSIC_TEXT_Y + 2, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(fitText(musicArtist, 110, 2), SCREEN_CX, MUSIC_TEXT_Y + 13, 1);
}

// Regions repaint independently: cover / text strip only when their rev
// changes, progress bar + time on every poll (partial fill, no flicker
// elsewhere).
void drawMusicScreen(bool coverChanged, bool textChanged) {
  if (!musicChromeDrawn) {
    tft.fillScreen(TFT_BLACK);
    coverChanged = true;
    textChanged = true;
    musicChromeDrawn = true;
  }
  if (coverChanged) {
    if (!drawMusicCoverFromBridge()) drawMusicCoverPlaceholder();
  }
  if (textChanged) {
    if (!drawMusicTextFromBridge()) drawMusicTextFallback();
  }

  const int bx = 14, by = 100, bw = 100, bh = 6;
  tft.fillRect(0, by - 2, SCREEN_W, SCREEN_H - by + 2, TFT_BLACK);
  tft.fillRect(bx, by, bw, bh, TFT_DARKGREY);
  float progress = musicDuration > 0 ? (float)musicElapsed / (float)musicDuration : 0;
  if (progress < 0) progress = 0;
  if (progress > 1) progress = 1;
  uint16_t color = musicPlaying ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRect(bx, by, (int)(bw * progress), bh, color);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(timeText(musicElapsed) + " / " + timeText(musicDuration), SCREEN_CX, 116, 1);
}

// ---------- music spectrum drawing ----------
// spectrumBars[] holds 24 magnitudes (0-255). Styles:
//   0 bars          ?? classic bottom-up vertical bars
//   1 mirrored      ?? symmetric bars spreading from the center column
//   2 waveform      ?? connected line across the width
//   3 peak-hold     ?? bars with a decaying peak dot above each bar
//   4 radial        ?? bars radiating from the center
// Palette index 0..7 selected by spectrumColor (see paletteSpectrum()).
uint16_t paletteColor(int idx) {
  switch (idx) {
    case 1: return TFT_CYAN;
    case 2: return TFT_YELLOW;
    case 3: return TFT_ORANGE;
    case 4: return TFT_RED;
    case 5: return TFT_MAGENTA;
    case 6: return TFT_WHITE;
    case 7: return TFT_GREENYELLOW;
    default: return TFT_GREEN;
  }
}
uint16_t paletteSpectrum() { return paletteColor(spectrumColor); }

// Spectrum-by-value: map 0..255 onto a hue sweep (blue??green??yellow??red)
// so the bars glow across the spectrum with the audio magnitude. Used when
// spectrumRainbow is enabled.
uint16_t spectrumRainbowColor(int v) {
  int h = (v * 300) / 255; // 0..300 -> hue
  int sector = h / 60;
  int f = h % 60;
  int r, g, b;
  switch (sector) {
    case 0: r = 255; g = f * 255 / 60; b = 0; break;
    case 1: r = 255 - f * 255 / 60; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = f * 255 / 60; break;
    case 3: r = 0; g = 255 - f * 255 / 60; b = 255; break;
    case 4: r = f * 255 / 60; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - f * 255 / 60; break;
  }
  return tft.color565(r, g, b);
}
void drawSpectrum() {
  if (!spectrumData) {
    tft.fillScreen(TFT_BLACK);
    spectrumFirstDraw = true;
    return;
  }
  const int n = SPECTRUM_BARS;
  const int bw = SCREEN_W / n;              // bar slot width (5 for 24/128)
  const int maxH = SCREEN_H - 12;           // leave a bottom margin
  uint16_t barColor = paletteSpectrum();
  // bar width: spectrumWidth 1..5 ?? 2..bw-1 px (thicker = more solid)
  int barBase = max(2, (bw - 1) * spectrumWidth / 5);
  // per-type fine-tuning: gap narrows the bars (bars type)
  int barGap = spectrumGap;
  int effW = max(1, barBase - barGap);
  // bars-type dynamic range: when spectrumAutoRange is on, normalize each bar
  // to the live [min,max] envelope so the spectrum always fills the panel;
  // spectrumOffset then shifts the whole spectrum up (positive) or down
  // (negative), in percent of maxH.
  int liveMin = 255, liveMax = 0;
  if (spectrumAutoRange) {
    for (int i = 0; i < n; i++) {
      if (spectrumBars[i] < liveMin) liveMin = spectrumBars[i];
      if (spectrumBars[i] > liveMax) liveMax = spectrumBars[i];
    }
    if (liveMax - liveMin < 8) liveMax = liveMin + 8; // avoid div-by-zero
  }
  // normalized magnitude (0..255) with autoRange + offset applied, shared by
  // all types (bars / wave / radial) so the offset & dynamic range work
  // everywhere, not just on the bars.
  auto barV = [&](int i) -> int {
    int v = spectrumBars[i];
    if (spectrumAutoRange) {
      v = (v - liveMin) * 255 / (liveMax - liveMin);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
    }
    v += spectrumOffset * 255 / 100; // ???/????
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
  };
  auto barH = [&](int i) -> int {
    int h = map(barV(i), 0, 255, 2, maxH);
    if (h < 2) h = 2;
    if (h > maxH) h = maxH;
    return h;
  };
  // rainbow modes live inside the color choice now: color 0-7 = solid
  // palette, 8 = ??????? (horizontal sweep across the bars, low??high hue),
  // 9 = ????? (each bar's color follows its magnitude over the live range),
  // 10 = ??????? (each bar is a vertical gradient from bottom hue to top).
  auto colFor = [&](int i) -> uint16_t {
    if (spectrumColor == 8) {
      // ???????: hue sweeps with the bar index (left??right), independent
      // of magnitude ?? a rainbow band across the whole spectrum.
      int v = (i * 255) / (n > 1 ? n - 1 : 1);
      if (spectrumGradReverse) v = 255 - v;
      return spectrumRainbowColor(v);
    }
    if (spectrumColor == 9) {
      // ?????: hue follows each bar's magnitude, mapped over the live
      // adaptive range so the sweep tracks the current audio level.
      float span = spectrumRainbowMax - spectrumRainbowMin;
      if (span <= 0) span = 1;
      int v = (int)((spectrumBars[i] - spectrumRainbowMin) * 255.0f / span);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      if (spectrumGradReverse) v = 255 - v;
      return spectrumRainbowColor(v);
    }
    return barColor;
  };

  // ??????? (color 10): a bar is painted with a fixed bottom??top hue
  // gradient (bottom = low hue, top = high hue). ????? (color 11): the
  // gradient is anchored to the SCREEN, not the bar ?? the hue at any y pixel
  // is fixed (bottom = hue 0, top = hue 255), so every bar shows the same
  // standing gradient; a taller bar (higher magnitude) simply reaches the
  // next gradient color further up. Falls back to a solid color otherwise.
  auto fillBar = [&](int i, int x, int y, int w, int h) {
    if ((spectrumColor == 10 || spectrumColor == 11) && h > 1) {
      const int segs = 8;
      // ????? threshold: the hue sweep occupies only [0 .. gradRange%] of
      // the screen height (from the top); rows below the threshold are
      // painted with the sweep's final hue.
      int thrPx = SCREEN_H * spectrumGradRange / 100;
      if (thrPx < 1) thrPx = 1;
      for (int s = 0; s < segs; s++) {
        int y0 = y + h * s / segs;
        int y1 = y + h * (s + 1) / segs;
        if (y1 <= y0) continue;
        int v;
        if (spectrumColor == 11) {
          // ?????: SCREEN-ANCHORED fixed gradient. Inside the threshold the
          // hue depends only on the pixel row (identical for every bar);
          // beyond it the final hue is used. gradReverse flips the sweep.
          if (y0 < thrPx) {
            int ratio = y0 * 255 / thrPx; // 0 (top) .. 255 (threshold)
            v = spectrumGradReverse ? ratio : 255 - ratio;
          } else {
            v = spectrumGradReverse ? 255 : 0; // past threshold: final hue
          }
        } else {
          // ???????: bar-anchored gradient (bottom ?? top inside the bar)
          v = s * 255 / (segs - 1);
          if (spectrumGradReverse) v = 255 - v;
        }
        tft.fillRect(x, y0, w, y1 - y0, spectrumRainbowColor(v));
      }
    } else {
      tft.fillRect(x, y, w, h, colFor(i));
    }
  };

  // secondary color (?????): 0-7 solid palette, 8 = ??????? (index sweep),
  // 9+ = magnitude sweep (????? / ??????? / ????? all use the live
  // range here since a single color per bar is needed).
  auto col2For = [&](int i) -> uint16_t {
    if (spectrumColor2 == 8) {
      int v = (i * 255) / (n > 1 ? n - 1 : 1);
      return spectrumRainbowColor(v);
    }
    if (spectrumColor2 >= 9) {
      float span = spectrumRainbowMax - spectrumRainbowMin;
      if (span <= 0) span = 1;
      int v = (int)((spectrumBars[i] - spectrumRainbowMin) * 255.0f / span);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      return spectrumRainbowColor(v);
    }
    return paletteColor(spectrumColor2);
  };

  // fill color (????): same rainbow modes as the primary color ?? 0-7 solid
  // palette, 8 = ??????? (index sweep), 9+ = magnitude sweep over the live
  // adaptive range (????? / ??????? / ????? collapse to per-bar colors).
  auto fillColorFor = [&](int i) -> uint16_t {
    if (spectrumFillColor == 8) {
      int v = (i * 255) / (n > 1 ? n - 1 : 1);
      return spectrumRainbowColor(v);
    }
    if (spectrumFillColor >= 9) {
      float span = spectrumRainbowMax - spectrumRainbowMin;
      if (span <= 0) span = 1;
      int v = (int)((spectrumBars[i] - spectrumRainbowMin) * 255.0f / span);
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      return spectrumRainbowColor(v);
    }
    return paletteColor(spectrumFillColor);
  };

  // ???????/????? for wave & radial types: color from a screen row y
  // (vertical sweep), so lines and radial bars show the gradient too.
  auto gradRow = [&](int y) -> uint16_t {
    if (spectrumColor == 10) { // ???????: full 0..255 sweep over the height
      int v = (SCREEN_H - 1 - y) * 255 / (SCREEN_H - 1);
      if (spectrumGradReverse) v = 255 - v;
      return spectrumRainbowColor(v);
    }
    if (spectrumColor == 11) { // ?????: SCREEN-ANCHORED, same as the bars
      // (identical direction & past-threshold hue as fillBar's ?????)
      int thrPx = SCREEN_H * spectrumGradRange / 100;
      if (thrPx < 1) thrPx = 1;
      int v;
      if (y < thrPx) {
        int ratio = y * 255 / thrPx; // 0 (top) .. 255 (threshold)
        v = spectrumGradReverse ? ratio : 255 - ratio;
      } else {
        v = spectrumGradReverse ? 255 : 0; // past threshold: final hue
      }
      return spectrumRainbowColor(v);
    }
    return colFor(0);
  };
  // radial version: color from a radius (inner ?? outer sweep)
  auto gradRad = [&](int r) -> uint16_t {
    int span = max(8, spectrumRingOuter - spectrumRingInner);
    int v = (r - spectrumRingInner) * 255 / span;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (spectrumGradReverse) v = 255 - v;
    return spectrumRainbowColor(v);
  };
  // wave lines: vertical-gradient color when color is 10/11, else per-bar
  auto waveCol = [&](int i, int y) -> uint16_t {
    if (spectrumColor == 10 || spectrumColor == 11) return gradRow(y);
    return colFor(i);
  };
  // radial bars: radius-gradient color when color is 10/11, else per-bar
  auto radCol = [&](int i, int r) -> uint16_t {
    if (spectrumColor == 10 || spectrumColor == 11) return gradRad(r);
    return colFor(i);
  };

  // vertical mirror for bars: with spectrumMirror ON the bar grows
  // symmetrically from the horizontal center line ?? the center line is the
  // spectrum's 0 point, so the bar extends +h/2 up and -h/2 down (oscilloscope
  // style). With it OFF the bar grows up from the bottom as usual.
  auto barM = [&](int i, int x, int w, int h) {
    int prevH = prevSpectrumH[i];
    if (prevH != h) {
      int e = max(prevH, h);
      if (spectrumMirror) {
        int midY = SCREEN_H / 2;
        int e2 = (e + 1) / 2, h2 = (h + 1) / 2;
        tft.fillRect(x, midY - e2, w, e2 * 2, TFT_BLACK); // erase both halves
        fillBar(i, x, midY - h2, w, h2 * 2);              // centered on the axis
      } else {
        tft.fillRect(x, SCREEN_H - e, w, e, TFT_BLACK);   // erase bottom
        fillBar(i, x, SCREEN_H - h, w, h);                // bottom bar
      }
      prevSpectrumH[i] = h;
    }
  };

  // Style changed ?? full repaint from scratch. Include fill & mirror in the
  // key: switching either changes the drawing form (line?fill, bottom?axis),
  // and incremental erasing alone would leave ghost pixels of the old form.
  int styleKey = spectrumType * 1000 + spectrumEffect * 100 + spectrumFill * 10 + spectrumMirror;
  if (spectrumFirstDraw || styleKey != lastSpectrumStyleDrawn) {
    tft.fillScreen(TFT_BLACK);
    spectrumFirstDraw = false;
    lastSpectrumStyleDrawn = styleKey;
    memset(prevSpectrumH, 0, sizeof(prevSpectrumH));
    memset(prevWaveY, 0, sizeof(prevWaveY));
    memset(prevWaveY2, 0, sizeof(prevWaveY2));
    memset(prevPeakY, 0, sizeof(prevPeakY));
    memset(prevRadialR, 0, sizeof(prevRadialR));
  }

  if (spectrumType == 0 && spectrumEffect == 1) {
    // mirrored: left half grows rightward from center, right half leftward.
    // Incremental: erase the previous bars (up to their old height), then
    // draw the new ones ?? never clear the whole panel.
    int col = SCREEN_CX - 1;              // center column (64)
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        int left = min(x, col), right = max(x + bw, col);
        tft.fillRect(left, SCREEN_H - prevH, right - left, prevH, TFT_BLACK);
        if (x < col) {
          fillBar(i, x, SCREEN_H - h, col - x, h);
        } else {
          fillBar(i, col, SCREEN_H - h, x + bw - col, h);
        }
        prevSpectrumH[i] = h;
      }
    }
    return;
  }

  if (spectrumType == 1 && spectrumEffect == 0) {
    // waveform: polyline through bar tops. Incremental: erase the previous
    // segments (3px-thick black line covers the old 1px line even when the
    // slope shifts) then draw the new ones ?? no full-panel repaint, so the
    // moving wave doesn't flicker. With spectrumMirror the CENTER LINE is the
    // spectrum's 0 point: the wave grows upward (+h) and its mirror downward
    // (-h), oscilloscope style; the axis itself is not drawn.
    int midY = SCREEN_H / 2;
    auto wy = [&](int i) -> int {
      return spectrumMirror
               ? midY - map(barV(i), 0, 255, 2, midY - 2)
               : SCREEN_H - map(barV(i), 0, 255, 2, maxH);
    };
    if (spectrumFill) {
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int y1 = wy(i), y2 = wy(i + 1);
        int yTop = min(y1, y2);
        tft.fillRect(x1, 0, x2 - x1, SCREEN_H, TFT_BLACK); // clear column
        if (spectrumMirror) {
          // fill from the axis upward (positive) and downward (mirror)
          tft.fillRect(x1, yTop, x2 - x1, midY - yTop, fillColorFor(i));
          tft.fillRect(x1, midY, x2 - x1, midY - yTop, fillColorFor(i));
        } else {
          tft.fillRect(x1, yTop, x2 - x1, SCREEN_H - yTop, fillColorFor(i));
        }
      }
    } else {
      // erase old wave segments (thicker black line guarantees full coverage)
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int p1 = prevWaveY[i], p2 = prevWaveY[i + 1];
        tft.drawLine(x1, p1 - 1, x2, p2 - 1, TFT_BLACK);
        tft.drawLine(x1, p1, x2, p2, TFT_BLACK);
        tft.drawLine(x1, p1 + 1, x2, p2 + 1, TFT_BLACK);
        if (spectrumMirror) {
          int m1 = 2 * midY - p1, m2 = 2 * midY - p2;
          tft.drawLine(x1, m1 - 1, x2, m2 - 1, TFT_BLACK);
          tft.drawLine(x1, m1, x2, m2, TFT_BLACK);
          tft.drawLine(x1, m1 + 1, x2, m2 + 1, TFT_BLACK);
        }
      }
    }
    // draw the new wave (and its downward mirror when enabled)
    for (int i = 0; i < n - 1; i++) {
      int x1 = 2 + i * bw;
      int x2 = 2 + (i + 1) * bw;
      int y1 = wy(i), y2 = wy(i + 1);
      for (int t = 0; t < spectrumLineW; t++) {
        tft.drawLine(x1, y1 + t, x2, y2 + t, waveCol(i, y1)); // rainbow-aware
        if (spectrumMirror) {
          tft.drawLine(x1, 2 * midY - y1 - t, x2, 2 * midY - y2 - t, waveCol(i, 2 * midY - y1));
        }
      }
      prevWaveY[i] = y1;
      prevWaveY[i + 1] = y2;
    }
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 2) {
    // peak-hold: bars + decaying peak dots; erase old bar+dot, draw new.
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      // decay speed: spectrumDecay 1..10 (higher = falls faster)
      if (spectrumBars[i] > spectrumPeaks[i]) spectrumPeaks[i] = spectrumBars[i];
      else spectrumPeaks[i] = (spectrumPeaks[i] * (11 - spectrumDecay)) / 10;
      int px = 2 + i * bw + bw / 2;
      int py = SCREEN_H - map(spectrumPeaks[i], 0, 255, 2, maxH);
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        tft.fillRect(x, SCREEN_H - max(prevH, h), effW, max(prevH, h), TFT_BLACK);
        barM(i, x, effW, h);
        prevSpectrumH[i] = h;
      }
      // peak dot only when spectrumPeak is enabled (style 3 = peak-hold)
      if (spectrumPeak) {
        tft.fillRect(px - 1, prevPeakY[i] - 1, 3, 3, TFT_BLACK);
        tft.fillRect(px - 1, py - 1, 3, 3, TFT_YELLOW);
        prevPeakY[i] = py;
      }
    }
    return;
  }

  if (spectrumType == 2 && spectrumEffect == 0) {
    // radial: trapezoid slices. Incremental: erase old slice (black
    // triangles from the previous outer radius) then draw the new one, so the
    // rotating ring doesn't flicker. With spectrumDualRing each slice is
    // drawn as an inner ring [ringInner, r1] plus an outer ring [r1, r2],
    // like the dedicated ??? style.
    int cx = SCREEN_CX, cy = SCREEN_CX;
    float step = 360.0f / n;
    float halfW = step * 0.35f * spectrumRingW / 2.0f;      // half angle (deg)
    float gapDeg = spectrumRingGap * 0.4f;                   // inter-slice gap
    int rSpan = max(8, spectrumRingOuter - spectrumRingInner);
    for (int i = 0; i < n; i++) {
      float a0 = (i * step + gapDeg) * 0.0174533f;
      float a1 = (i * step + gapDeg + halfW * 2) * 0.0174533f;
      int r1 = spectrumRingInner + map(barV(i), 0, 255, 4,
                                       spectrumDualRing ? rSpan * spectrumDualInner / 100 : rSpan);
      // dual-ring mirrors the dedicated ??? style: each slice becomes a
      // spoke from r1 (inner tip) to r2 (outer tip), both following the
      // magnitude. dualInner scales r1's sweep, dualOuter scales r2's reach.
      int r2 = spectrumDualRing ? r1 + map(barV(i), 0, 255, 6, 24 * spectrumDualOuter / 100) : r1;
      // keep the outer ring inside the panel (center 64 ?? max radius 63)
      if (spectrumDualRing && r2 > SCREEN_CX - 1) r2 = SCREEN_CX - 1;
      int outer = spectrumDualRing ? r2 : r1;
      int rIn = spectrumDualRing ? r1 : spectrumRingInner;
      int pr1 = spectrumRingInner + prevRadialR[i];      // old outer radius
      int prIn = spectrumRingInner + prevRadialInner[i]; // old inner radius
      int x0 = cx + (int)(cosf(a0) * spectrumRingInner);
      int y0 = cy + (int)(sinf(a0) * spectrumRingInner);
      int x1 = cx + (int)(cosf(a1) * spectrumRingInner);
      int y1 = cy + (int)(sinf(a1) * spectrumRingInner);
      // erase old slice: compare BOTH radii (dual-ring inner moves too) and
      // widen the erase area by 2px so triangle edges leave no fringe.
      if (pr1 != outer || prIn != rIn) {
        int eIn = min(prIn, rIn) - 2;
        int eOut = max(pr1, outer) + 2;
        int px2 = cx + (int)(cosf(a1) * eOut);
        int py2 = cy + (int)(sinf(a1) * eOut);
        int px3 = cx + (int)(cosf(a0) * eOut);
        int py3 = cy + (int)(sinf(a0) * eOut);
        int qx0 = cx + (int)(cosf(a0) * eIn);
        int qy0 = cy + (int)(sinf(a0) * eIn);
        int qx1 = cx + (int)(cosf(a1) * eIn);
        int qy1 = cy + (int)(sinf(a1) * eIn);
        tft.fillTriangle(qx0, qy0, qx1, qy1, px2, py2, TFT_BLACK);
        tft.fillTriangle(qx0, qy0, px2, py2, px3, py3, TFT_BLACK);
      }
      // draw slice: normal = [ringInner, r1]; dual-ring = spoke [r1, r2]
      // (inner tip r1 and outer tip r2 both follow the magnitude, exactly
      // like the dedicated ??? effect).
      int xa = cx + (int)(cosf(a0) * rIn);
      int ya = cy + (int)(sinf(a0) * rIn);
      int xb = cx + (int)(cosf(a1) * rIn);
      int yb = cy + (int)(sinf(a1) * rIn);
      int xc = cx + (int)(cosf(a1) * outer);
      int yc = cy + (int)(sinf(a1) * outer);
      int xd = cx + (int)(cosf(a0) * outer);
      int yd = cy + (int)(sinf(a0) * outer);
      tft.fillTriangle(xa, ya, xb, yb, xc, yc, radCol(i, outer));
      tft.fillTriangle(xa, ya, xc, yc, xd, yd, radCol(i, outer));
      prevRadialR[i] = outer - spectrumRingInner;
      prevRadialInner[i] = rIn - spectrumRingInner;
    }
    if (spectrumRingInColor != 9) // 9 = off: skip the inner ring
      tft.drawCircle(cx, cy, spectrumRingInner,
                     spectrumRingInColor == 8 ? TFT_BLACK : paletteColor(spectrumRingInColor));
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 3) {
    // twin: two thin bars per slot (main + half-height companion)
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int h2 = barH(i) / 2;
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        int eraseH = max(prevH, h);
        tft.fillRect(x, SCREEN_H - eraseH, effW * 2, eraseH, TFT_BLACK);
        barM(i, x, effW, h);
        tft.fillRect(x + effW + 1, SCREEN_H - h2, effW, h2, TFT_DARKGREY);
        prevSpectrumH[i] = h;
      }
    }
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 4) {
    // dotted: bar height shown as a column of 2x2 dots. With spectrumMirror
    // the dot columns grow symmetrically from the center line (0 point).
    int midY = SCREEN_H / 2;
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int x = 2 + i * bw + 1;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        if (spectrumMirror) {
          int h2 = (h + 1) / 2;
          int e2 = max(prevH, h) / 2 + 3;
          tft.fillRect(x, midY - e2, effW, e2 * 2, TFT_BLACK); // erase both halves
          for (int y = midY - 2; y > midY - h2; y -= 4) fillBar(i, x, y - 2, effW, 2);
          for (int y = midY + 2; y < midY + h2; y += 4) fillBar(i, x, y - 2, effW, 2);
        } else {
          int eraseH = max(prevH, h) + 3;
          tft.fillRect(x, SCREEN_H - eraseH, effW, eraseH, TFT_BLACK);
          for (int y = SCREEN_H - 2; y > SCREEN_H - h; y -= 4) {
            fillBar(i, x, y - 2, effW, 2);
          }
        }
        prevSpectrumH[i] = h;
      }
    }
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 5) {
    // glow: bar with a bright 2px cap and darker body. With spectrumMirror
    // the bar grows symmetrically from the center line (0 point).
    int midY = SCREEN_H / 2;
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        if (spectrumMirror) {
          int h2 = (h + 1) / 2;
          int e2 = max(prevH, h) / 2 + 3;
          tft.fillRect(x, midY - e2, effW, e2 * 2, TFT_BLACK); // erase both halves
          tft.fillRect(x, midY - h2, effW, h2 * 2 - 2, TFT_DARKGREY);
          fillBar(i, x, midY - h2, effW, 2);        // top cap
          fillBar(i, x, midY + h2 - 2, effW, 2);    // bottom cap (mirror)
        } else {
          int eraseH = max(prevH, h) + 3;
          tft.fillRect(x, SCREEN_H - eraseH, effW, eraseH, TFT_BLACK);
          tft.fillRect(x, SCREEN_H - h, effW, h - 2, TFT_DARKGREY);
          fillBar(i, x, SCREEN_H - h, effW, 2);
        }
        prevSpectrumH[i] = h;
      }
    }
    return;
  }

  if (spectrumType == 1 && spectrumEffect == 1) {
    // mirror-wave: two mirrored polylines around the vertical center.
    // Incremental: erase old segments (thick black line) then draw new ones.
    int midY = SCREEN_H / 2;
    if (spectrumFill) {
      // fill toward the center line: clear each column, then repaint fill
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int y1 = midY - map(barV(i), 0, 255, 2, midY - 6);
        int y2 = midY - map(barV(i + 1), 0, 255, 2, midY - 6);
        int yTop = min(y1, y2);
        tft.fillRect(x1, 0, x2 - x1, SCREEN_H, TFT_BLACK); // clear column
        tft.fillRect(x1, yTop, x2 - x1, midY - yTop, fillColorFor(i));
        tft.fillRect(x1, midY, x2 - x1, midY - yTop, fillColorFor(i));
      }
    } else {
      // erase old mirrored segments
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int p1 = prevWaveY[i], p2 = prevWaveY[i + 1];
        tft.drawLine(x1, p1 - 1, x2, p2 - 1, TFT_BLACK);
        tft.drawLine(x1, p1, x2, p2, TFT_BLACK);
        tft.drawLine(x1, p1 + 1, x2, p2 + 1, TFT_BLACK);
        tft.drawLine(x1, 2 * midY - p1 - 1, x2, 2 * midY - p2 - 1, TFT_BLACK);
        tft.drawLine(x1, 2 * midY - p1, x2, 2 * midY - p2, TFT_BLACK);
        tft.drawLine(x1, 2 * midY - p1 + 1, x2, 2 * midY - p2 + 1, TFT_BLACK);
      }
    }
    // draw the new mirrored wave
    for (int i = 0; i < n - 1; i++) {
      int x1 = 2 + i * bw;
      int x2 = 2 + (i + 1) * bw;
      int y1 = midY - map(barV(i), 0, 255, 2, midY - 6);
      int y2 = midY - map(barV(i + 1), 0, 255, 2, midY - 6);
      for (int t = 0; t < spectrumLineW; t++) {
        tft.drawLine(x1, y1 + t, x2, y2 + t, waveCol(i, y1));       // rainbow-aware
        tft.drawLine(x1, 2 * midY - y1 - t, x2, 2 * midY - y2 - t, waveCol(i, 2 * midY - y1));
      }
      prevWaveY[i] = y1;
      prevWaveY[i + 1] = y2;
    }
    tft.drawLine(2, midY, 2 + (n - 1) * bw, midY, TFT_DARKGREY);
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 6) {
    // fire: three-tier gradient bar (red bottom, orange mid, yellow top)
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        int eraseH = max(prevH, h);
        tft.fillRect(x, SCREEN_H - eraseH, effW, eraseH, TFT_BLACK);
        int red = h * 2 / 3, orange = h / 3;
        tft.fillRect(x, SCREEN_H - red, effW, red, TFT_RED);
        tft.fillRect(x, SCREEN_H - red, effW, orange, TFT_ORANGE);
        tft.fillRect(x, SCREEN_H - h, effW, h - red, TFT_YELLOW);
        prevSpectrumH[i] = h;
      }
    }
    return;
  }

  if (spectrumType == 1 && spectrumEffect == 2) {
    // aurora: a bright polyline plus a dimmer echo above it.
    // Incremental: erase old segments then draw new ones. With spectrumMirror
    // the center line is the 0 point: aurora grows up and mirrors down.
    int midY = SCREEN_H / 2;
    auto wy = [&](int i) -> int {
      return spectrumMirror
               ? midY - map(barV(i), 0, 255, 2, midY - 2)
               : SCREEN_H - map(barV(i), 0, 255, 2, maxH);
    };
    if (spectrumFill) {
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int y1 = wy(i), y2 = wy(i + 1);
        int yTop = min(y1, y2);
        tft.fillRect(x1, 0, x2 - x1, SCREEN_H, TFT_BLACK); // clear column
        if (spectrumMirror) {
          tft.fillRect(x1, yTop, x2 - x1, midY - yTop, fillColorFor(i));
          tft.fillRect(x1, midY, x2 - x1, midY - yTop, fillColorFor(i));
        } else {
          tft.fillRect(x1, yTop, x2 - x1, SCREEN_H - yTop, fillColorFor(i));
        }
      }
    } else {
      // erase old main + echo segments (and their mirrors)
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int p1 = prevWaveY[i], p2 = prevWaveY[i + 1];
        tft.drawLine(x1, p1 - 7, x2, p2 - 7, TFT_BLACK);   // echo
        tft.drawLine(x1, p1 - 6, x2, p2 - 6, TFT_BLACK);
        tft.drawLine(x1, p1 - 5, x2, p2 - 5, TFT_BLACK);
        tft.drawLine(x1, p1 - 1, x2, p2 - 1, TFT_BLACK);
        tft.drawLine(x1, p1, x2, p2, TFT_BLACK);
        tft.drawLine(x1, p1 + 1, x2, p2 + 1, TFT_BLACK);
        if (spectrumMirror) {
          int m1 = 2 * midY - p1, m2 = 2 * midY - p2;
          tft.drawLine(x1, m1 - 7, x2, m2 - 7, TFT_BLACK); // mirror echo
          tft.drawLine(x1, m1 - 6, x2, m2 - 6, TFT_BLACK);
          tft.drawLine(x1, m1 - 5, x2, m2 - 5, TFT_BLACK);
          tft.drawLine(x1, m1 - 1, x2, m2 - 1, TFT_BLACK);
          tft.drawLine(x1, m1, x2, m2, TFT_BLACK);
          tft.drawLine(x1, m1 + 1, x2, m2 + 1, TFT_BLACK);
        }
      }
    }
    // draw the new aurora (and its downward mirror)
    for (int i = 0; i < n - 1; i++) {
      int x1 = 2 + i * bw;
      int x2 = 2 + (i + 1) * bw;
      int y1 = wy(i), y2 = wy(i + 1);
      tft.drawLine(x1, y1 - 6, x2, y2 - 6, TFT_DARKGREY);   // echo
      for (int t = 0; t < spectrumLineW; t++) {
        tft.drawLine(x1, y1 + t, x2, y2 + t, waveCol(i, y1));      // main, rainbow-aware
      }
      if (spectrumMirror) {
        int m1 = 2 * midY - y1, m2 = 2 * midY - y2;
        tft.drawLine(x1, m1 - 6, x2, m2 - 6, TFT_DARKGREY); // mirror echo
        for (int t = 0; t < spectrumLineW; t++) {
          tft.drawLine(x1, m1 + t, x2, m2 + t, waveCol(i, m1)); // mirror main
        }
      }
      prevWaveY[i] = y1;
      prevWaveY[i + 1] = y2;
    }
    return;
  }

  if (spectrumType == 0 && spectrumEffect == 8) {
    // starry (bars type): dotted bars with a random bright twinkle. With
    // spectrumMirror the dot columns grow symmetrically from the center line.
    int midY = SCREEN_H / 2;
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      int x = 2 + i * bw + 1;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        if (spectrumMirror) {
          int h2 = (h + 1) / 2;
          int e2 = max(prevH, h) / 2 + 3;
          tft.fillRect(x, midY - e2, effW, e2 * 2, TFT_BLACK); // erase both halves
          for (int y = midY - 2; y > midY - h2; y -= 4) fillBar(i, x, y - 2, effW, 2);
          for (int y = midY + 2; y < midY + h2; y += 4) fillBar(i, x, y - 2, effW, 2);
        } else {
          int eraseH = max(prevH, h) + 3;
          tft.fillRect(x, SCREEN_H - eraseH, effW, eraseH, TFT_BLACK);
          for (int y = SCREEN_H - 2; y > SCREEN_H - h; y -= 4) {
            fillBar(i, x, y - 2, effW, 2);
          }
        }
        prevSpectrumH[i] = h;
      }
      // twinkle: occasionally light pixels just beyond both bar tips
      if (random(100) < 30 && h > 6) {
        tft.drawPixel(x + effW / 2, (spectrumMirror ? midY - h / 2 : SCREEN_H) - h / 2 - 3, TFT_WHITE);
        if (spectrumMirror)
          tft.drawPixel(x + effW / 2, midY + h / 2 + 3, TFT_WHITE);
      }
    }
    return;
  }

  // ---- combo styles (full repaint; wave parts would leave trails
  // with incremental drawing) ----
  if (spectrumType == 0 && spectrumEffect == 7) {
    // bars+wave: classic bars (primary color) with a polyline (color2)
    tft.fillScreen(TFT_BLACK);
    for (int i = 0; i < n; i++) {
      int h = barH(i);
      barM(i, 2 + i * bw, effW, h);
    }
    for (int i = 0; i < n - 1; i++) {
      int x1 = 2 + i * bw;
      int x2 = 2 + (i + 1) * bw;
      int y1 = SCREEN_H - map(barV(i), 0, 255, 2, maxH);
      int y2 = SCREEN_H - map(barV(i + 1), 0, 255, 2, maxH);
      tft.drawLine(x1, y1, x2, y2, col2For(i)); // rainbow-aware secondary
    }
    return;
  }

  if (spectrumType == 1 && spectrumEffect == 3) {
    // wave+bars: half-height bars (color2) under a bright wave (primary).
    // Incremental: erase old bars + wave segments, then draw new ones. With
    // spectrumMirror the CENTER LINE is the 0 point: bars/wave grow up (+h)
    // and mirror down (-h), oscilloscope style.
    int midY = SCREEN_H / 2;
    auto wy = [&](int i) -> int {
      return spectrumMirror
               ? midY - map(barV(i), 0, 255, 2, midY - 2)
               : SCREEN_H - map(barV(i), 0, 255, 2, maxH);
    };
    if (spectrumFill) {
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int y1 = wy(i), y2 = wy(i + 1);
        int yTop = min(y1, y2);
        tft.fillRect(x1, 0, x2 - x1, SCREEN_H, TFT_BLACK); // clear column
        if (spectrumMirror) {
          tft.fillRect(x1, yTop, x2 - x1, midY - yTop, fillColorFor(i));
          tft.fillRect(x1, midY, x2 - x1, midY - yTop, fillColorFor(i));
        } else {
          tft.fillRect(x1, yTop, x2 - x1, SCREEN_H - yTop, fillColorFor(i));
        }
      }
    } else {
      // erase old wave segments (and mirrors)
      for (int i = 0; i < n - 1; i++) {
        int x1 = 2 + i * bw;
        int x2 = 2 + (i + 1) * bw;
        int p1 = prevWaveY[i], p2 = prevWaveY[i + 1];
        tft.drawLine(x1, p1 - 1, x2, p2 - 1, TFT_BLACK);
        tft.drawLine(x1, p1, x2, p2, TFT_BLACK);
        tft.drawLine(x1, p1 + 1, x2, p2 + 1, TFT_BLACK);
        if (spectrumMirror) {
          int m1 = 2 * midY - p1, m2 = 2 * midY - p2;
          tft.drawLine(x1, m1 - 1, x2, m2 - 1, TFT_BLACK);
          tft.drawLine(x1, m1, x2, m2, TFT_BLACK);
          tft.drawLine(x1, m1 + 1, x2, m2 + 1, TFT_BLACK);
        }
      }
    }
    // half-height bars: erase old bar, draw new (centered on axis when mirror)
    for (int i = 0; i < n; i++) {
      int h = barH(i) / 2;
      int x = 2 + i * bw;
      int prevH = prevSpectrumH[i];
      if (prevH != h) {
        int e = max(prevH, h);
        if (spectrumMirror) {
          int e2 = (e + 1) / 2, h2 = (h + 1) / 2;
          tft.fillRect(x, midY - e2, effW, e2 * 2, TFT_BLACK);
          tft.fillRect(x, midY - h2, effW, h2 * 2, col2For(i)); // rainbow-aware
        } else {
          tft.fillRect(x, SCREEN_H - e, effW, e, TFT_BLACK);
          tft.fillRect(x, SCREEN_H - h, effW, h, col2For(i)); // rainbow-aware
        }
        prevSpectrumH[i] = h;
      }
    }
    // draw the new wave (and its downward mirror)
    for (int i = 0; i < n - 1; i++) {
      int x1 = 2 + i * bw;
      int x2 = 2 + (i + 1) * bw;
      int y1 = wy(i), y2 = wy(i + 1);
      for (int t = 0; t < spectrumLineW; t++) {
        tft.drawLine(x1, y1 + t, x2, y2 + t, waveCol(i, y1)); // rainbow-aware
        if (spectrumMirror) {
          tft.drawLine(x1, 2 * midY - y1 - t, x2, 2 * midY - y2 - t, waveCol(i, 2 * midY - y1));
        }
      }
      prevWaveY[i] = y1;
      prevWaveY[i + 1] = y2;
    }
    return;
  }

  if (spectrumType == 2 && spectrumEffect == 1) {
    // double-ring radial: two concentric radiating rings. Incremental:
    // erase the old spoke, then draw the new one.
    int cx = SCREEN_CX, cy = SCREEN_CX;
    int rSpan = max(8, spectrumRingOuter - spectrumRingInner);
    for (int i = 0; i < n; i++) {
      float ang = (i * 360.0f / n) * 0.0174533f;
      int r1 = spectrumRingInner + map(barV(i), 0, 255, 4, rSpan);
      int r2 = r1 + map(barV(i), 0, 255, 6, 24);
      int x0 = cx + (int)(cosf(ang) * r1);
      int y0 = cy + (int)(sinf(ang) * r1);
      int x1 = cx + (int)(cosf(ang) * r2);
      int y1 = cy + (int)(sinf(ang) * r2);
      int pr1 = spectrumRingInner + prevRadialR[i];
      if (pr1 != r1) {
        int px1 = cx + (int)(cosf(ang) * pr1);
        int py1 = cy + (int)(sinf(ang) * pr1);
        tft.drawLine(x0, y0, px1, py1, TFT_BLACK); // erase old spoke
      }
      tft.drawLine(x0, y0, x1, y1, radCol(i, r2));
      prevRadialR[i] = r1 - spectrumRingInner;
    }
    if (spectrumRingInColor != 9) // 9 = off: skip the inner ring
      tft.drawCircle(cx, cy, spectrumRingInner,
                     spectrumRingInColor == 8 ? TFT_BLACK : paletteColor(spectrumRingInColor));
    return;
  }

  if (spectrumType == 2 && spectrumEffect == 2) {
    // ring polyline: bar tips connected into a closed, wavy continuous line,
    // with the band between the inner circle and the line filled. Incremental:
    // erase old segments (thick black line), then draw new ones.
    int cx = SCREEN_CX, cy = SCREEN_CX;
    int rIn = spectrumRingInner;
    int rSpan = max(8, spectrumRingOuter - spectrumRingInner);
    int px[n], py[n];
    for (int i = 0; i < n; i++) {
      float ang = (i * 360.0f / n) * 0.0174533f;
      int r = rIn + map(barV(i), 0, 255, 4, rSpan);
      px[i] = cx + (int)(cosf(ang) * r);
      py[i] = cy + (int)(sinf(ang) * r);
    }
    // erase old wavy ring (thick black line covers the old segments)
    for (int i = 0; i < n; i++) {
      int j = (i + 1) % n;
      int pr1 = rIn + prevRadialR[i];
      int pr2 = rIn + prevRadialR[j];
      int qx1 = cx + (int)(cosf((i * 360.0f / n) * 0.0174533f) * pr1);
      int qy1 = cy + (int)(sinf((i * 360.0f / n) * 0.0174533f) * pr1);
      int qx2 = cx + (int)(cosf((j * 360.0f / n) * 0.0174533f) * pr2);
      int qy2 = cy + (int)(sinf((j * 360.0f / n) * 0.0174533f) * pr2);
      tft.drawLine(qx1 - 1, qy1, qx2 - 1, qy2, TFT_BLACK);
      tft.drawLine(qx1, qy1, qx2, qy2, TFT_BLACK);
      tft.drawLine(qx1 + 1, qy1, qx2 + 1, qy2, TFT_BLACK);
    }
    // filled ribbon: triangle per segment from the inner circle to the line
    if (spectrumRingFill) {
      for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ang = (i * 360.0f / n) * 0.0174533f;
        int ix = cx + (int)(cosf(ang) * rIn);
        int iy = cy + (int)(sinf(ang) * rIn);
        tft.fillTriangle(ix, iy, px[i], py[i], px[j], py[j], fillColorFor(i));
      }
    }
    // the wavy closed line itself
    for (int t = 0; t < spectrumRingW; t++) {
      for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        tft.drawLine(px[i], py[i], px[j], py[j], waveCol(i, py[i]));
      }
    }
    for (int i = 0; i < n; i++) {
      float ang = (i * 360.0f / n) * 0.0174533f;
      int r = rIn + map(barV(i), 0, 255, 4, rSpan);
      prevRadialR[i] = r - rIn;
    }
    if (spectrumRingInColor != 9) // 9 = off: skip the inner ring
      tft.drawCircle(cx, cy, rIn,
                     spectrumRingInColor == 8 ? TFT_BLACK : paletteColor(spectrumRingInColor));
    return;
  }

  if (spectrumType == 2 && spectrumEffect == 3) {
    // fan: each spectrum line becomes a small pie slice from the inner to
    // the outer radius (quad approximated by two triangles). Incremental:
    // erase the old slice (black triangles), then draw the new one. With
    // spectrumDualRing each slice is inner [rIn, r1] + outer [r1, r2].
    int cx = SCREEN_CX, cy = SCREEN_CX;
    int rIn = spectrumRingInner;
    int rSpan = max(8, spectrumRingOuter - spectrumRingInner);
    float step = 360.0f / n;
    for (int i = 0; i < n; i++) {
      float a0 = (i * step) * 0.0174533f;
      float a1 = ((i + 1) * step) * 0.0174533f;
      int r = rIn + map(barV(i), 0, 255, 4,
                        spectrumDualRing ? rSpan * spectrumDualInner / 100 : rSpan);
      // dual-ring mirrors the dedicated ??? style: each slice becomes a
      // spoke from r (inner tip) to r2 (outer tip), both following the
      // magnitude. dualInner scales r's sweep, dualOuter scales r2's reach.
      int r2 = spectrumDualRing ? r + map(barV(i), 0, 255, 6, 24 * spectrumDualOuter / 100) : r;
      // keep the outer ring inside the panel (center 64 ?? max radius 63)
      if (spectrumDualRing && r2 > SCREEN_CX - 1) r2 = SCREEN_CX - 1;
      int outer = spectrumDualRing ? r2 : r;
      int rIn2 = spectrumDualRing ? r : rIn;
      int pr = rIn + prevRadialR[i];      // old outer radius
      int prIn = rIn + prevRadialInner[i]; // old inner radius
      int x0 = cx + (int)(cosf(a0) * rIn);
      int y0 = cy + (int)(sinf(a0) * rIn);
      int x1 = cx + (int)(cosf(a1) * rIn);
      int y1 = cy + (int)(sinf(a1) * rIn);
      // erase old slice: compare BOTH radii (dual-ring inner moves too) and
      // widen the erase area by 2px so triangle edges leave no fringe.
      if (pr != outer || prIn != rIn2) {
        int eIn = min(prIn, rIn2) - 2;
        int eOut = max(pr, outer) + 2;
        int px2 = cx + (int)(cosf(a1) * eOut);
        int py2 = cy + (int)(sinf(a1) * eOut);
        int px3 = cx + (int)(cosf(a0) * eOut);
        int py3 = cy + (int)(sinf(a0) * eOut);
        int qx0 = cx + (int)(cosf(a0) * eIn);
        int qy0 = cy + (int)(sinf(a0) * eIn);
        int qx1 = cx + (int)(cosf(a1) * eIn);
        int qy1 = cy + (int)(sinf(a1) * eIn);
        tft.fillTriangle(qx0, qy0, qx1, qy1, px2, py2, TFT_BLACK);
        tft.fillTriangle(qx0, qy0, px2, py2, px3, py3, TFT_BLACK);
      }
      // draw slice: normal = [rIn, r]; dual-ring = spoke [r, r2]
      // (inner tip r and outer tip r2 both follow the magnitude, exactly
      // like the dedicated ??? effect).
      int xa = cx + (int)(cosf(a0) * rIn2);
      int ya = cy + (int)(sinf(a0) * rIn2);
      int xb = cx + (int)(cosf(a1) * rIn2);
      int yb = cy + (int)(sinf(a1) * rIn2);
      int xc = cx + (int)(cosf(a1) * outer);
      int yc = cy + (int)(sinf(a1) * outer);
      int xd = cx + (int)(cosf(a0) * outer);
      int yd = cy + (int)(sinf(a0) * outer);
      tft.fillTriangle(xa, ya, xb, yb, xc, yc, radCol(i, outer));
      tft.fillTriangle(xa, ya, xc, yc, xd, yd, radCol(i, outer));
      prevRadialR[i] = outer - rIn;
      prevRadialInner[i] = rIn2 - rIn;
    }
    if (spectrumRingInColor != 9) // 9 = off: skip the inner ring
      tft.drawCircle(cx, cy, rIn,
                     spectrumRingInColor == 8 ? TFT_BLACK : paletteColor(spectrumRingInColor));
    return;
  }

  // style 0 (default): classic bottom-up bars; erase old, draw new.
  for (int i = 0; i < n; i++) {
    int h = barH(i);
    int x = 2 + i * bw;
    int prevH = prevSpectrumH[i];
    if (prevH != h) {
      tft.fillRect(x, SCREEN_H - max(prevH, h), effW, max(prevH, h), TFT_BLACK);
      barM(i, x, effW, h);
      prevSpectrumH[i] = h;
    }
  }
}

void pollMusic() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      musicTitle = doc["title"] | "";
      musicArtist = doc["artist"] | "";
      musicAlbum = doc["album"] | "";
      musicPlaying = doc["playing"] | false;
      statusMusicPlaying = musicPlaying; // fast stop-detection while music shows
      musicElapsed = doc["elapsed"] | 0;
      musicDuration = doc["duration"] | 0;
      musicHasArtwork = doc["has_artwork"] | false;
      int rev = doc["artwork_rev"] | -1;
      bool coverChanged = rev != musicArtworkRev;
      musicArtworkRev = rev;
      int tRev = doc["text_rev"] | -1;
      bool textChanged = tRev != musicTextRev;
      musicTextRev = tRev;
      drawMusicScreen(coverChanged, textChanged);
    }
  }
  http.end();
}

// ---------- music spectrum ----------
// Poll the bridge's live spectrum (24 magnitudes, JSON) and repaint the
// current style. Runs only while MODE_SPECTRUM is showing.
void pollSpectrum() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/music/spectrum";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      JsonArray bars = doc["bars"];
      if (bars.size() > 0) {
        // time smoothing: spectrumSmooth 0 = raw, 10 = very damped.
        // Each frame blends toward the target; higher smooth = slower.
        float alpha = 1.0f - spectrumSmooth * 0.09f;
        for (int i = 0; i < SPECTRUM_BARS && i < (int)bars.size(); i++) {
          float target = (float)constrain(bars[i].as<int>(), 0, 255);
          spectrumSmoothed[i] = spectrumSmoothed[i] + (target - spectrumSmoothed[i]) * (1.0f - alpha);
          spectrumBars[i] = (byte)spectrumSmoothed[i];
        }
        // adapt the rainbow range to the live magnitude envelope: the max
        // decays slowly, the min rises slowly, so the color sweep always
        // spans the current dynamic range even in long quiet sections.
        float curMin = 255, curMax = 0;
        for (int i = 0; i < SPECTRUM_BARS; i++) {
          float v = spectrumSmoothed[i];
          if (v < curMin) curMin = v;
          if (v > curMax) curMax = v;
        }
        spectrumRainbowMax = max(curMax, spectrumRainbowMax * 0.985f);
        spectrumRainbowMin = min(curMin, spectrumRainbowMin * 1.02f + 0.5f);
        if (spectrumRainbowMax - spectrumRainbowMin < 20) {
          spectrumRainbowMax = spectrumRainbowMin + 20; // keep a sane sweep
        }
        // silence gate: if the loudest bar is below the user threshold, treat
        // the frame as silence and zero it (no phantom spectrum when quiet).
        byte peakBar = 0;
        for (int i = 0; i < SPECTRUM_BARS; i++) {
          if (spectrumBars[i] > peakBar) peakBar = spectrumBars[i];
        }
        if (peakBar < spectrumSilence) {
          memset(spectrumBars, 0, sizeof(spectrumBars));
        }
        spectrumData = true;
        // static-frame guard: wave/radial repaint the whole panel every poll;
        // repainting an identical frame flickers. Skip unless the bars moved,
        // ANY drawing parameter changed, or this is the first frame.
        int styleKey = spectrumType * 10 + spectrumEffect;
        int paramKey = styleKey * 31
                     + spectrumColor * 17
                     + spectrumColor2 * 13
                     + spectrumPeak * 11
                     + spectrumSmooth
                     + spectrumWidth * 7
                     + spectrumGap * 5
                     + spectrumDecay
                     + spectrumLineW
                     + spectrumFill * 3
                     + spectrumFillColor * 2
                     + spectrumRainbow * 19
                     + spectrumRingW * 23
                     + spectrumRingGap * 29
                     + spectrumRingInner
                     + spectrumRingOuter
                     + spectrumRingInColor * 3
                     + spectrumRingFill * 5
                     + spectrumGradRange
                     + spectrumGradReverse * 7
                     + spectrumAutoRange * 11
                     + spectrumOffset
                     + spectrumSilence * 3
                     + spectrumMirror * 13
                     + spectrumDualRing * 17
                     + spectrumDualInner * 19
                     + spectrumDualOuter * 23;
        bool changed = spectrumFirstDraw || paramKey != lastSpectrumParamKey;
        if (!changed) {
          for (int i = 0; i < SPECTRUM_BARS; i++) {
            if (spectrumBars[i] != lastDrawnBars[i]) { changed = true; break; }
          }
        }
        if (changed) {
          lastSpectrumParamKey = paramKey;
          drawSpectrum();
          memcpy(lastDrawnBars, spectrumBars, sizeof(lastDrawnBars));
        }
      }
    }
  }
  http.end();
}

// ---------- screen mirror ----------
// The bridge scales the Mac/PC desktop (or a chosen window) down to the
// panel size and serves each frame as raw RGB565 via GET /mirror/raw. We
// fetch one frame per interval and stream it row-by-row straight to the
// panel (rowBuf scratch row, no full-frame buffer), byte order matching the
// pushImage() pipeline used by the sprite/cover code.
const unsigned long MIRROR_POLL_INTERVAL_MS = 500; // ~2 fps over LAN
const unsigned long MIRROR_HTTP_TIMEOUT_MS = 1500;

void pollMirror() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/mirror/raw";
  http.setTimeout(MIRROR_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    WiFiClient *stream = http.getStreamPtr();
    const size_t rowBytes = (size_t)SCREEN_W * 2;
    bool ok = true;
    for (int r = 0; r < SCREEN_H && ok; r++) {
      if (stream->readBytes((uint8_t *)rowBuf, rowBytes) != (int)rowBytes) {
        ok = false;
        break;
      }
      tft.pushImage(0, r, SCREEN_W, 1, rowBuf);
      yield();
    }
  }
  http.end();
}

// ---------- photo album ----------
// The bridge serves a photo list via GET /album/list (JSON {"count":N}) and
// the current resized 128x128 RGB565 photo via GET /album/raw. The bridge
// advances to the next photo on each /album/raw request, so we just poll it
// once per ALBUM_POLL_INTERVAL_MS and stream the frame row-by-row exactly
// like the mirror path (no idx needed on the wire).
const unsigned long ALBUM_POLL_INTERVAL_MS = 4000; // one slide per ~4s
const unsigned long ALBUM_HTTP_TIMEOUT_MS = 2000;

void pollAlbum() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/album/raw";
  http.setTimeout(ALBUM_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    WiFiClient *stream = http.getStreamPtr();
    const size_t rowBytes = (size_t)SCREEN_W * 2;
    bool ok = true;
    for (int r = 0; r < SCREEN_H && ok; r++) {
      if (stream->readBytes((uint8_t *)rowBuf, rowBytes) != (int)rowBytes) {
        ok = false;
        break;
      }
      tft.pushImage(0, r, SCREEN_W, 1, rowBuf);
      yield();
    }
  }
  http.end();
}

// ---------- stock watchlist screen ----------

bool handleStockPayload(const String &payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  JsonArray arr = doc["stocks"];
  stockCount = 0;
  for (JsonObject s : arr) {
    if (stockCount >= MAX_STOCKS) break;
    stocks[stockCount].code = s["code"] | "";
    stocks[stockCount].price = s["price"] | "";
    stocks[stockCount].pct = s["pct"] | "";
    stocks[stockCount].up = s["up"] | 0;
    stockCount++;
  }
  stockNamesRev = doc["names_rev"] | -1;
  stockEverLoaded = true;
  stockDirty = true;
  return true;
}

// Streams the Mac-rendered name strips and blits one per row (top line,
// right of the ASCII code). Wired-only mode has no HTTP: codes still show.
bool drawStockNames() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return false;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/stock/names.raw";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t cnt = 0;
  if (stream->readBytes(&cnt, 1) != 1) {
    http.end();
    return false;
  }
  const size_t rowBytes = (size_t)STOCK_NAME_W * 2;
  // Strip height isn't negotiated on the wire: derive it from Content-Length
  // ([1B count][cnt x WxH RGB565 strips]) so an out-of-sync bridge version
  // degrades to smaller names instead of garbage. No length -> STOCK_NAME_H.
  int stripH = STOCK_NAME_H;
  int len = http.getSize();
  size_t perRow = cnt * rowBytes;
  if (len > 1 && perRow > 0 && (size_t)(len - 1) % perRow == 0) {
    int h = (int)((size_t)(len - 1) / perRow);
    if (h >= 8 && h <= 32) stripH = h;
  }
  const int dispRows = min(stripH / 2, 12); // top line is 12px tall
  bool ok = true;
  // 2x downsample for the 128px panel: read a full 156px strip row, keep
  // every other pixel (78px wide); display every other row (dispRows tall).
  for (int i = 0; i < cnt && ok; i++) {
    int y0 = 4 + i * 26;
    for (int r = 0; r < stripH / 2; r++) {
      for (int skip = 0; skip < 2; skip++) {
        if (stream->readBytes((uint8_t *)rowBuf, rowBytes) != (int)rowBytes) {
          ok = false;
          break;
        }
        if (skip == 0 && r < dispRows) {
          for (int c = 0; c < STOCK_NAME_W / 2; c++) rowBuf[c] = rowBuf[c * 2];
          if (i < stockCount) tft.pushImage(48, y0 + r, STOCK_NAME_W / 2, 1, rowBuf);
        }
      }
      yield();
    }
  }
  http.end();
  return ok;
}

void pollStock() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) return;
  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + "/stock";
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == HTTP_CODE_OK) handleStockPayload(http.getString());
  http.end();
}

// 26px per row (compact for 128px panel): small grey code on top, font-2
// price (white) on the left and change% on the right - red rising / green
// falling (CN convention). Rows repaint only when their text changes.
void drawStockScreen() {
  if (!stockChromeDrawn) {
    tft.fillScreen(TFT_BLACK);
    stockChromeDrawn = true;
    for (int i = 0; i < MAX_STOCKS; i++) {
      stockLastCode[i] = "\x01"; // force repaint
      stockLastVal[i] = "\x01";
    }
    stockNamesDrawnRev = -1;
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(0x7BEF, TFT_BLACK);
    tft.drawString("STOCKS", SCREEN_CX, 118, 1);
  }
  stockDirty = false;

  if (stockCount == 0) {
    if (stockLastCode[0] != "") {
      for (int i = 0; i < MAX_STOCKS; i++) {
        stockLastCode[i] = "";
        stockLastVal[i] = "";
      }
      tft.fillRect(0, 0, SCREEN_W, 116, TFT_BLACK);
      tft.setTextDatum(TC_DATUM);
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString(stockEverLoaded ? "No stocks configured" : "Waiting for bridge...", SCREEN_CX, 56, 2);
      if (stockEverLoaded) tft.drawString("Mac menu: Set watchlist", SCREEN_CX, 74, 1);
    }
    return;
  }

  for (int i = 0; i < MAX_STOCKS; i++) {
    int y0 = 4 + i * 26;
    bool has = i < stockCount;
    // top line (code + name strip) and value line refresh independently, so
    // a price tick never wipes the name bitmap
    String codeKey = has ? stocks[i].code : "";
    if (codeKey != stockLastCode[i]) {
      stockLastCode[i] = codeKey;
      tft.fillRect(0, y0, SCREEN_W, 12, TFT_BLACK);
      stockNamesDrawnRev = -1; // strip area wiped: re-fetch names
      if (has) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(0x7BEF, TFT_BLACK);
        // font-1 code (8px tall) shifted down 2px so it center-aligns
        // with the 12px CJK name bitmap strip beside it
        tft.drawString(stocks[i].code, 6, y0 + 2, 1);
      }
    }
    String valKey = has ? stocks[i].price + "|" + stocks[i].pct + "|" + String(stocks[i].up) : "";
    if (valKey != stockLastVal[i]) {
      stockLastVal[i] = valKey;
      tft.fillRect(0, y0 + 12, SCREEN_W, 14, TFT_BLACK); // value line + inter-row gap
      if (has) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(stocks[i].price, 6, y0 + 12, 2);
        uint16_t pc = stocks[i].up > 0 ? TFT_RED : (stocks[i].up < 0 ? TFT_GREEN : TFT_LIGHTGREY);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(pc, TFT_BLACK);
        tft.drawString(stocks[i].pct, 124, y0 + 12, 2);
      }
    }
  }

  // CJK name strips, re-fetched when the watchlist (names_rev) changes
  if (stockNamesRev >= 0 && stockNamesDrawnRev != stockNamesRev) {
    if (drawStockNames()) stockNamesDrawnRev = stockNamesRev;
  }
}

// ---------- WiFi / bridge polling ----------

WiFiManager wifiManager; // global: the config portal now runs non-blocking in loop()

void configModeCallback(WiFiManager *wm) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("WiFi setup needed", 8, 8, 2);
  tft.drawString("Connect phone to AP:", 8, 28, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(WIFI_PORTAL_AP_NAME, 8, 46, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("then open 192.168.4.1", 8, 66, 1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Or: plug into the computer", 8, 84, 1);
  tft.drawString("via USB - no WiFi needed", 8, 96, 1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Firmware v" FW_VERSION, 8, 116, 1);
}

// Non-blocking: with saved credentials this still waits ~10s for the join,
// but a missing/failed WiFi no longer traps boot in the portal - the portal
// keeps running from loop() while the USB serial link can take over the
// screen (wired mode for APs with client isolation).
void setupWiFi() {
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setConfigPortalBlocking(false);

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting WiFi...", 8, 56, 2);

  Serial.println("[wifi] starting WiFiManager autoConnect (non-blocking portal)...");
  bool ok = wifiManager.autoConnect(WIFI_PORTAL_AP_NAME);
  Serial.printf("[wifi] autoConnect result=%d ssid=%s ip=%s\n", ok, WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  Serial.printf("[wifi] bridge host = '%s'\n", bridgeHost.c_str());
}

bool parseStatusJson(const String &payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return false;

  JsonObject c = doc["claude"];
  if (!c.isNull()) {
    claudeStatus.status = c["status"] | "unknown";
    claudeStatus.tokensToday = c["tokens_today"] | 0;
    claudeStatus.sessionMin = c["session_min"] | 0;
    claudeStatus.sessionWindowMin = c["session_window_min"] | 300;
    claudeStatus.fiveHourPct = c["five_hour_pct"] | -1.0;
    claudeStatus.fiveHourResetMin = c["five_hour_reset_min"] | -1;
    claudeStatus.sevenDayPct = c["seven_day_pct"] | -1.0;
    claudeStatus.sevenDayResetMin = c["seven_day_reset_min"] | -1;
    claudeStatus.needsInput = c["needs_input"] | false;
  }

  JsonObject x = doc["codex"];
  if (!x.isNull()) {
    codexStatus.status = x["status"] | "unknown";
    codexStatus.tokensToday = x["tokens_today"] | 0;
    codexStatus.primaryPct = x["primary_pct"] | -1.0;
    codexStatus.primaryResetMin = x["primary_reset_min"] | -1;
    codexStatus.weeklyPct = x["weekly_pct"] | -1.0;
    codexStatus.weeklyResetMin = x["weekly_reset_min"] | -1;
    codexStatus.needsInput = x["needs_input"] | false;
  }
  statusMusicPlaying = doc["music_playing"] | false;
  return true;
}

// The mode actually rendered. In AUTO: a pending approval prompt wins (stay on
// the pet so its border can flash red at you), otherwise audio promotes to the
// music page.
DisplayMode effectiveMode() {
  if (displayMode == MODE_AUTO) {
    if (claudeStatus.needsInput || codexStatus.needsInput) return MODE_AUTO;
    // music page needs HTTP for cover/text bitmaps, so don't auto-promote
    // when running wired-only (no WiFi)
    if (statusMusicPlaying && WiFi.status() == WL_CONNECTED) return MODE_MUSIC;
  }
  return displayMode;
}

void pollBridge() {
  if (WiFi.status() != WL_CONNECTED || bridgeHost.length() == 0) {
    Serial.printf("[bridge] skip poll: wifi=%d host='%s'\n", WiFi.status() == WL_CONNECTED, bridgeHost.c_str());
    return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + bridgeHost + BRIDGE_DEFAULT_PATH;
  http.setTimeout(BRIDGE_HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("[bridge] http.begin() failed");
    return;
  }
  int code = http.GET();
  Serial.printf("[bridge] GET %s -> %d\n", url.c_str(), code);
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    if (parseStatusJson(payload)) {
      lastSuccessMs = millis();
      everPolled = true;
      Serial.printf("[bridge] claude=%s tok=%ld | codex=%s tok=%ld primary=%.0f%%\n",
                    claudeStatus.status.c_str(), claudeStatus.tokensToday,
                    codexStatus.status.c_str(), codexStatus.tokensToday, codexStatus.primaryPct);
    } else {
      Serial.println("[bridge] JSON parse failed");
    }
  } else {
    claudeStatus.status = "offline";
    codexStatus.status = "offline";
  }
  http.end();
  DisplayMode eff = effectiveMode();
  if (eff != MODE_NET && eff != MODE_MUSIC && eff != MODE_STOCK &&
      eff != MODE_MIRROR && eff != MODE_ALBUM && eff != MODE_SPECTRUM) {
    // Only a real app switch clears the screen; a plain data refresh paints
    // in place so the poll doesn't flash the whole display.
    if (updateActiveApp()) drawActiveApp();
    else refreshActiveApp();
  }
}

// ---------- wired (USB serial) bridge link ----------
// Fallback for WiFi networks with client isolation (device can't reach the
// bridge over LAN) - or for skipping WiFi setup entirely: when the clock is
// plugged into the computer over USB, the bridge pushes the same /status and
// /net payloads down the CH340 serial line as newline-terminated frames:
//   bridge -> device:  #HELLO   #STATUS {json}   #NET {json}   #CMD {json}
//   device -> bridge:  #DEVICE {"name":"aiclock","fw":"x.y.z"}
// Everything else the device prints (logs) is ignored by the bridge.
unsigned long lastSerialFrameMs = 0;
bool wiredEverLinked = false;
char serialLine[1600]; // biggest frame is #STATUS at ~600 bytes
size_t serialLineLen = 0;

bool wiredActive() { return wiredEverLinked && (millis() - lastSerialFrameMs) < 15000UL; }

// First data over either transport replaces the boot/portal screen.
void showMainUiIfNeeded() {
  if (mainUiShown) return;
  mainUiShown = true;
  drawStaticChrome();
  updateActiveApp();
  drawActiveApp();
}

void handleSerialFrame(char *line) {
  lastSerialFrameMs = millis();
  wiredEverLinked = true;
  if (!strncmp(line, "#HELLO", 6)) {
    Serial.printf("#DEVICE {\"name\":\"aiclock\",\"fw\":\"%s\"}\n", FW_VERSION);
    return;
  }
  if (!strncmp(line, "#STATUS ", 8)) {
    if (parseStatusJson(String(line + 8))) {
      lastSuccessMs = millis();
      everPolled = true;
      showMainUiIfNeeded();
      DisplayMode eff = effectiveMode();
      if (eff != MODE_NET && eff != MODE_MUSIC && eff != MODE_STOCK &&
          eff != MODE_MIRROR && eff != MODE_ALBUM && eff != MODE_SPECTRUM) {
        if (updateActiveApp()) drawActiveApp();
        else refreshActiveApp();
      }
    }
    return;
  }
  if (!strncmp(line, "#NET ", 5)) {
    handleNetPayload(String(line + 5));
    return;
  }
  if (!strncmp(line, "#STOCK ", 7)) {
    handleStockPayload(String(line + 7));
    return;
  }
  if (!strncmp(line, "#CMD ", 5)) {
    JsonDocument doc;
    if (deserializeJson(doc, line + 5)) return;
    if (doc["brightness"].is<int>()) {
      brightness = constrain(doc["brightness"].as<int>(), 0, 100);
      applyBrightness();
      saveBrightness();
    }
    const char *mode = doc["display"] | (const char *)nullptr;
    if (mode) {
      String m(mode);
      if (m == "auto") displayMode = MODE_AUTO;
      else if (m == "claude") displayMode = MODE_CLAUDE;
      else if (m == "codex") displayMode = MODE_CODEX;
      else if (m == "net") displayMode = MODE_NET;
      else if (m == "music") displayMode = MODE_MUSIC;
      else if (m == "stock") displayMode = MODE_STOCK;
      // the effectiveMode transition handler in loop() repaints the chrome
    }
    return;
  }
}

// Drains the UART, splitting on newlines; frames start with '#', everything
// else (line noise, echoes) is dropped.
void pumpSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (serialLineLen > 0 && serialLine[0] == '#') {
        serialLine[serialLineLen] = 0;
        handleSerialFrame(serialLine);
      }
      serialLineLen = 0;
    } else if (serialLineLen < sizeof(serialLine) - 1) {
      serialLine[serialLineLen++] = ch;
    } else {
      serialLineLen = 0; // oversized line: drop it
    }
  }
}

// ---------- web admin ----------

String htmlEscape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

void handleRoot() {
  String age = everPolled ? String((millis() - lastSuccessMs) / 1000) + "s ago" : "never";
  String html;
  html.reserve(3072);
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>AI Clock ????</title>";
  html += "<style>body{font-family:-apple-system,sans-serif;max-width:480px;margin:24px "
          "auto;padding:0 16px;color:#222} h1{font-size:20px} label{display:block;margin-top:16px;font-weight:600}"
          "input{width:100%;box-sizing:border-box;padding:8px;font-size:16px;margin-top:4px}"
          "button{margin-top:16px;padding:10px 20px;font-size:16px;background:#2563eb;color:#fff;"
          "border:none;border-radius:6px}"
          "table{margin-top:20px;border-collapse:collapse;width:100%}"
          "td{padding:4px 8px;border-bottom:1px solid #eee;font-size:14px}"
          ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}"
          "</style></head><body>";
  html += "<h1>AI Clock ????</h1>";

  html += "<form method='POST' action='/save'>";
  html += "<label>Bridge host (ip:port)</label>";
  html += "<input name='bridge' value='" + htmlEscape(bridgeHost) + "' placeholder='192.168.1.181:8765'>";
  html += "<button type='submit'>????</button>";
  html += "</form>";

  // Backlight brightness slider: applies live on release (PWM, persisted).
  html += "<h2 style='font-size:16px;margin-top:28px'>???????</h2>";
  html += "<input type='range' min='0' max='100' value='" + String(brightness) + "' id='bri' "
          "oninput=\"document.getElementById('briv').textContent=this.value+'%'\" "
          "onchange=\"fetch('/api/brightness',{method:'POST',headers:{'Content-Type':"
          "'application/x-www-form-urlencoded'},body:'level='+this.value})\">";
  html += "<div style='font-size:13px;color:#555'>?????<span id='briv'>" + String(brightness) +
          "%</span>??0 = ???????????????????????</div>";

  // Backlight polarity invert toggle.
  html += "<div style='margin-top:8px'>";
  html += "<label style='display:inline;font-weight:400'>";
  html += "<input type='checkbox' id='blInv' " +
          String(brightnessInvert ? "checked" : "") +
          " onchange=\"fetch('/api/brightness-invert',{method:'POST',"
          "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'invert='+(this.checked?1:0)}).then(r=>{if(!r.ok)location.reload()})\"> "
          "????????????????????????????</label></div>";

  // Display rotation / mirror ?? applies live, persisted.
  html += "<h2 style='font-size:16px;margin-top:28px'>??????? & ????</h2>";
  html += "<select id='rotSel' onchange=\"fetch('/api/rotation',{method:'POST',"
          "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'rot='+this.value}).then(r=>{if(!r.ok)location.reload()})\">";
  const char *rotLabels[8] = {
    "0?? ????", "90?? ????", "180?? ????", "270?? ?????",
    "0?? ??????", "90?? ??????", "180?? ??????", "270?? ??????"
  };
  for (int i = 0; i < 8; i++) {
    html += "<option value='" + String(i) + "'" +
            (i == displayRotation ? " selected" : "") + ">" +
            rotLabels[i] + "</option>";
  }
  html += "</select><div style='font-size:13px;color:#555'>????????????????</div>";

  // On-device GIF upload: replaces a character's animation without reflashing.
  html += "<h2 style='font-size:16px;margin-top:28px'>??????????? GIF??</h2>";
  html += "<p style='font-size:13px;color:#555'>?????? .gif?????????????????????????????"
          "?????????????????????????????GIF ???????????????????????????????</p>";
  html += "<form id='gifForm' method='POST' enctype='multipart/form-data' onsubmit='return setGifAction()'>";
  html += "<label>???</label>";
  html += "<select id='gifTarget'><option value='claude'>Claude</option><option value='codex'>Codex</option></select>";
  html += "<label>GIF ???</label><input type='file' name='file' accept='.gif' required>";
  html += "<button type='submit'>????????</button>";
  html += "</form>";
  html += "<script>function setGifAction(){"
          "document.getElementById('gifForm').action='/sprite/'+document.getElementById('gifTarget').value;"
          "return true;}</script>";

  html += "<table>";
  html += "<tr><td>WiFi SSID</td><td>" + htmlEscape(WiFi.SSID()) + "</td></tr>";
  html += "<tr><td>?? IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>?????????</td><td>" + age + "</td></tr>";
  html += "<tr><td>Claude</td><td>" + htmlEscape(claudeStatus.status) + ", " +
          formatTokens(claudeStatus.tokensToday) + " tok</td></tr>";
  html += "<tr><td>Codex</td><td>" + htmlEscape(codexStatus.status) + ", " +
          formatTokens(codexStatus.tokensToday) + " tok, " +
          (codexStatus.primaryPct >= 0 ? "5h " + String(codexStatus.primaryPct, 0) + "%"
           : codexStatus.weeklyPct >= 0 ? "Wk " + String(codexStatus.weeklyPct, 0) + "%"
                                        : "5h ?") + "</td></tr>";
  html += "</table>";

  html += "<form method='POST' action='/reset-wifi' onsubmit=\"return confirm('??? WiFi "
          "?????????????????????');\">";
  html += "<button type='submit' style='background:#dc2626'>???? WiFi</button>";
  html += "</form>";

  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void handleSave() {
  String newHost = webServer.arg("bridge");
  newHost.trim();
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[web] bridge host updated to '%s'\n", bridgeHost.c_str());
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// ---------- JSON API for the Mac app ----------

const char *displayModeName(DisplayMode m) {
  if (m == MODE_CLAUDE) return "claude";
  if (m == MODE_CODEX) return "codex";
  if (m == MODE_NET) return "net";
  if (m == MODE_MUSIC) return "music";
  if (m == MODE_STOCK) return "stock";
  if (m == MODE_MIRROR) return "mirror";
  if (m == MODE_ALBUM) return "album";
  if (m == MODE_SPECTRUM) return "spectrum";
  return "auto";
}

void handleApiInfo() {
  JsonDocument doc;
  doc["ip"] = WiFi.localIP().toString();
  doc["ssid"] = WiFi.SSID();
  doc["bridge"] = bridgeHost;
  doc["mode"] = displayModeName(displayMode);           // configured mode
  doc["effective"] = displayModeName(effectiveMode());   // what's on screen now
  doc["music_playing"] = statusMusicPlaying;
  doc["showing"] = (currentApp == APP_CLAUDE) ? "claude" : "codex";
  doc["last_update_s"] = everPolled ? (long)((millis() - lastSuccessMs) / 1000) : -1;
  doc["sprite_rev"] = spriteRev;
  doc["brightness"] = brightness;
  doc["brightness_invert"] = brightnessInvert;
  doc["rotation"] = displayRotation;
  doc["spectrum_type"] = spectrumType;
  doc["spectrum_effect"] = spectrumEffect;
  doc["spectrum_color"] = spectrumColor;
  doc["spectrum_color2"] = spectrumColor2;
  doc["spectrum_peak"] = spectrumPeak;
  doc["spectrum_smooth"] = spectrumSmooth;
  doc["spectrum_width"] = spectrumWidth;
  doc["spectrum_rainbow"] = spectrumRainbow;
  doc["spectrum_gap"] = spectrumGap;
  doc["spectrum_decay"] = spectrumDecay;
  doc["spectrum_linew"] = spectrumLineW;
  doc["spectrum_fill"] = spectrumFill;
  doc["spectrum_ringw"] = spectrumRingW;
  doc["spectrum_ringgap"] = spectrumRingGap;
  doc["spectrum_ringinner"] = spectrumRingInner;
  doc["spectrum_ringouter"] = spectrumRingOuter;
  doc["spectrum_fillcolor"] = spectrumFillColor;
  doc["spectrum_ringincolor"] = spectrumRingInColor;
  doc["spectrum_ringfill"] = spectrumRingFill;
  doc["spectrum_gradrange"] = spectrumGradRange;
  doc["spectrum_gradreverse"] = spectrumGradReverse;
  doc["spectrum_autorange"] = spectrumAutoRange;
  doc["spectrum_offset"] = spectrumOffset;
  doc["spectrum_silence"] = spectrumSilence;
  doc["spectrum_mirror"] = spectrumMirror;
  doc["spectrum_dualring"] = spectrumDualRing;
  doc["spectrum_dualin"] = spectrumDualInner;
  doc["spectrum_dualout"] = spectrumDualOuter;
  doc["wired"] = wiredActive(); // true = data currently arrives over USB serial
  doc["fw"] = FW_VERSION;
  JsonObject c = doc["claude"].to<JsonObject>();
  c["status"] = claudeStatus.status;
  c["custom_sprite"] = claudeCustom;
  c["w"] = CLAUDE_SPRITE_W;
  c["h"] = CLAUDE_SPRITE_H;
  JsonObject x = doc["codex"].to<JsonObject>();
  x["status"] = codexStatus.status;
  x["custom_sprite"] = codexCustom;
  x["w"] = CODEX_SPRITE_W;
  x["h"] = CODEX_SPRITE_H;
  String out;
  serializeJson(doc, out);
  webServer.send(200, "application/json", out);
}

void handleApiDisplay() {
  String mode = webServer.arg("mode");
  if (mode == "auto") displayMode = MODE_AUTO;
  else if (mode == "claude") displayMode = MODE_CLAUDE;
  else if (mode == "codex") displayMode = MODE_CODEX;
  else if (mode == "net") displayMode = MODE_NET;
  else if (mode == "music") displayMode = MODE_MUSIC;
  else if (mode == "stock") displayMode = MODE_STOCK;
  else if (mode == "mirror") displayMode = MODE_MIRROR;
  else if (mode == "album") displayMode = MODE_ALBUM;
  else if (mode == "spectrum") displayMode = MODE_SPECTRUM;
  else {
    webServer.send(400, "text/plain", "mode must be auto|claude|codex|net|music|stock|mirror|album|spectrum");
    return;
  }
  Serial.printf("[api] display mode = %s\n", mode.c_str());
  if (displayMode == MODE_NET) {
    netChromeDrawn = false;
    lastNetPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_MUSIC) {
    musicChromeDrawn = false;
    lastMusicPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_STOCK) {
    stockChromeDrawn = false;
    lastStockPollMs = 0; // poll + draw on the next loop tick
  } else if (displayMode == MODE_MIRROR || displayMode == MODE_ALBUM || displayMode == MODE_SPECTRUM) {
    lastMirrorPollMs = 0; // fetch a frame on the next loop tick
    lastAlbumPollMs = 0;
    lastSpectrumPollMs = 0;
  } else {
    updateActiveApp();
    drawActiveApp(); // unconditional: also repaints over a previous net chart
  }
  webServer.send(200, "text/plain", "ok");
}

void handleApiBrightness() {
  String levelArg = webServer.arg("level");
  if (levelArg.length() == 0) {
    webServer.send(400, "text/plain", "missing level (0-100)");
    return;
  }
  int level = levelArg.toInt();
  if (level < 0) level = 0;
  if (level > 100) level = 100;
  brightness = level;
  applyBrightness();
  saveBrightness();
  Serial.printf("[api] brightness = %d\n", brightness);
  webServer.send(200, "text/plain", "ok");
}

void handleApiBrightnessInvert() {
  String invArg = webServer.arg("invert");
  if (invArg.length() == 0) {
    webServer.send(400, "text/plain", "missing invert (0/1)");
    return;
  }
  brightnessInvert = (invArg.toInt() != 0);
  applyBrightness();
  saveBrightnessInvert();
  Serial.printf("[api] brightness_invert = %d\n", brightnessInvert);
  webServer.send(200, "text/plain", "ok");
}

void handleApiMusicSpectrum() {
  // Optional parameters: type=0-2, effect=0-6, color=0-7, peak=0/1,
  // smooth=0-10, width=1-5. Any that are present are applied together.
  String typeArg = webServer.arg("type");
  if (typeArg.length() > 0) {
    int t = typeArg.toInt();
    if (t < 0 || t > 2) {
      webServer.send(400, "text/plain", "type must be 0-2");
      return;
    }
    spectrumType = t;
    // clamp effect to the new type's range
    int maxEffect = t == 0 ? 8 : (t == 1 ? 3 : 3);
    if (spectrumEffect > maxEffect) spectrumEffect = maxEffect;
  }
  String effectArg = webServer.arg("effect");
  if (effectArg.length() > 0) {
    int e = effectArg.toInt();
    int maxEffect = spectrumType == 0 ? 8 : (spectrumType == 1 ? 3 : 3);
    if (e < 0 || e > maxEffect) {
      webServer.send(400, "text/plain", "effect out of range for type");
      return;
    }
    spectrumEffect = e;
  }
  String colorArg = webServer.arg("color");
  if (colorArg.length() > 0) {
    int c = colorArg.toInt();
    if (c < 0 || c > 11) {
      webServer.send(400, "text/plain", "color must be 0-11");
      return;
    }
    spectrumColor = c;
  }
  String color2Arg = webServer.arg("color2");
  if (color2Arg.length() > 0) {
    int c2 = color2Arg.toInt();
    if (c2 < 0 || c2 > 11) {
      webServer.send(400, "text/plain", "color2 must be 0-11");
      return;
    }
    spectrumColor2 = c2;
  }
  String peakArg = webServer.arg("peak");
  if (peakArg.length() > 0) {
    int p = peakArg.toInt();
    if (p != 0 && p != 1) {
      webServer.send(400, "text/plain", "peak must be 0 or 1");
      return;
    }
    spectrumPeak = p;
  }
  String smoothArg = webServer.arg("smooth");
  if (smoothArg.length() > 0) {
    int sm = smoothArg.toInt();
    if (sm < 0 || sm > 10) {
      webServer.send(400, "text/plain", "smooth must be 0-10");
      return;
    }
    spectrumSmooth = sm;
  }
  String widthArg = webServer.arg("width");
  if (widthArg.length() > 0) {
    int w = widthArg.toInt();
    if (w < 1 || w > 5) {
      webServer.send(400, "text/plain", "width must be 1-5");
      return;
    }
    spectrumWidth = w;
  }
  String rainbowArg = webServer.arg("rainbow");
  if (rainbowArg.length() > 0) {
    int rb = rainbowArg.toInt();
    if (rb != 0 && rb != 1) {
      webServer.send(400, "text/plain", "rainbow must be 0 or 1");
      return;
    }
    spectrumRainbow = rb;
  }
  // per-type fine-tuning params
  String gapArg = webServer.arg("gap");
  if (gapArg.length() > 0) {
    int g = gapArg.toInt();
    if (g < 0 || g > 4) {
      webServer.send(400, "text/plain", "gap must be 0-4");
      return;
    }
    spectrumGap = g;
  }
  String decayArg = webServer.arg("decay");
  if (decayArg.length() > 0) {
    int d = decayArg.toInt();
    if (d < 1 || d > 20) {
      webServer.send(400, "text/plain", "decay must be 1-20");
      return;
    }
    spectrumDecay = d;
  }
  String linewArg = webServer.arg("linew");
  if (linewArg.length() > 0) {
    int lw = linewArg.toInt();
    if (lw < 1 || lw > 5) {
      webServer.send(400, "text/plain", "linew must be 1-5");
      return;
    }
    spectrumLineW = lw;
  }
  String fillArg = webServer.arg("fill");
  if (fillArg.length() > 0) {
    int f = fillArg.toInt();
    if (f != 0 && f != 1) {
      webServer.send(400, "text/plain", "fill must be 0 or 1");
      return;
    }
    spectrumFill = f;
  }
  // radial/ring params
  String ringwArg = webServer.arg("ringw");
  if (ringwArg.length() > 0) {
    int rw = ringwArg.toInt();
    if (rw < 1 || rw > 8) {
      webServer.send(400, "text/plain", "ringw must be 1-8");
      return;
    }
    spectrumRingW = rw;
  }
  String ringgapArg = webServer.arg("ringgap");
  if (ringgapArg.length() > 0) {
    int rg = ringgapArg.toInt();
    if (rg < 0 || rg > 10) {
      webServer.send(400, "text/plain", "ringgap must be 0-10");
      return;
    }
    spectrumRingGap = rg;
  }
  String ringinArg = webServer.arg("ringinner");
  if (ringinArg.length() > 0) {
    int ri = ringinArg.toInt();
    if (ri < 2 || ri > 60) {
      webServer.send(400, "text/plain", "ringinner must be 2-60");
      return;
    }
    spectrumRingInner = ri;
  }
  String ringoutArg = webServer.arg("ringouter");
  if (ringoutArg.length() > 0) {
    int ro = ringoutArg.toInt();
    if (ro < 20 || ro > 64) {
      webServer.send(400, "text/plain", "ringouter must be 20-64");
      return;
    }
    spectrumRingOuter = ro;
  }
  String fillcolArg = webServer.arg("fillcolor");
  if (fillcolArg.length() > 0) {
    int fc = fillcolArg.toInt();
    if (fc < 0 || fc > 11) {
      webServer.send(400, "text/plain", "fillcolor must be 0-11");
      return;
    }
    spectrumFillColor = fc;
  }
  String ringincolArg = webServer.arg("ringincolor");
  if (ringincolArg.length() > 0) {
    int rc = ringincolArg.toInt();
    if (rc < 0 || rc > 9) {
      webServer.send(400, "text/plain", "ringincolor must be 0-9");
      return;
    }
    spectrumRingInColor = rc;
  }
  String ringfillArg = webServer.arg("ringfill");
  if (ringfillArg.length() > 0) {
    int rf = ringfillArg.toInt();
    if (rf != 0 && rf != 1) {
      webServer.send(400, "text/plain", "ringfill must be 0 or 1");
      return;
    }
    spectrumRingFill = rf;
  }
  String gradrangeArg = webServer.arg("gradrange");
  if (gradrangeArg.length() > 0) {
    int gr = gradrangeArg.toInt();
    if (gr < 0 || gr > 100) {
      webServer.send(400, "text/plain", "gradrange must be 0-100");
      return;
    }
    spectrumGradRange = gr;
  }
  String gradrevArg = webServer.arg("gradreverse");
  if (gradrevArg.length() > 0) {
    int gv = gradrevArg.toInt();
    if (gv != 0 && gv != 1) {
      webServer.send(400, "text/plain", "gradreverse must be 0 or 1");
      return;
    }
    spectrumGradReverse = gv;
  }
  String autorangeArg = webServer.arg("autorange");
  if (autorangeArg.length() > 0) {
    int ar = autorangeArg.toInt();
    if (ar != 0 && ar != 1) {
      webServer.send(400, "text/plain", "autorange must be 0 or 1");
      return;
    }
    spectrumAutoRange = ar;
  }
  String offsetArg = webServer.arg("offset");
  if (offsetArg.length() > 0) {
    int of = offsetArg.toInt();
    if (of < -100 || of > 100) {
      webServer.send(400, "text/plain", "offset must be -100..100");
      return;
    }
    spectrumOffset = of;
  }
  String silenceArg = webServer.arg("silence");
  if (silenceArg.length() > 0) {
    int si = silenceArg.toInt();
    if (si < 0 || si > 50) {
      webServer.send(400, "text/plain", "silence must be 0-50");
      return;
    }
    spectrumSilence = si;
  }
  String mirrorArg = webServer.arg("mirror");
  if (mirrorArg.length() > 0) {
    int mi = mirrorArg.toInt();
    if (mi != 0 && mi != 1) {
      webServer.send(400, "text/plain", "mirror must be 0 or 1");
      return;
    }
    spectrumMirror = mi;
  }
  String dualringArg = webServer.arg("dualring");
  if (dualringArg.length() > 0) {
    int dr = dualringArg.toInt();
    if (dr != 0 && dr != 1) {
      webServer.send(400, "text/plain", "dualring must be 0 or 1");
      return;
    }
    spectrumDualRing = dr;
  }
  String dualinArg = webServer.arg("dualin");
  if (dualinArg.length() > 0) {
    int di = dualinArg.toInt();
    if (di < 0 || di > 100) {
      webServer.send(400, "text/plain", "dualin must be 0-100");
      return;
    }
    spectrumDualInner = di;
  }
  String dualoutArg = webServer.arg("dualout");
  if (dualoutArg.length() > 0) {
    int do2 = dualoutArg.toInt();
    if (do2 < 0 || do2 > 100) {
      webServer.send(400, "text/plain", "dualout must be 0-100");
      return;
    }
    spectrumDualOuter = do2;
  }
  saveSpectrumStyle();
  Serial.printf("[api] spectrum type=%d effect=%d color=%d peak=%d smooth=%d width=%d gap=%d decay=%d linew=%d fill=%d ringw=%d ringgap=%d ringin=%d ringout=%d fillcol=%d ringincol=%d ringfill=%d gradrange=%d gradreverse=%d autorange=%d offset=%d\n",
                spectrumType, spectrumEffect, spectrumColor, spectrumPeak, spectrumSmooth, spectrumWidth,
                spectrumGap, spectrumDecay, spectrumLineW, spectrumFill,
                spectrumRingW, spectrumRingGap, spectrumRingInner, spectrumRingOuter,
                spectrumFillColor, spectrumRingInColor, spectrumRingFill,
                spectrumGradRange, spectrumGradReverse, spectrumAutoRange, spectrumOffset);
  webServer.send(200, "text/plain", "ok");
}

void handleApiRotation() {
  String rotArg = webServer.arg("rot");
  if (rotArg.length() == 0) {
    webServer.send(400, "text/plain", "missing rot (0-7)");
    return;
  }
  int r = rotArg.toInt();
  if (r < 0 || r > 7) {
    webServer.send(400, "text/plain", "rot must be 0-7");
    return;
  }
  displayRotation = r;
  applyDisplayRotation();
  // Clear the physical GRAM (MADCTL changed how pixel data maps) then
  // draw the pet UI immediately so the user sees something.  Non-pet modes
  // (NET/MUSIC/STOCK) will take over on the next bridge/serial data poll.
  tft.fillScreen(TFT_BLACK);
  netChromeDrawn = false;
  musicChromeDrawn = false;
  stockChromeDrawn = false;
  stockNamesDrawnRev = -1;
  mainUiShown = false;
  showMainUiIfNeeded();
  saveDisplayConfig();
  Serial.printf("[api] rotation = %d\n", displayRotation);
  webServer.send(200, "text/plain", "ok");
}

void handleApiBridge() {
  String newHost = webServer.arg("host");
  newHost.trim();
  if (newHost.length() == 0) {
    webServer.send(400, "text/plain", "missing host");
    return;
  }
  bridgeHost = newHost;
  saveBridgeHost(bridgeHost);
  Serial.printf("[api] bridge host = '%s'\n", bridgeHost.c_str());
  webServer.send(200, "text/plain", "ok");
  lastPollMs = 0; // poll the new bridge on the next loop tick
}

// Streams the animation currently in use for a slot, in the same wire format
// as the custom .bin: [1 byte frame count][RGB565 frames...]. Lets the Mac
// app mirror exactly what the device is showing (custom upload or built-in).
void handleSpriteRaw(ActiveApp slot) {
  bool custom = (slot == APP_CLAUDE) ? claudeCustom : codexCustom;
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  if (custom) {
    File f = LittleFS.open(binPath, "r");
    if (f) {
      webServer.streamFile(f, "application/octet-stream");
      f.close();
      return;
    }
  }
  int frames = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FRAMES : CODEX_SPRITE_FRAMES;
  int w = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int h = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;
  const uint16_t *const *arr = (slot == APP_CLAUDE) ? claude_sprite_frames : codex_sprite_frames;
  size_t frameBytes = (size_t)w * h * 2;
  webServer.setContentLength(1 + (size_t)frames * frameBytes);
  webServer.send(200, "application/octet-stream", "");
  uint8_t cnt = (uint8_t)frames;
  webServer.sendContent((const char *)&cnt, 1);
  for (int i = 0; i < frames; i++) {
    webServer.sendContent_P((PGM_P)arr[i], frameBytes);
    yield();
  }
}

// Removes a custom sprite so the compiled-in default animation comes back.
void handleSpriteReset(ActiveApp slot) {
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  LittleFS.remove(binPath);
  spriteRev++;
  loadCustomSpriteState();
  if (slot == APP_CLAUDE) claudeFrame = 0;
  else codexFrame = 0;
  if (currentApp == slot) drawActiveApp();
  webServer.send(200, "text/plain", "ok");
}

void handleResetWifi() {
  webServer.send(200, "text/html", "<html><body>Resetting WiFi, device will restart...</body></html>");
  delay(200);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ---------- on-device GIF decode (AnimatedGIF) ----------
// AnimatedGIF hands us the image one horizontal line at a time (via the draw
// callback) at the GIF's native resolution, so we never need a full-canvas
// buffer. We nearest-neighbour rescale into the target slot size and stream the
// result straight to the .bin one target row at a time. Because the .bin can't
// hold a whole frame in RAM to composite against, GIFs that only re-encode a
// changed sub-rectangle (the common optimizer output, disposal method 1) are
// composited by reading the *previous frame's* rows back out of the .bin we're
// writing. (Disposal method 2 "restore to background" isn't distinguished -
// uncovered pixels keep the previous frame instead of clearing; fine for the
// looping character animations this is for.)

struct GifDecodeCtx {
  int canvasW, canvasH; // GIF native size
  int targetW, targetH; // slot size we're rescaling down to
  size_t rowBytes;      // targetW * 2
  File out;             // output .bin, written sequentially
  File prevFile;        // previous frame in the .bin, read sequentially for compositing
  bool hasPrev;         // false for frame 0 (nothing to composite over -> black)
  int producedRow;      // next target row still owed for the current frame
};

static File gifReadFile; // one decode runs at a time, so a single handle is fine

void *gifOpenCB(const char *fname, int32_t *pSize) {
  gifReadFile = LittleFS.open(fname, "r");
  if (!gifReadFile) return nullptr;
  *pSize = (int32_t)gifReadFile.size();
  return (void *)&gifReadFile;
}

void gifCloseCB(void *) {
  if (gifReadFile) gifReadFile.close();
}

int32_t gifReadCB(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  File *f = (File *)pFile->fHandle;
  // AnimatedGIF's own SD example keeps this one-byte-short guard near EOF.
  if ((pFile->iSize - pFile->iPos) < iLen) iLen = pFile->iSize - pFile->iPos - 1;
  if (iLen <= 0) return 0;
  int32_t n = (int32_t)f->read(pBuf, iLen);
  pFile->iPos = (int32_t)f->position();
  return n;
}

int32_t gifSeekCB(GIFFILE *pFile, int32_t iPosition) {
  File *f = (File *)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}

// Loads the next previous-frame row into prevRowBuf (black if there's no
// previous frame). Reads are sequential and stay aligned with producedRow.
static void readPrevRow(GifDecodeCtx *ctx) {
  if (ctx->hasPrev)
    ctx->prevFile.read((uint8_t *)prevRowBuf, ctx->rowBytes);
  else
    memset(prevRowBuf, 0, ctx->rowBytes);
}

// Appends the current rowBuf as the next output row.
static void emitRow(GifDecodeCtx *ctx) {
  ctx->out.write((const uint8_t *)rowBuf, ctx->rowBytes);
  ctx->producedRow++;
}

// Emits a row that this frame doesn't touch: a straight copy of the previous
// frame (top/bottom gaps of a partial frame).
static void emitPrevRow(GifDecodeCtx *ctx) {
  readPrevRow(ctx);
  memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
  emitRow(ctx);
}

// Rescales one decoded native line into target rows, compositing over the
// previous frame, and streams every target row it can now finalize.
void gifDrawCB(GIFDRAW *pDraw) {
  GifDecodeCtx *ctx = (GifDecodeCtx *)pDraw->pUser;
  int sy = pDraw->iY + pDraw->y; // absolute source line on the GIF canvas
  if (sy < 0 || sy >= ctx->canvasH) return;

  const uint8_t *pal = pDraw->pPalette24; // RGB888, 256 entries
  const uint8_t *src = pDraw->pPixels;    // palette indices, one per pixel of this line
  bool hasTrans = pDraw->ucHasTransparency;
  uint8_t transIdx = pDraw->ucTransparent;

  // Emit every target row whose nearest source line is <= sy and isn't done yet.
  while (ctx->producedRow < ctx->targetH) {
    int ty = ctx->producedRow;
    int srcRow = (int)((long)ty * ctx->canvasH / ctx->targetH);
    if (srcRow > sy) break;                       // needs a later source line
    if (srcRow < sy) { emitPrevRow(ctx); continue; } // source line was skipped -> previous frame

    // srcRow == sy: composite this source line over the previous frame's row.
    readPrevRow(ctx);
    memcpy(rowBuf, prevRowBuf, ctx->rowBytes);
    for (int tx = 0; tx < ctx->targetW; tx++) {
      int sx = (int)((long)tx * ctx->canvasW / ctx->targetW);
      int rel = sx - pDraw->iX;
      if (rel < 0 || rel >= pDraw->iWidth) continue; // outside this frame's rect: keep previous pixel
      uint8_t idx = src[rel];
      if (hasTrans && idx == transIdx) continue;     // transparent: keep previous pixel
      uint8_t r = pal[idx * 3 + 0], g = pal[idx * 3 + 1], b = pal[idx * 3 + 2];
      uint16_t val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      rowBuf[tx] = (uint16_t)(((val & 0xFF) << 8) | (val >> 8)); // byte-swap to match convert_sprites.py
    }
    emitRow(ctx);
  }
}

// Decodes gifPath into binPath in the [count][frames...] wire format the
// display path reads. Returns false on open/decode failure.
bool decodeGifToBin(const char *gifPath, const char *binPath, int targetW, int targetH) {
  // AnimatedGIF's internal state (~24KB of LZW/line/palette buffers) is big, so
  // allocate it on the heap only for the duration of a decode rather than
  // paying for it in .bss for the whole uptime.
  AnimatedGIF *gif = new AnimatedGIF();
  if (!gif) return false;
  gif->begin(GIF_PALETTE_RGB888);
  if (!gif->open(gifPath, gifOpenCB, gifCloseCB, gifReadCB, gifSeekCB, gifDrawCB)) {
    Serial.printf("[gif] open failed err=%d\n", gif->getLastError());
    delete gif;
    return false;
  }

  GifDecodeCtx ctx;
  ctx.canvasW = gif->getCanvasWidth();
  ctx.canvasH = gif->getCanvasHeight();
  ctx.targetW = targetW;
  ctx.targetH = targetH;
  ctx.rowBytes = (size_t)targetW * 2;
  ctx.hasPrev = false;
  size_t frameBytes = (size_t)targetW * targetH * 2;

  ctx.out = LittleFS.open(binPath, "w");
  if (!ctx.out) {
    gif->close();
    delete gif;
    return false;
  }
  ctx.out.write((uint8_t)0); // placeholder frame count, patched once we know the total

  uint8_t count = 0;
  int delayMs = 0, more = 1;
  while (count < MAX_CUSTOM_FRAMES) {
    ctx.producedRow = 0;
    ctx.hasPrev = false;
    if (count > 0) {
      ctx.out.flush(); // make the just-written previous frame visible to the read handle
      ctx.prevFile = LittleFS.open(binPath, "r");
      ctx.hasPrev = (bool)ctx.prevFile;
      if (ctx.hasPrev) ctx.prevFile.seek(1 + (size_t)(count - 1) * frameBytes);
    }

    more = gif->playFrame(false, &delayMs, &ctx);

    if (more >= 0) {
      // finalize any bottom rows this frame never touched
      while (ctx.producedRow < ctx.targetH) emitPrevRow(&ctx);
      count++;
    }
    if (ctx.prevFile) ctx.prevFile.close();
    if (more <= 0) break; // 0 = last frame, <0 = decode error
    yield();              // feed the WDT between frames
  }
  gif->close();
  delete gif;
  ctx.out.close();

  if (count == 0) {
    LittleFS.remove(binPath);
    return false;
  }
  File patch = LittleFS.open(binPath, "r+");
  if (patch) {
    patch.seek(0);
    patch.write(count);
    patch.close();
  }
  Serial.printf("[gif] decoded %d frame(s) %dx%d -> %dx%d\n", count, ctx.canvasW, ctx.canvasH, targetW, targetH);
  return true;
}

// ---------- sprite upload (raw .gif -> on-device decode) ----------
// ESP8266WebServer fully buffers a plain POST body into a heap String before
// the handler runs, which a whole GIF would blow RAM on - so we take the
// upload over its streaming multipart/HTTPUpload path, writing the raw .gif to
// LittleFS in small chunks, then decode it on the done callback.
File uploadFile;

void handleSpriteUploadChunk(const char *gifPath) {
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadFile = LittleFS.open(gifPath, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

void handleSpriteUploadDone(ActiveApp slot) {
  const char *gifPath = (slot == APP_CLAUDE) ? CLAUDE_GIF_FILE : CODEX_GIF_FILE;
  const char *binPath = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_FILE : CODEX_SPRITE_FILE;
  int tw = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_W : CODEX_SPRITE_W;
  int th = (slot == APP_CLAUDE) ? CLAUDE_SPRITE_H : CODEX_SPRITE_H;

  bool ok = decodeGifToBin(gifPath, binPath, tw, th);
  LittleFS.remove(gifPath); // temp raw gif no longer needed once decoded

  spriteRev++;
  loadCustomSpriteState();
  if (slot == APP_CLAUDE) claudeFrame = 0;
  else codexFrame = 0;
  if (currentApp == slot) drawActiveApp();

  if (ok) {
    webServer.send(200, "text/plain", "ok");
    Serial.println("[sprite] gif decoded & applied");
  } else {
    webServer.send(500, "text/plain", "gif decode failed (too large or unsupported?)");
    Serial.println("[sprite] gif decode FAILED");
  }
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/reset-wifi", HTTP_POST, handleResetWifi);
  webServer.on("/api/info", HTTP_GET, handleApiInfo);
  webServer.on("/api/display", HTTP_POST, handleApiDisplay);
  webServer.on("/api/bridge", HTTP_POST, handleApiBridge);
  webServer.on("/api/brightness", HTTP_POST, handleApiBrightness);
  webServer.on("/api/rotation", HTTP_POST, handleApiRotation);
  webServer.on("/api/music-spectrum", HTTP_POST, handleApiMusicSpectrum);
  webServer.on("/api/brightness-invert", HTTP_POST, handleApiBrightnessInvert);
  webServer.on("/sprite/claude/reset", HTTP_POST, []() { handleSpriteReset(APP_CLAUDE); });
  webServer.on("/sprite/codex/reset", HTTP_POST, []() { handleSpriteReset(APP_CODEX); });
  webServer.on("/sprite/claude/raw", HTTP_GET, []() { handleSpriteRaw(APP_CLAUDE); });
  webServer.on("/sprite/codex/raw", HTTP_GET, []() { handleSpriteRaw(APP_CODEX); });
  webServer.on(
      "/sprite/claude", HTTP_POST, []() { handleSpriteUploadDone(APP_CLAUDE); },
      []() { handleSpriteUploadChunk(CLAUDE_GIF_FILE); });
  webServer.on(
      "/sprite/codex", HTTP_POST, []() { handleSpriteUploadDone(APP_CODEX); },
      []() { handleSpriteUploadChunk(CODEX_GIF_FILE); });
  webServer.begin();
  Serial.printf("[web] admin server listening on http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---------- Arduino entry points ----------

void setup() {
  Serial.setRxBufferSize(2048); // a serial #STATUS frame (~600B) must survive a slow draw
  Serial.begin(115200);
  LittleFS.begin();
  loadBridgeHost();
  loadBrightness();
  loadBrightnessInvert();
  loadCustomSpriteState();

  tft.init();
  loadDisplayConfig();
  loadSpectrumStyle();
  applyDisplayRotation();
  tft.fillScreen(TFT_BLACK);
  analogWriteFreq(BRIGHTNESS_PWM_FREQ);
  analogWriteRange(100); // duty maps 1:1 to a 0-100 percentage
  applyBrightness();

  setupWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupWebServer();
    webServerStarted = true;

    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("WiFi connected", 8, 16, 2);
    tft.drawString("Admin page:", 8, 36, 2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    // "http://" + 15-char IP overflows the 128px panel; show the bare IP.
    tft.drawString("IP: " + WiFi.localIP().toString(), 8, 56, 1);
    delay(3000);

    showMainUiIfNeeded();
    pollBridge();
  }
  // else: the config-portal screen stays up; either the user configures WiFi
  // (handled in loop) or serial #STATUS frames arrive and take the screen over
}

void loop() {
  // WiFi dropped (router/modem restart, DHCP re-lease): ESP8266's auto-rejoin
  // only runs while the SDK connection is up; after a hard drop the STA just
  // sits disconnected. Explicitly retry the join every 10s so the device
  // comes back without a reboot.
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > 10000) {
    lastWifiRetry = millis();
    Serial.println("[wifi] disconnected - reconnecting...");
    WiFi.reconnect();
  }
  wifiManager.process(); // keeps the config portal alive until WiFi is set up
  pumpSerial();          // wired (USB) bridge frames

  if (!webServerStarted && WiFi.status() == WL_CONNECTED) {
    // WiFi came up after boot (portal or slow AP); the portal has released
    // port 80 by now, so the admin server can bind it
    setupWebServer();
    webServerStarted = true;
    showMainUiIfNeeded();
    lastPollMs = 0; // poll the bridge right away
  }
  if (webServerStarted) webServer.handleClient();
  if (!mainUiShown) return; // config-portal screen is up, nothing to animate

  unsigned long nowMs = millis();

  // Effective mode may differ from the configured one (AUTO -> music while
  // audio plays). On a transition, reset the incoming mode's chrome so it
  // repaints cleanly, and repaint the pet immediately when returning to it.
  DisplayMode eff = effectiveMode();
  if (eff != lastEffectiveMode) {
    lastEffectiveMode = eff;
    if (eff == MODE_NET) {
      netChromeDrawn = false;
      lastNetPollMs = 0;
    } else if (eff == MODE_MUSIC) {
      musicChromeDrawn = false;
      lastMusicPollMs = 0;
    } else if (eff == MODE_STOCK) {
      stockChromeDrawn = false;
      lastStockPollMs = 0;
    } else if (eff == MODE_MIRROR || eff == MODE_ALBUM || eff == MODE_SPECTRUM) {
      tft.fillScreen(TFT_BLACK); // first frame arrives on the next poll
      lastMirrorPollMs = 0;
      lastAlbumPollMs = 0;
      lastSpectrumPollMs = 0;
    } else {
      updateActiveApp();
      drawActiveApp();
    }
  }

  if (eff == MODE_NET) {
    // net-speed mode: rendering (constant-rate sweep) is independent of the
    // bridge polls that refill its sample queue
    if (nowMs - lastNetDrawMs >= NET_DRAW_INTERVAL_MS) {
      lastNetDrawMs = nowMs;
      netDrawTick();
    }
    if (nowMs - lastNetPollMs >= NET_POLL_INTERVAL_MS) {
      lastNetPollMs = nowMs;
      pollNet();
    }
  } else if (eff == MODE_MUSIC) {
    // music now-playing mode: cover art + track metadata from the bridge
    if (nowMs - lastMusicPollMs >= MUSIC_POLL_INTERVAL_MS) {
      lastMusicPollMs = nowMs;
      pollMusic();
    }
  } else if (eff == MODE_STOCK) {
    // stock watchlist: HTTP poll unless the serial link is pushing #STOCK
    if (nowMs - lastStockPollMs >= STOCK_POLL_INTERVAL_MS) {
      lastStockPollMs = nowMs;
      if (!wiredActive()) pollStock();
    }
    if (!stockChromeDrawn || stockDirty) drawStockScreen();
  } else if (eff == MODE_MIRROR) {
    // desktop mirror: fetch the latest scaled RGB565 frame from the bridge
    if (nowMs - lastMirrorPollMs >= MIRROR_POLL_INTERVAL_MS) {
      lastMirrorPollMs = nowMs;
      pollMirror();
    }
  } else if (eff == MODE_ALBUM) {
    if (nowMs - lastAlbumPollMs >= ALBUM_POLL_INTERVAL_MS) {
      lastAlbumPollMs = nowMs;
      pollAlbum();
    }
  } else if (eff == MODE_SPECTRUM) {
    if (nowMs - lastSpectrumPollMs >= SPECTRUM_POLL_INTERVAL_MS) {
      lastSpectrumPollMs = nowMs;
      pollSpectrum();
    }
  } else {
    // sprite walk-cycle animation (only advances while that app is showing)
    if (nowMs - lastAnimMs >= ANIM_INTERVAL_MS) {
      lastAnimMs = nowMs;
      bool claudeWorking = claudeStatus.status == "working";
      bool codexWorking = codexStatus.status == "working";
      if (showingCd != CD_NONE) {
        // countdown owns the center area: no sprite frames over it
      } else if (currentApp == APP_CLAUDE && claudeWorking) {
        claudeFrame = (claudeFrame + 1) % claudeFrameCount();
        drawClaudeSprite(claudeFrame);
      } else if (currentApp == APP_CODEX && codexWorking) {
        codexFrame = (codexFrame + 1) % codexFrameCount();
        drawCodexSprite(codexFrame);
      }
    }

    // countdown seconds tick locally between bridge polls
    static unsigned long lastCdTickMs = 0;
    if (showingCd != CD_NONE && nowMs - lastCdTickMs >= 1000) {
      lastCdTickMs = nowMs;
      drawCountdown(false);
    }

    // "urgent" flash toggle (independent, faster cadence)
    if (nowMs - lastFlashMs >= FLASH_INTERVAL_MS) {
      lastFlashMs = nowMs;
      flashOn = !flashOn;
      if (bridgeStale()) {
        redrawRingOnly();
      } else if (currentAppNeedsInput()) {
        // approval needed: blink the whole border red, restore the quota ring
        // on the off-phase so it doesn't erase the normal chrome permanently
        if (flashOn) drawFullBorder(TFT_RED);
        else redrawRingOnly();
      }
    }

    // alternate which app is shown when neither/both are uniquely working
    if (updateActiveApp()) {
      drawActiveApp();
    }
  }

  // status poll continues in every mode (feeds /api/info and the web page).
  // Wired-first: while serial frames are flowing, skip HTTP polling entirely
  // (works around AP client isolation, and avoids double updates).
  if (nowMs - lastPollMs >= BRIDGE_POLL_INTERVAL_MS) {
    lastPollMs = nowMs;
    if (!wiredActive()) pollBridge();
  }
}
