/*
  ESP32 VGA Clock
  ---------------
  - Digital clock with 14-segment style digits, or an analogue railway-station style clock
  - Web configuration UI (http://<hostname>.local or the IP shown on screen)
  - Firmware update via the web UI and via ArduinoOTA (Arduino IDE "network port")
  - Wi-Fi: WPA/WPA2-Personal or WPA2-Enterprise (PEAP / TTLS with MSCHAPv2, e.g. eduroam)
  - NTP with a POSIX time-zone string, optional DS3231 / DS1307 RTC (stores UTC)

  Target  : classic ESP32 (WROOM/WROVER). Not the S2/S3/C3: FabGL needs the I2S LCD mode of the classic ESP32.
  Core    : arduino-esp32 2.0.x (FabGL does not build on 3.x)
  Library : FabGL 1.0.9
  Board   : "ESP32 Dev Module", Partition scheme "Minimal SPIFFS (1.9MB APP with OTA)"

  VGA wiring (64-colour, 2 bits per channel; resistors only, no ICs):
     GPIO5  --390R--+                      GPIO15 ------------- VGA 13 (HSync)
     GPIO4  --820R--+-- VGA 1 (Red)        GPIO23 ------------- VGA 14 (VSync)
     GPIO19 --390R--+                      GND    ------------- VGA 5,6,7,8,10
     GPIO18 --820R--+-- VGA 2 (Green)
     GPIO22 --390R--+
     GPIO21 --820R--+-- VGA 3 (Blue)
  (390/820 ohm into the monitor's 75 ohm termination gives ~0.7 V full scale.)

  First boot: no Wi-Fi configured -> access point "VGA-Clock-XXXX", password "vgaclock",
  browse to http://192.168.4.1, login admin / admin (change it!).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_idf_version.h"

// ESP-IDF 4.4 (arduino-esp32 2.0.x) vs ESP-IDF 5.x (arduino-esp32 3.x) API differences
#if ESP_IDF_VERSION_MAJOR >= 5
  #include "esp_eap_client.h"
  #define SNTP_ENABLED()            esp_sntp_enabled()
  #define SNTP_STOP()               esp_sntp_stop()
  #define EAP_SET_IDENTITY(p, l)    esp_eap_client_set_identity(p, l)
  #define EAP_SET_USERNAME(p, l)    esp_eap_client_set_username(p, l)
  #define EAP_SET_PASSWORD(p, l)    esp_eap_client_set_password(p, l)
  #define EAP_SET_CA_CERT(p, l)     esp_eap_client_set_ca_cert(p, l)
  #define EAP_DISABLE_TIME_CHECK(b) esp_eap_client_set_disable_time_check(b)
  #define EAP_ENABLE()              esp_wifi_sta_enterprise_enable()
  #define EAP_DISABLE()             esp_wifi_sta_enterprise_disable()
#else
  #include "esp_wpa2.h"
  #define SNTP_ENABLED()            sntp_enabled()
  #define SNTP_STOP()               sntp_stop()
  #define EAP_SET_IDENTITY(p, l)    esp_wifi_sta_wpa2_ent_set_identity(p, l)
  #define EAP_SET_USERNAME(p, l)    esp_wifi_sta_wpa2_ent_set_username(p, l)
  #define EAP_SET_PASSWORD(p, l)    esp_wifi_sta_wpa2_ent_set_password(p, l)
  #define EAP_SET_CA_CERT(p, l)     esp_wifi_sta_wpa2_ent_set_ca_cert(p, l)
  #define EAP_DISABLE_TIME_CHECK(b) esp_wifi_sta_wpa2_ent_set_disable_time_check(b)
  #define EAP_ENABLE()              esp_wifi_sta_wpa2_ent_enable()
  #define EAP_DISABLE()             esp_wifi_sta_wpa2_ent_disable()
#endif
#include "fabgl.h"

// ------------------------------------------------------------------ hardware
// Pinout per final schematic (x1 = MSB via 390R, x0 = LSB via 820R)
static const gpio_num_t PIN_R1 = GPIO_NUM_5,  PIN_R0 = GPIO_NUM_4;
static const gpio_num_t PIN_G1 = GPIO_NUM_19, PIN_G0 = GPIO_NUM_18;
static const gpio_num_t PIN_B1 = GPIO_NUM_22, PIN_B0 = GPIO_NUM_21;
static const gpio_num_t PIN_HS = GPIO_NUM_15, PIN_VS = GPIO_NUM_23;

static const char *AP_PASS   = "vgaclock";
static const char *ADMIN_USR = "admin";

// ------------------------------------------------------------------ video modes
// squarePx: mode is meant for a widescreen panel with square pixels (otherwise a 4:3 screen is assumed)
struct ResMode { const char *name; const char *modeline; bool allow2bpp; bool squarePx; };
static const ResMode RES[] = {
  { "320x240 @60Hz",            "\"320x240@60Hz\" 12.5875 320 328 376 400 240 245 246 262 -HSync -VSync DoubleScan", true  },
  { "400x300 @60Hz",            "\"400x300@60Hz\" 20 400 420 484 528 300 301 303 314 +HSync +VSync DoubleScan",      true  },
  { "512x384 @60Hz",            "\"512x384@60Hz\" 32.5 512 524 592 672 384 385 388 403 -HSync -VSync DoubleScan",    true  },
  { "640x350 @70Hz (EGA)",      "\"640x350@70Hz\" 25.175 640 656 752 800 350 387 389 449 +HSync -VSync",             true  },
  { "640x400 @70Hz",            "\"640x400@70Hz\" 25.175 640 656 752 800 400 412 414 449 -HSync +VSync",             true  },
  { "640x480 @60Hz",            "\"640x480@60Hz\" 25.175 640 656 752 800 480 490 492 525 -HSync -VSync",             true  },
  { "720x400 @70Hz (text)",     "\"720x400@70Hz\" 28.322 720 738 846 900 400 412 414 449 -HSync +VSync",             true  },
  { "800x600 @56Hz",            "\"800x600@56Hz\" 36 800 824 896 1024 600 601 603 625 +HSync +VSync",                true  },
  { "800x600 @60Hz",            "\"800x600@60Hz\" 40 800 840 968 1056 600 601 605 628 +HSync +VSync",                true  },
  { "1024x768 @60Hz (2 colours)", "\"1024x768@60Hz\" 65 1024 1048 1184 1344 768 771 777 806 -HSync -VSync",          false },
  // Widescreen (WXGA / HD). Frame buffer at 1 bpp is 115-130 KB, so these are 2-colour only.
  // Reduced-blanking (CVT-RB / DMT-RB) timings keep the pixel clock under ~75 MHz.
  { "1280x720 @60Hz HD (2 colours)",    "\"1280x720@60Hz\" 74.48 1280 1468 1604 1664 720 721 724 746 +HSync +VSync",       false, true },
  { "1280x768 @60Hz WXGA (2 colours)",  "\"1280x768@60Hz\" 68.25 1280 1328 1360 1440 768 771 778 790 +HSync -VSync",       false, true },
  { "1280x768 @50Hz WXGA (2 colours)",  "\"1280x768@50Hz\" 64.05 1280 1312 1552 1584 768 784 791 807 -HSync -VSync",       false, true },
  { "1280x800 @60Hz WXGA (2 colours)",  "\"1280x800@60Hz\" 71 1280 1328 1360 1440 800 803 809 823 +HSync -VSync",          false, true },
  { "960x540 @60Hz qHD 16:9",           "\"960x540@60Hz\" 37.26 960 976 1008 1104 540 542 548 563 +HSync +VSync",        true,  true },
  { "1360x768 @60Hz WXGA (2 colours)",  "\"1360x768@60Hz\" 72 1360 1408 1440 1520 768 771 776 790 +HSync -VSync",          false, true },
};
static const int NUM_RES = sizeof(RES) / sizeof(RES[0]);

// Physical shape of the picture on the monitor, used to keep the dial round and digits upright
// when the pixels are not square. 0 = auto: 4:3 for classic modes, square pixels for widescreen ones.
static const char *ASPECT_NAMES[] = { "Auto", "4:3", "5:4", "3:2", "16:10", "16:9", "21:9", "Square pixels" };
static const float ASPECT_VALUES[] = { 0, 4.0f / 3, 5.0f / 4, 3.0f / 2, 16.0f / 10, 16.0f / 9, 64.0f / 27, -1 };
static const int NUM_ASPECT = sizeof(ASPECT_VALUES) / sizeof(ASPECT_VALUES[0]);

enum { MODE_DIGITAL = 0, MODE_ANALOG = 1 };

// ------------------------------------------------------------------ configuration
struct Config {
  // display
  uint8_t  mode    = MODE_DIGITAL;
  uint8_t  res     = 5;           // 640x480
  uint8_t  aspect  = 0;           // index into ASPECT_VALUES
  uint32_t color   = 0xFFAA00;    // digits
  bool     ghost   = true;        // dim unlit segments
  bool     h24     = true;
  bool     seconds = false;       // digital: show seconds / analogue: second hand
  bool     blink   = true;
  uint32_t face    = 0xFFFFFF;    // analogue dial
  uint32_t hand    = 0x000000;    // analogue hour/minute hands + ticks
  uint32_t sec     = 0xFF0000;    // analogue second hand
  bool     jump    = true;        // minute hand jumps once per minute
  // time
  String   tz      = "CET-1CEST,M3.5.0,M10.5.0/3";
  bool     ntpOn   = true;
  String   ntp1    = "pool.ntp.org";
  String   ntp2    = "time.nist.gov";
  // RTC
  bool     rtcOn   = false;
  uint8_t  rtcType = 0;           // 0 = DS3231, 1 = DS1307
  int8_t   sda     = 32;
  int8_t   scl     = 33;
  // Wi-Fi
  bool     ent     = false;
  String   ssid, psk;
  String   eapId, eapUser, eapPass, eapCA;
  // system
  String   host    = "vgaclock";
  String   admin   = "admin";
} cfg;

Preferences prefs;

void loadConfig() {
  prefs.begin("vgaclock", true);
  cfg.mode    = prefs.getUChar("mode", cfg.mode);
  cfg.res     = prefs.getUChar("res", cfg.res);
  cfg.aspect  = prefs.getUChar("aspect", cfg.aspect);
  if (cfg.aspect >= NUM_ASPECT) cfg.aspect = 0;
  if (cfg.res >= NUM_RES) cfg.res = 5;
  cfg.color   = prefs.getUInt("color", cfg.color);
  cfg.ghost   = prefs.getBool("ghost", cfg.ghost);
  cfg.h24     = prefs.getBool("h24", cfg.h24);
  cfg.seconds = prefs.getBool("secs", cfg.seconds);
  cfg.blink   = prefs.getBool("blink", cfg.blink);
  cfg.face    = prefs.getUInt("face", cfg.face);
  cfg.hand    = prefs.getUInt("hand", cfg.hand);
  cfg.sec     = prefs.getUInt("sec", cfg.sec);
  cfg.jump    = prefs.getBool("jump", cfg.jump);
  cfg.tz      = prefs.getString("tz", cfg.tz);
  cfg.ntpOn   = prefs.getBool("ntp", cfg.ntpOn);
  cfg.ntp1    = prefs.getString("ntp1", cfg.ntp1);
  cfg.ntp2    = prefs.getString("ntp2", cfg.ntp2);
  cfg.rtcOn   = prefs.getBool("rtc", cfg.rtcOn);
  cfg.rtcType = prefs.getUChar("rtct", cfg.rtcType);
  cfg.sda     = prefs.getChar("sda", cfg.sda);
  cfg.scl     = prefs.getChar("scl", cfg.scl);
  cfg.ent     = prefs.getBool("ent", cfg.ent);
  cfg.ssid    = prefs.getString("ssid", "");
  cfg.psk     = prefs.getString("psk", "");
  cfg.eapId   = prefs.getString("eapid", "");
  cfg.eapUser = prefs.getString("eapuser", "");
  cfg.eapPass = prefs.getString("eappass", "");
  size_t n = prefs.getBytesLength("eapca");          // stored as blob: PEM chains can exceed the 4000-byte NVS string limit
  if (n) {
    char *b = (char *)malloc(n + 1);
    if (b) { prefs.getBytes("eapca", b, n); b[n] = 0; cfg.eapCA = b; free(b); }
  }
  cfg.host    = prefs.getString("host", cfg.host);
  cfg.admin   = prefs.getString("admin", cfg.admin);
  prefs.end();
}

void saveConfig() {
  prefs.begin("vgaclock", false);
  prefs.putUChar("mode", cfg.mode);
  prefs.putUChar("res", cfg.res);
  prefs.putUChar("aspect", cfg.aspect);
  prefs.putUInt("color", cfg.color);
  prefs.putBool("ghost", cfg.ghost);
  prefs.putBool("h24", cfg.h24);
  prefs.putBool("secs", cfg.seconds);
  prefs.putBool("blink", cfg.blink);
  prefs.putUInt("face", cfg.face);
  prefs.putUInt("hand", cfg.hand);
  prefs.putUInt("sec", cfg.sec);
  prefs.putBool("jump", cfg.jump);
  prefs.putString("tz", cfg.tz);
  prefs.putBool("ntp", cfg.ntpOn);
  prefs.putString("ntp1", cfg.ntp1);
  prefs.putString("ntp2", cfg.ntp2);
  prefs.putBool("rtc", cfg.rtcOn);
  prefs.putUChar("rtct", cfg.rtcType);
  prefs.putChar("sda", cfg.sda);
  prefs.putChar("scl", cfg.scl);
  prefs.putBool("ent", cfg.ent);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("psk", cfg.psk);
  prefs.putString("eapid", cfg.eapId);
  prefs.putString("eapuser", cfg.eapUser);
  prefs.putString("eappass", cfg.eapPass);
  if (cfg.eapCA.length()) prefs.putBytes("eapca", cfg.eapCA.c_str(), cfg.eapCA.length());
  else prefs.remove("eapca");
  prefs.putString("host", cfg.host);
  prefs.putString("admin", cfg.admin);
  prefs.end();
}

// ------------------------------------------------------------------ globals
WebServer server(80);

fabgl::VGA2Controller *vga2 = nullptr;   // 1 bpp: 2 colours
fabgl::VGA4Controller *vga4 = nullptr;   // 2 bpp: 4 colours
fabgl::Canvas *canvas = nullptr;
int     scrW = 0, scrH = 0;
uint8_t bpp = 1;
float   aspY = 1.0f;                     // vertical scale: physical pixel height / width
bool    videoOff = false;

fabgl::RGB888 cBg(0, 0, 0), cOn, cGhost, cFace, cHand, cSec, cText;

String   apSsid;
bool     apActive = false, staUp = false;
volatile uint8_t wifiReason = 0;         // last STA disconnect reason (0 = none)
String   wifiScanInfo = "not scanned";   // what the boot scan saw of the configured SSID
uint32_t lastStaUp = 0, connectedAt = 0, lastRetry = 0;

volatile bool ntpEvent = false;
time_t   lastNtp = 0;
String   timeSrc = "none";
bool     rtcFound = false;
uint32_t lastRtcResync = 0;

uint32_t restartAt = 0;
bool     uploadAuth = false, uploadOK = false;

char ntpBuf1[64], ntpBuf2[64];           // lwIP SNTP keeps the pointers, so they must stay valid

// ------------------------------------------------------------------ colours
static uint8_t lvl(uint8_t v) { return (v + 42) / 85; }   // 0..3, the DAC has 4 levels per channel

static fabgl::RGB888 snapColor(uint32_t rgb, bool nonBlack) {
  uint8_t r = lvl(rgb >> 16), g = lvl((rgb >> 8) & 0xFF), b = lvl(rgb & 0xFF);
  if (nonBlack && !(r | g | b)) r = g = b = 1;
  return fabgl::RGB888(r * 85, g * 85, b * 85);
}

static void setPal(int idx, const fabgl::RGB888 &c) {
  if (vga4) vga4->setPaletteItem(idx, c);
  else if (vga2 && idx < 2) vga2->setPaletteItem(idx, c);
}

// FabGL maps a drawing colour to a palette index through a lookup table that is built
// once (in setResolution) from the *default* palette, and setPaletteItem() does not rebuild it.
// So we always draw with the default-palette "key" colours and only change what each index
// actually outputs with setPaletteItem().
//   VGA4 default: 0 black, 1 blue, 2 green, 3 white      VGA2 default: 0 black, 1 white
static fabgl::RGB888 keyColor(int idx) {
  if (bpp == 1) return idx ? fabgl::RGB888(255, 255, 255) : fabgl::RGB888(0, 0, 0);
  static const fabgl::RGB888 k[4] = { {0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 255} };
  return k[idx & 3];
}

static bool ghostActive = false;

void setupColors() {
  fabgl::RGB888 black(0, 0, 0);
  if (cfg.mode == MODE_DIGITAL) {
    fabgl::RGB888 on = snapColor(cfg.color, true);
    // dim version: one DAC step on every channel that is at least half bright
    uint8_t gr = on.R >= 170 ? 85 : 0, gg = on.G >= 170 ? 85 : 0, gb = on.B >= 170 ? 85 : 0;
    ghostActive = (bpp == 2) && cfg.ghost && (gr | gg | gb);
    setPal(0, black); setPal(1, on); setPal(2, ghostActive ? fabgl::RGB888(gr, gg, gb) : black); setPal(3, black);
    cBg = keyColor(0); cOn = keyColor(1); cGhost = keyColor(ghostActive ? 2 : 0);
    cText = cOn;
  } else {
    setPal(0, black);
    setPal(1, snapColor(cfg.face, true));
    setPal(2, snapColor(cfg.hand, false));
    setPal(3, snapColor(cfg.sec, false));
    cBg = keyColor(0); cFace = keyColor(1);
    // 2-colour mode: hands, ticks and second hand are drawn in the background colour (index 0)
    cHand = keyColor(bpp == 2 ? 2 : 0);
    cSec  = keyColor(bpp == 2 ? 3 : 0);
    cText = cFace;
  }
}

// ------------------------------------------------------------------ video
static uint8_t neededBpp(const Config &c) {
  return (RES[c.res].allow2bpp && (c.mode == MODE_ANALOG || c.ghost)) ? 2 : 1;
}

void computeAspect() {
  float a = ASPECT_VALUES[cfg.aspect];
  if (a == 0) a = RES[cfg.res].squarePx ? -1 : 4.0f / 3;     // auto
  aspY = (a < 0) ? 1.0f : ((float)scrH / scrW) * a;          // a pixel is aspY times as tall as wide
}

void startVideo() {
  const ResMode &r = RES[cfg.res];
  bpp = neededBpp(cfg);
  fabgl::BitmappedDisplayController *d;
  if (bpp == 2) {
    vga4 = new fabgl::VGA4Controller;
    vga4->begin(PIN_R1, PIN_R0, PIN_G1, PIN_G0, PIN_B1, PIN_B0, PIN_HS, PIN_VS);
    vga4->setResolution(r.modeline);
    d = vga4;
  } else {
    vga2 = new fabgl::VGA2Controller;
    vga2->begin(PIN_R1, PIN_R0, PIN_G1, PIN_G0, PIN_B1, PIN_B0, PIN_HS, PIN_VS);
    vga2->setResolution(r.modeline);
    d = vga2;
  }
  canvas = new fabgl::Canvas(d);
  scrW = canvas->getWidth();
  scrH = canvas->getHeight();
  Serial.printf("Video %s, %d bpp, free heap %u\n", r.name, bpp, ESP.getFreeHeap());
}

void stopVideo() {               // before flash writes (OTA)
  if (videoOff) return;
  videoOff = true;
  if (vga4) vga4->end();
  if (vga2) vga2->end();
}

// ------------------------------------------------------------------ 14-segment digital clock
// Segment bit order: 0 A(top) 1 B 2 C 3 D(bottom) 4 E 5 F 6 G1 7 G2
//                    8 H(UL diag) 9 I(upper centre) 10 J(UR diag) 11 K(LL diag) 12 L(lower centre) 13 M(LR diag)
static const uint16_t DIG[10] = { 0x0C3F, 0x0406, 0x00DB, 0x008F, 0x00E6, 0x00ED, 0x00FD, 0x0007, 0x00FF, 0x00EF };
static const uint16_t GL_A = 0x00F7, GL_P = 0x00F3, GL_M = 0x0536, GL_DASH = 0x00C0;

static const int MAX_CELLS = 8;                 // 6 digits + "AM"/"PM" (2 letters)
static fabgl::Point segPts[MAX_CELLS][14][6];
static uint16_t shown[MAX_CELLS];
static int      nDigits = 4, nColons = 1;
static struct { int x1, y1, x2, y2; } colonDot[2][2];
static int      colonShown = -1;

static void P(fabgl::Point &p, float x, float y) { p.X = lroundf(x); p.Y = lroundf(y); }

static void hseg(fabgl::Point *p, float xa, float xb, float yc, float hx, float hy) {
  P(p[0], xa, yc); P(p[1], xa + hx, yc - hy); P(p[2], xb - hx, yc - hy);
  P(p[3], xb, yc); P(p[4], xb - hx, yc + hy); P(p[5], xa + hx, yc + hy);
}
static void vseg(fabgl::Point *p, float xc, float ya, float yb, float hx, float hy) {
  P(p[0], xc, ya); P(p[1], xc + hx, ya + hy); P(p[2], xc + hx, yb - hy);
  P(p[3], xc, yb); P(p[4], xc - hx, yb - hy); P(p[5], xc - hx, ya + hy);
}
// thick diagonal clipped to its box: top-left -> bottom-right
static void diagDR(fabgl::Point *p, float x1, float y1, float x2, float y2, float dx, float dy) {
  P(p[0], x1, y1); P(p[1], x1 + dx, y1); P(p[2], x2, y2 - dy);
  P(p[3], x2, y2); P(p[4], x2 - dx, y2); P(p[5], x1, y1 + dy);
}
// top-right -> bottom-left
static void diagDL(fabgl::Point *p, float xr, float y1, float xl, float y2, float dx, float dy) {
  P(p[0], xr, y1); P(p[1], xr, y1 + dy); P(p[2], xl + dx, y2);
  P(p[3], xl, y2); P(p[4], xl, y2 - dy); P(p[5], xr - dx, y1);
}

static void buildCell(int c, float x, float y, float w, float h) {
  float tx = max(2.0f, w * 0.15f), ty = max(2.0f, tx * aspY);
  float hx = tx / 2, hy = ty / 2;
  float gx = max(1.0f, tx * 0.18f), gy = max(1.0f, gx * aspY);
  float L = x + hx, R = x + w - hx, T = y + hy, B = y + h - hy, M = y + h / 2, XM = x + w / 2;
  fabgl::Point (*s)[6] = segPts[c];
  hseg(s[0], L + gx, R - gx, T, hx, hy);            // A
  vseg(s[1], R, T + gy, M - gy, hx, hy);            // B
  vseg(s[2], R, M + gy, B - gy, hx, hy);            // C
  hseg(s[3], L + gx, R - gx, B, hx, hy);            // D
  vseg(s[4], L, M + gy, B - gy, hx, hy);            // E
  vseg(s[5], L, T + gy, M - gy, hx, hy);            // F
  hseg(s[6], L + gx, XM - gx, M, hx, hy);           // G1
  hseg(s[7], XM + gx, R - gx, M, hx, hy);           // G2
  vseg(s[9],  XM, T + hy + gy, M - gy, hx, hy);     // I
  vseg(s[12], XM, M + gy, B - hy - gy, hx, hy);     // L
  float ulx1 = L + hx + gx, ulx2 = XM - hx - gx, urx1 = XM + hx + gx, urx2 = R - hx - gx;
  float uy1 = T + hy + gy, uy2 = M - hy - gy, ly1 = M + hy + gy, ly2 = B - hy - gy;
  float dx = min(tx * 0.9f, (ulx2 - ulx1) * 0.5f), dy = min(ty * 0.9f, (uy2 - uy1) * 0.5f);
  diagDR(s[8],  ulx1, uy1, ulx2, uy2, dx, dy);      // H
  diagDL(s[10], urx2, uy1, urx1, uy2, dx, dy);      // J
  diagDL(s[11], ulx2, ly1, ulx1, ly2, dx, dy);      // K
  diagDR(s[13], urx1, ly1, urx2, ly2, dx, dy);      // M
}

void layoutDigital() {
  nDigits = cfg.seconds ? 6 : 4;
  nColons = nDigits / 2 - 1;
  bool ampm = !cfg.h24;
  // width units: digit = 1, gap inside a pair = 0.2, colon = 0.5, AM/PM block = 1.25
  float units = nDigits + (nDigits / 2) * 0.2f + nColons * 0.5f + (ampm ? 1.25f : 0);
  float dw = min(scrW * 0.92f / units, scrH * 0.6f / (1.8f * aspY));
  float dh = dw * 1.8f * aspY;
  float x = (scrW - units * dw) / 2, y = (scrH - dh) / 2 - scrH * 0.03f;
  float dot = max(2.0f, dw * 0.17f);
  int col = 0;
  for (int i = 0; i < nDigits; i++) {
    buildCell(i, x, y, dw, dh);
    x += dw;
    if (i % 2 == 0) x += 0.2f * dw;
    else if (i < nDigits - 1) {
      float cx = x + 0.25f * dw;
      for (int k = 0; k < 2; k++) {
        float cy = y + dh * (k ? 0.68f : 0.32f);
        colonDot[col][k] = { (int)lroundf(cx - dot / 2), (int)lroundf(cy - dot * aspY / 2),
                             (int)lroundf(cx + dot / 2), (int)lroundf(cy + dot * aspY / 2) };
      }
      col++;
      x += 0.5f * dw;
    }
  }
  if (ampm) {
    x += 0.25f * dw;
    float sw = 0.45f * dw;
    buildCell(6, x, y, sw, sw * 1.8f * aspY);
    x += sw + 0.1f * dw;
    buildCell(7, x, y, sw, sw * 1.8f * aspY);
  }
  for (int i = 0; i < MAX_CELLS; i++) shown[i] = 0xFFFF;
  colonShown = -1;
}

static void drawCell(int c, uint16_t mask) {
  if (shown[c] == mask) return;
  shown[c] = mask;
  for (int s = 0; s < 14; s++) {
    canvas->setBrushColor((mask >> s) & 1 ? cOn : cGhost);
    canvas->fillPath(segPts[c][s], 6);
  }
}

static void drawColons(bool on) {
  if (colonShown == (int)on) return;
  colonShown = on;
  canvas->setBrushColor(on ? cOn : cGhost);
  for (int c = 0; c < nColons; c++)
    for (int k = 0; k < 2; k++)
      canvas->fillRectangle(colonDot[c][k].x1, colonDot[c][k].y1, colonDot[c][k].x2, colonDot[c][k].y2);
}

void renderDigital(const struct tm &lt, bool valid) {
  uint16_t g[MAX_CELLS] = { 0 };
  if (!valid) {
    for (int i = 0; i < nDigits; i++) g[i] = GL_DASH;
  } else {
    int h = lt.tm_hour;
    if (!cfg.h24) { h %= 12; if (!h) h = 12; }
    g[0] = (!cfg.h24 && h < 10) ? 0 : DIG[h / 10];
    g[1] = DIG[h % 10];
    g[2] = DIG[lt.tm_min / 10];
    g[3] = DIG[lt.tm_min % 10];
    g[4] = DIG[lt.tm_sec / 10];
    g[5] = DIG[lt.tm_sec % 10];
    g[6] = lt.tm_hour >= 12 ? GL_P : GL_A;
    g[7] = GL_M;
  }
  for (int i = 0; i < nDigits; i++) drawCell(i, g[i]);
  if (!cfg.h24) { drawCell(6, g[6]); drawCell(7, g[7]); }
  drawColons(!valid || !cfg.blink || (lt.tm_sec % 2 == 0));
}

// ------------------------------------------------------------------ analogue station clock
static float acx, acy, aR;
static fabgl::Point tickPts[60][4];
static fabgl::Point handPts[2][3][4];      // double buffer: [set][hour, minute, second]
static int  handSet = 0;
static bool handsDrawn = false, secDrawn = false;
static long handKey = -1;

// rotated quad: angle in degrees (0 = 12 o'clock), radii/half-widths in units of the dial radius
static void rotQuad(fabgl::Point *p, float ang, float r0, float r1, float w0, float w1) {
  float a = ang * (float)PI / 180.0f;
  float ux = sinf(a), uy = -cosf(a), px = cosf(a), py = sinf(a);
  float minW = 0.6f / aR;                           // keep at least ~1 px wide
  w0 = max(w0, minW); w1 = max(w1, minW);
  auto Q = [&](fabgl::Point &o, float r, float w) {
    P(o, acx + (ux * r + px * w) * aR, acy + (uy * r + py * w) * aR * aspY);
  };
  Q(p[0], r0, -w0); Q(p[1], r1, -w1); Q(p[2], r1, w1); Q(p[3], r0, w0);
}

static void drawTicks() {
  canvas->setBrushColor(cHand);
  for (int i = 0; i < 60; i++) canvas->fillPath(tickPts[i], 4);
}

void layoutAnalog() {
  acx = scrW / 2.0f;
  acy = (scrH - 12) / 2.0f;
  aR  = min(scrW / 2.0f, (scrH - 16) / 2.0f / aspY) * 0.94f;
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) rotQuad(tickPts[i], i * 6, 0.72f, 0.95f, 0.040f, 0.040f);   // hour bars
    else            rotQuad(tickPts[i], i * 6, 0.87f, 0.95f, 0.013f, 0.013f);   // minute ticks
  }
  canvas->setBrushColor(cHand);                    // rim
  canvas->fillEllipse(acx, acy, aR * 2.08f, aR * 2.08f * aspY);
  canvas->setBrushColor(cFace);                    // dial
  canvas->fillEllipse(acx, acy, aR * 2.0f, aR * 2.0f * aspY);
  drawTicks();
  handsDrawn = secDrawn = false;
  handKey = -1;
}

void renderAnalog(const struct tm &lt, bool valid) {
  long key = valid ? (lt.tm_hour * 3600L + lt.tm_min * 60L + (cfg.seconds || !cfg.jump ? lt.tm_sec : 0)) : -2;
  if (key == handKey) return;
  handKey = key;

  // erase previous hands with dial colour, then restore the ticks they covered
  if (handsDrawn) {
    fabgl::Point (*o)[4] = handPts[handSet];
    canvas->setBrushColor(cFace);
    canvas->fillPath(o[0], 4);
    canvas->fillPath(o[1], 4);
    if (secDrawn) canvas->fillPath(o[2], 4);
    canvas->fillEllipse(acx, acy, aR * 0.12f, aR * 0.12f * aspY);
    drawTicks();
    handsDrawn = secDrawn = false;
  }
  if (!valid) { canvas->waitCompletion(); return; }

  handSet ^= 1;
  fabgl::Point (*n)[4] = handPts[handSet];
  float am = lt.tm_min * 6.0f + (cfg.jump ? 0 : lt.tm_sec * 0.1f);
  float ah = (lt.tm_hour % 12) * 30.0f + lt.tm_min * 0.5f;
  float as = lt.tm_sec * 6.0f;
  rotQuad(n[0], ah, -0.20f, 0.62f, 0.048f, 0.040f);  // hour
  rotQuad(n[1], am, -0.20f, 0.90f, 0.036f, 0.028f);  // minute
  canvas->setBrushColor(cHand);
  canvas->fillPath(n[0], 4);
  canvas->fillPath(n[1], 4);
  canvas->fillEllipse(acx, acy, aR * 0.10f, aR * 0.10f * aspY);
  if (cfg.seconds) {
    rotQuad(n[2], as, -0.28f, 0.92f, 0.010f, 0.010f);
    canvas->setBrushColor(cSec);
    canvas->fillPath(n[2], 4);
    canvas->fillEllipse(acx, acy, aR * 0.05f, aR * 0.05f * aspY);
    secDrawn = true;
  }
  handsDrawn = true;
  canvas->waitCompletion();   // point buffers are reused next time; make sure they've been consumed
}

// ------------------------------------------------------------------ screen
static String statusShown = "\x01";

void drawStatus(const String &s) {
  if (videoOff || s == statusShown) return;
  statusShown = s;
  canvas->setBrushColor(cBg);
  canvas->fillRectangle(0, scrH - 11, scrW - 1, scrH - 1);
  if (!s.length()) return;
  int maxc = scrW / 8;
  String t = (int)s.length() > maxc ? s.substring(0, maxc) : s;
  canvas->selectFont(&fabgl::FONT_8x8);
  canvas->setPenColor(cText);
  canvas->drawText((scrW - (int)t.length() * 8) / 2, scrH - 10, t.c_str());
}

void relayout() {
  if (videoOff) return;
  computeAspect();
  setupColors();
  canvas->setBrushColor(cBg);
  canvas->clear();
  statusShown = "\x01";
  if (cfg.mode == MODE_DIGITAL) layoutDigital(); else layoutAnalog();
}

static bool timeValid() { return time(nullptr) > 1700000000; }

void renderClock() {
  if (videoOff) return;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  if (cfg.mode == MODE_DIGITAL) renderDigital(lt, timeValid());
  else renderAnalog(lt, timeValid());
}

String computeStatus() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (apActive && !up) {
    if (cfg.ssid.length() && wifiReason && (millis() / 4000) % 2)
      return "Wi-Fi \"" + cfg.ssid + "\": " + wifiReasonText(wifiReason);
    return "Setup: Wi-Fi \"" + apSsid + "\" pw " + AP_PASS + "  http://192.168.4.1";
  }
  if (!up) return timeValid() ? "" : (cfg.ssid.length() ? "Wi-Fi: connecting to " + cfg.ssid : "No Wi-Fi configured");
  String ip = WiFi.localIP().toString();
  if (!timeValid()) return "IP " + ip + "  waiting for NTP";
  if (millis() - connectedAt < 60000) return "http://" + cfg.host + ".local  " + ip;
  return "";
}

// ------------------------------------------------------------------ RTC (DS3231 / DS1307, stores UTC)
static const uint8_t RTC_ADDR = 0x68;
static uint8_t bcd2b(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t b2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int64_t)doe - 719468;
}

static uint8_t rtcReadReg(uint8_t reg) {
  Wire.beginTransmission(RTC_ADDR); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)1) != 1) return 0xFF;
  return Wire.read();
}

bool rtcProbe() {
  Wire.beginTransmission(RTC_ADDR);
  return Wire.endTransmission() == 0;
}

bool rtcRead(time_t &out) {
  Wire.beginTransmission(RTC_ADDR); Wire.write(0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (cfg.rtcType == 1 && (r[0] & 0x80)) return false;              // DS1307 clock halted
  if (cfg.rtcType == 0 && (rtcReadReg(0x0F) & 0x80)) return false;  // DS3231 oscillator-stop flag
  int sec = bcd2b(r[0] & 0x7F), min = bcd2b(r[1] & 0x7F), hour;
  if (r[2] & 0x40) hour = bcd2b(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0);
  else hour = bcd2b(r[2] & 0x3F);
  int day = bcd2b(r[4] & 0x3F), mon = bcd2b(r[5] & 0x1F), year = 2000 + bcd2b(r[6]);
  if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
  out = (time_t)(daysFromCivil(year, mon, day) * 86400LL + hour * 3600 + min * 60 + sec);
  return out > 1700000000;
}

bool rtcWrite(time_t t) {
  struct tm u;
  gmtime_r(&t, &u);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0);
  Wire.write(b2bcd(u.tm_sec));            // also clears DS1307 CH bit
  Wire.write(b2bcd(u.tm_min));
  Wire.write(b2bcd(u.tm_hour));           // 24-hour mode
  Wire.write(u.tm_wday + 1);
  Wire.write(b2bcd(u.tm_mday));
  Wire.write(b2bcd(u.tm_mon + 1));
  Wire.write(b2bcd(u.tm_year % 100));
  if (Wire.endTransmission() != 0) return false;
  if (cfg.rtcType == 0) {                 // clear OSF
    uint8_t st = rtcReadReg(0x0F);
    Wire.beginTransmission(RTC_ADDR); Wire.write(0x0F); Wire.write(st & ~0x80); Wire.endTransmission();
  }
  return true;
}

void setSystemTime(time_t t) {
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
}

void rtcLoadSystemTime() {
  time_t t;
  if (rtcFound && rtcRead(t)) {
    setSystemTime(t);
    if (timeSrc != "NTP") timeSrc = "RTC";
  }
}

// ------------------------------------------------------------------ time
void onSntpSync(struct timeval *) { ntpEvent = true; }

void applyTime() {
  if (cfg.ntpOn && cfg.ntp1.length()) {
    strlcpy(ntpBuf1, cfg.ntp1.c_str(), sizeof(ntpBuf1));
    strlcpy(ntpBuf2, cfg.ntp2.c_str(), sizeof(ntpBuf2));
    sntp_set_time_sync_notification_cb(onSntpSync);
    configTzTime(cfg.tz.c_str(), ntpBuf1, ntpBuf2[0] ? ntpBuf2 : nullptr);
  } else {
    if (SNTP_ENABLED()) SNTP_STOP();
    setenv("TZ", cfg.tz.c_str(), 1);
    tzset();
  }
}

// ------------------------------------------------------------------ Wi-Fi
const char *wifiReasonText(uint8_t r) {
  switch (r) {
    case 2:   return "auth expired (wrong password?)";
    case 15:  return "4-way handshake timeout (wrong password)";
    case 23:  return "802.1X auth failed (check user/pass/CA)";
    case 201: return "SSID not found (5 GHz only? hidden? typo?)";
    case 202: return "auth failed (wrong password / WPA3-only?)";
    case 203: return "association failed (AP refused / MAC filter?)";
    case 204: return "handshake timeout (wrong password?)";
    case 205: return "connection failed";
    case 200: return "beacon timeout (weak signal)";
    default: {
      static char b[24];
      snprintf(b, sizeof(b), "disconnected, reason %u", r);
      return b;
    }
  }
}

static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "other";
  }
}

// Scan once at boot so the web UI / serial log show whether the configured SSID is visible at all
void scanForConfiguredSsid() {
  int n = WiFi.scanNetworks(false, true);          // include hidden networks
  int best = -1;
  for (int i = 0; i < n; i++)
    if (WiFi.SSID(i) == cfg.ssid && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  if (best < 0) {
    wifiScanInfo = "\"" + cfg.ssid + "\" NOT seen; " + n + " networks visible:";
    for (int i = 0; i < n && i < 12; i++) {
      String id = WiFi.SSID(i);
      wifiScanInfo += String(i ? "," : "") + " \"" + (id.length() ? id : String("<hidden>")) + "\"(ch" + WiFi.channel(i) + ")";
      if (id.equalsIgnoreCase(cfg.ssid)) wifiScanInfo += "<-case differs";
    }
    Serial.printf("Configured SSID bytes (%u):", cfg.ssid.length());
    for (unsigned i = 0; i < cfg.ssid.length(); i++) Serial.printf(" %02X", (uint8_t)cfg.ssid[i]);
    Serial.println();
  } else {
    wifiScanInfo = "\"" + cfg.ssid + "\" seen: ch " + WiFi.channel(best) + ", " + WiFi.RSSI(best) +
                   " dBm, " + authName(WiFi.encryptionType(best));
    if (cfg.ent != (WiFi.encryptionType(best) == WIFI_AUTH_WPA2_ENTERPRISE))
      wifiScanInfo += cfg.ent ? "  <- configured as Enterprise but AP is not" : "  <- AP is Enterprise, select WPA2-Enterprise";
  }
  WiFi.scanDelete();
  Serial.println("Scan: " + wifiScanInfo);
}

void startAP() {
  if (apActive) return;
  WiFi.mode(cfg.ssid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(apSsid.c_str(), AP_PASS);
  apActive = true;
  Serial.printf("AP %s started\n", apSsid.c_str());
}

void startWiFi() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char b[24];
  snprintf(b, sizeof(b), "VGA-Clock-%02X%02X", mac[4], mac[5]);
  apSsid = b;

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);           // retries handled in wifiTick() so the setup AP stays usable
  WiFi.setHostname(cfg.host.c_str());
  WiFi.mode(WIFI_STA);
  // Default is country "CN" with AUTO policy: the ESP32 adopts the country from nearby beacons, and a
  // neighbour advertising "US" limits it to channels 1-11. Force the EU plan (1-13) instead.
  wifi_country_t country = {};
  strcpy(country.cc, "EU");
  country.schan  = 1;
  country.nchan  = 13;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);
  lastStaUp = lastRetry = millis();

  if (!cfg.ssid.length()) { startAP(); return; }

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    wifiReason = info.wifi_sta_disconnected.reason ? info.wifi_sta_disconnected.reason : 1;
    Serial.printf("Wi-Fi disconnected, reason %u: %s\n", wifiReason, wifiReasonText(wifiReason));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { wifiReason = 0; }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

  scanForConfiguredSsid();
  Serial.printf("Connecting to \"%s\" (%s, password length %u)\n", cfg.ssid.c_str(),
                cfg.ent ? "WPA2-Enterprise" : "WPA/WPA2-Personal", cfg.ent ? cfg.eapPass.length() : cfg.psk.length());

  if (cfg.ent) {
    const String &id = cfg.eapId.length() ? cfg.eapId : cfg.eapUser;
    EAP_SET_IDENTITY((const unsigned char *)id.c_str(), id.length());
    EAP_SET_USERNAME((const unsigned char *)cfg.eapUser.c_str(), cfg.eapUser.length());
    EAP_SET_PASSWORD((const unsigned char *)cfg.eapPass.c_str(), cfg.eapPass.length());
    if (cfg.eapCA.length()) {
      // buffer must stay valid: cfg.eapCA is only replaced together with a reboot
      EAP_SET_CA_CERT((const unsigned char *)cfg.eapCA.c_str(), cfg.eapCA.length() + 1);
      EAP_DISABLE_TIME_CHECK(true);   // no valid clock yet at boot
    }
    EAP_ENABLE();
    WiFi.begin(cfg.ssid.c_str());
  } else {
    EAP_DISABLE();
    WiFi.begin(cfg.ssid.c_str(), cfg.psk.length() ? cfg.psk.c_str() : nullptr);
  }
}

void wifiTick() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up) {
    lastStaUp = now;
    if (!staUp) { staUp = true; connectedAt = now; Serial.println("Wi-Fi up: " + WiFi.localIP().toString()); }
  } else if (staUp) staUp = false;

  if (!cfg.ssid.length()) return;
  if (!up && now - lastRetry > (apActive ? 60000UL : 30000UL)) { lastRetry = now; WiFi.reconnect(); }
  if (!up && !apActive && now - lastStaUp > 45000UL) startAP();
  if (up && apActive && now - connectedAt > 60000UL && WiFi.softAPgetStationNum() == 0) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive = false;
  }
}

// ------------------------------------------------------------------ web UI
#include "index_html.h"

static String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}
static void jS(String &j, const char *k, const String &v) { j += '"'; j += k; j += "\":\""; j += jsonEsc(v); j += "\","; }
static void jN(String &j, const char *k, long v)          { j += '"'; j += k; j += "\":"; j += v; j += ','; }
static void jB(String &j, const char *k, bool v)          { j += '"'; j += k; j += "\":"; j += v ? "true" : "false"; j += ','; }
static String hexColor(uint32_t c) { char b[8]; snprintf(b, sizeof(b), "#%06x", (unsigned)(c & 0xFFFFFF)); return b; }
static void jEnd(String &j) { if (j.endsWith(",")) j.remove(j.length() - 1); j += '}'; }

bool auth() {
  if (server.authenticate(ADMIN_USR, cfg.admin.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "VGA Clock");
  return false;
}

void handleConfig() {
  if (!auth()) return;
  String j = "{\"resList\":[";
  for (int i = 0; i < NUM_RES; i++) { if (i) j += ','; j += '"'; j += RES[i].name; j += '"'; }
  j += "],";
  jN(j, "mode", cfg.mode); jN(j, "res", cfg.res); jN(j, "aspect", cfg.aspect);
  j += "\"aspList\":[";
  for (int i = 0; i < NUM_ASPECT; i++) { if (i) j += ','; j += '"'; j += ASPECT_NAMES[i]; j += '"'; }
  j += "],";
  jS(j, "color", hexColor(cfg.color)); jS(j, "face", hexColor(cfg.face));
  jS(j, "hand", hexColor(cfg.hand));   jS(j, "sec", hexColor(cfg.sec));
  jB(j, "ghost", cfg.ghost); jB(j, "h24", cfg.h24); jB(j, "secs", cfg.seconds);
  jB(j, "blink", cfg.blink); jB(j, "jump", cfg.jump);
  jS(j, "tz", cfg.tz); jB(j, "ntp", cfg.ntpOn); jS(j, "ntp1", cfg.ntp1); jS(j, "ntp2", cfg.ntp2);
  jB(j, "rtc", cfg.rtcOn); jN(j, "rtct", cfg.rtcType); jN(j, "sda", cfg.sda); jN(j, "scl", cfg.scl);
  jB(j, "ent", cfg.ent); jS(j, "ssid", cfg.ssid);
  jS(j, "eapid", cfg.eapId); jS(j, "eapuser", cfg.eapUser); jS(j, "eapca", cfg.eapCA);
  jS(j, "host", cfg.host);
  jEnd(j);
  server.send(200, "application/json", j);
}

void handleStatus() {
  if (!auth()) return;
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char tb[40];
  strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S %Z", &lt);
  bool up = WiFi.status() == WL_CONNECTED;
  String j = "{";
  jS(j, "time", timeValid() ? String(tb) : String("not set"));
  jS(j, "src", timeSrc);
  jS(j, "wifi", up ? "connected to " + WiFi.SSID()
                   : (String(apActive ? "setup AP active, " : "") + "not connected" +
                      (wifiReason ? String(": ") + wifiReasonText(wifiReason) : String(""))));
  jS(j, "scan", wifiScanInfo);
  jS(j, "ip", up ? WiFi.localIP().toString() : (apActive ? WiFi.softAPIP().toString() : String("-")));
  jN(j, "rssi", up ? WiFi.RSSI() : 0);
  jS(j, "rtc", !cfg.rtcOn ? String("disabled") : (rtcFound ? String("found") : String("not found")));
  jS(j, "video", String(RES[cfg.res].name) + ", " + bpp + " bpp");
  jN(j, "heap", ESP.getFreeHeap());
  jEnd(j);
  server.send(200, "application/json", j);
}

static bool rtcPinOK(int p) {
  static const int ok[] = { 2, 13, 14, 16, 17, 25, 26, 27, 32, 33 };
  for (int v : ok) if (v == p) return true;
  return false;
}

static uint32_t parseColor(const String &s, uint32_t def) {
  return (s.length() == 7 && s[0] == '#') ? strtoul(s.c_str() + 1, nullptr, 16) : def;
}

void sendErr(const char *e) { server.send(200, "application/json", String("{\"ok\":false,\"err\":\"") + e + "\"}"); }

void handleSave() {
  if (!auth()) return;
  Config n = cfg;
  n.mode    = server.arg("mode").toInt() == 1 ? MODE_ANALOG : MODE_DIGITAL;
  n.res     = constrain(server.arg("res").toInt(), 0, NUM_RES - 1);
  n.aspect  = constrain(server.arg("aspect").toInt(), 0, NUM_ASPECT - 1);
  n.color   = parseColor(server.arg("color"), n.color);
  n.face    = parseColor(server.arg("face"), n.face);
  n.hand    = parseColor(server.arg("hand"), n.hand);
  n.sec     = parseColor(server.arg("sec"), n.sec);
  n.ghost   = server.hasArg("ghost");
  n.h24     = server.hasArg("h24");
  n.seconds = server.hasArg("secs");
  n.blink   = server.hasArg("blink");
  n.jump    = server.hasArg("jump");
  n.tz      = server.arg("tz"); n.tz.trim();
  if (!n.tz.length()) n.tz = "UTC0";
  n.ntpOn   = server.hasArg("ntp");
  n.ntp1    = server.arg("ntp1"); n.ntp1.trim();
  n.ntp2    = server.arg("ntp2"); n.ntp2.trim();
  if (n.ntp1.length() >= sizeof(ntpBuf1) || n.ntp2.length() >= sizeof(ntpBuf2)) return sendErr("NTP server name too long");
  n.rtcOn   = server.hasArg("rtc");
  n.rtcType = server.arg("rtct").toInt() == 1 ? 1 : 0;
  n.sda     = server.arg("sda").toInt();
  n.scl     = server.arg("scl").toInt();
  if (n.rtcOn && (!rtcPinOK(n.sda) || !rtcPinOK(n.scl) || n.sda == n.scl)) return sendErr("Invalid RTC GPIO pins");
  n.ent     = server.arg("ent") == "1";
  n.ssid    = server.arg("ssid"); n.ssid.trim();
  if (server.arg("psk").length()) n.psk = server.arg("psk");
  n.eapId   = server.arg("eapid");
  n.eapUser = server.arg("eapuser");
  if (server.arg("eappass").length()) n.eapPass = server.arg("eappass");
  n.eapCA   = server.arg("eapca"); n.eapCA.replace("\r", ""); n.eapCA.trim();
  if (n.eapCA.length()) n.eapCA += "\n";
  String h  = server.arg("host"); h.toLowerCase();
  n.host = "";
  for (unsigned i = 0; i < h.length() && n.host.length() < 31; i++)
    if (isalnum((unsigned char)h[i]) || h[i] == '-') n.host += h[i];
  if (!n.host.length()) n.host = "vgaclock";
  if (server.arg("admin").length()) n.admin = server.arg("admin");

  bool reboot = n.res != cfg.res || neededBpp(n) != bpp
             || n.ent != cfg.ent || n.ssid != cfg.ssid || n.psk != cfg.psk
             || n.eapId != cfg.eapId || n.eapUser != cfg.eapUser || n.eapPass != cfg.eapPass || n.eapCA != cfg.eapCA
             || n.host != cfg.host || n.admin != cfg.admin
             || n.rtcOn != cfg.rtcOn || n.rtcType != cfg.rtcType || n.sda != cfg.sda || n.scl != cfg.scl;

  cfg = n;
  saveConfig();
  if (reboot) restartAt = millis() + 1500;
  else { applyTime(); relayout(); renderClock(); }
  server.send(200, "application/json", String("{\"ok\":true,\"reboot\":") + (reboot ? "true}" : "false}"));
}

void handleScan() {
  if (!auth()) return;
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"rssi\":" + WiFi.RSSI(i) +
         ",\"ent\":" + (WiFi.encryptionType(i) == WIFI_AUTH_WPA2_ENTERPRISE ? "true" : "false") + "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleSetTime() {
  if (!auth()) return;
  time_t t = (time_t)strtoll(server.arg("epoch").c_str(), nullptr, 10);
  if (t < 1700000000) return sendErr("bad epoch");
  setSystemTime(t);
  timeSrc = "browser";
  if (cfg.rtcOn && rtcFound) rtcWrite(t);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadAuth = server.authenticate(ADMIN_USR, cfg.admin.c_str());
    uploadOK = uploadAuth;
    if (!uploadAuth) return;
    Serial.printf("Firmware upload: %s\n", up.filename.c_str());
    stopVideo();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) uploadOK = false;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOK && Update.write(up.buf, up.currentSize) != up.currentSize) { uploadOK = false; Update.abort(); }
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadOK && !Update.end(true)) uploadOK = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    uploadOK = false;
  }
}

void handleUploadDone() {
  if (!uploadAuth) { server.requestAuthentication(BASIC_AUTH, "VGA Clock"); return; }
  server.send(200, "text/plain", uploadOK ? "Update OK, rebooting…" : "Update FAILED, rebooting…");
  restartAt = millis() + 1000;     // also after failure: video was stopped
}

void startServer() {
  server.on("/", HTTP_GET, []() { if (auth()) server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/config.json", HTTP_GET, handleConfig);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/settime", HTTP_POST, handleSetTime);
  server.on("/update", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/reboot", HTTP_POST, []() { if (!auth()) return; server.send(200, "text/plain", "ok"); restartAt = millis() + 500; });
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();
}

void startOTA() {
  ArduinoOTA.setHostname(cfg.host.c_str());
  ArduinoOTA.setPassword(cfg.admin.c_str());
  ArduinoOTA.onStart([]() { stopVideo(); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); restartAt = millis() + 500; });
  ArduinoOTA.begin();                         // also starts mDNS with the hostname
  MDNS.addService("http", "tcp", 80);
}

// ------------------------------------------------------------------ main
void setup() {
  Serial.begin(115200);
  delay(100);
  loadConfig();

  startVideo();                 // allocate the frame buffer first, before Wi-Fi fragments the heap
  relayout();
  renderClock();

  if (cfg.rtcOn) {
    Wire.begin(cfg.sda, cfg.scl, 100000);
    rtcFound = rtcProbe();
    Serial.printf("RTC %s\n", rtcFound ? "found" : "NOT found");
    rtcLoadSystemTime();
  }
  applyTime();
  startWiFi();
  startServer();
  startOTA();
  renderClock();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  static time_t lastSec = 0;
  static uint32_t lastTick = 0;
  time_t now = time(nullptr);
  if (now != lastSec) {
    lastSec = now;
    renderClock();
  }

  if (millis() - lastTick > 500) {
    lastTick = millis();
    wifiTick();
    drawStatus(computeStatus());
  }

  if (ntpEvent) {                          // flag set from the SNTP task; do the I2C work here
    ntpEvent = false;
    lastNtp = time(nullptr);
    timeSrc = "NTP";
    if (cfg.rtcOn && rtcFound) rtcWrite(lastNtp);
  }

  // without recent NTP, re-discipline the system clock from the RTC every hour
  if (cfg.rtcOn && rtcFound && millis() - lastRtcResync > 3600000UL) {
    lastRtcResync = millis();
    if (!cfg.ntpOn || time(nullptr) - lastNtp > 7200) rtcLoadSystemTime();
  }

  if (restartAt && (int32_t)(millis() - restartAt) > 0) ESP.restart();
  delay(1);
}
