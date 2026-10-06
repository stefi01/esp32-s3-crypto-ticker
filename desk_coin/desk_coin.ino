/*
  Desk Coin — 4.0 in Hosyond / LCD wiki E32R40T.
  ESP32-32E, ST7796S, XPT2046. No extra RAM.
  Arduino: ESP32 Dev Module, flash 4MB, partition Huge APP (3MB No OTA).
  Drawn in an 800x480 layout and scaled onto 480x320. SD slot on top.
  Live prices and candles from Kraken's public feed. No account.
  This file is only for this board. Do not flash it on the 7 in.
*/

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include "driver/gpio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GFX_BL 27
#define PW 800
#define PH 480
#define PANEL_W 480
#define PANEL_H 320
#define LCD_DC 2
#define LCD_CS 15
#define LCD_SCK 14
#define LCD_MOSI 13
#define LCD_MISO 12
#define TP_CS 33
#define TP_IRQ 36
#define SD_CS 5

#define MAX_C 60
#define PRICE_MS 20000UL
#define CANDLE_MS 60000UL
#define FAIL_MS 60000UL
#define COIN_MS 20000UL

enum { PG_BOARD = 0, PG_COINS, PG_WIFI };

struct CoinDef {
  const char *name;
  const char *pair;
};

static const CoinDef COINS[] = {
    {"BTC", "XBTUSD"}, {"ETH", "ETHUSD"}, {"SOL", "SOLUSD"}, {"XRP", "XRPUSD"},
    {"DOGE", "XDGUSD"}, {"ADA", "ADAUSD"}, {"AVAX", "AVAXUSD"}, {"LINK", "LINKUSD"},
    {"LTC", "LTCUSD"}, {"DOT", "DOTUSD"}, {"TRX", "TRXUSD"}, {"TON", "TONUSD"},
    {"UNI", "UNIUSD"}, {"NEAR", "NEARUSD"}, {"ATOM", "ATOMUSD"}, {"PEPE", "PEPEUSD"},
};
static const int NCOIN = (int)(sizeof(COINS) / sizeof(COINS[0]));

struct RngDef {
  const char *chip;
  const char *title;
  int interval;
  int span;
};

static const RngDef RANGES[3] = {
    {"1H", "1 HOUR", 1, 3600},
    {"4H", "4 HOUR", 5, 4 * 3600},
    {"1D", "1 DAY", 30, 86400},
};

struct Candle {
  float o, h, l, c;
};

struct TouchCal {
  uint16_t x0, x1, y0, y1, flags;
  bool valid;
} touchCal = {1, 1, 1, 1, 0, false};

static const char *TZ_NAME[] = {"PT", "MT", "CT", "ET"};
static const char *TZ_POSIX[] = {
    "PST8PDT,M3.2.0,M11.1.0",
    "MST7MDT,M3.2.0,M11.1.0",
    "CST6CDT,M3.2.0,M11.1.0",
    "EST5EDT,M3.2.0,M11.1.0",
};
static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

const int CX = 400;
const int KW = 80;
const int CHART_X = 12, CHART_Y = 188, CHART_W = 776, CHART_H = 216;
const int CHIP_Y = 414, CHIP_H = 52;

uint16_t COL_BG, COL_DIM, COL_MID, COL_HOT, COL_TXT, COL_WHT, COL_UP, COL_DN, COL_BLK;

SPIClass lcdSpi(HSPI);
Arduino_DataBus *lcdBus = nullptr;
Arduino_ST7796 *panel = nullptr;
Arduino_GFX *gfx = nullptr;
Preferences prefs;

int page = PG_BOARD;
int coinIx = 0;
int rangeIx = 0;
int tzIx = 0;
int blLevel = 2;
bool autoOn = true;
bool autoArm = false;
bool wifiOk = false;
bool quoteOk = false;
bool forceCandles = true;
char quoteErr[24] = "waiting";
float lastPx = 0, openPx = 0, hiPx = 0, loPx = 0, vol24 = 0;
uint32_t quoteAt = 0;
uint32_t nextPxAt = 0;
uint32_t nextCandleAt = 0;
uint32_t lastWifiTry = 0;
uint32_t lastTap = 0;
Candle candles[MAX_C];
int nCandles = 0;

volatile int netPend = 0;
volatile bool netBusy = false;
volatile bool boardDirty = false;

char wifiSsid[64] = "";
char wifiPass[64] = "";
char wifiScan[16][33];
char wifiMsg[28] = "";
int nWifiScan = 0, wifiScanTop = 0, wifiFocus = 0;
bool wifiShift = false, wifiSym = false, wifiScanBusy = false, wifiScanWait = false;

void cols() {
  COL_BG = RGB565(18, 22, 28);
  COL_DIM = RGB565(32, 40, 52);
  COL_MID = RGB565(200, 210, 220);
  COL_HOT = RGB565(255, 210, 40);
  COL_TXT = RGB565(250, 250, 250);
  COL_WHT = RGB565(255, 255, 255);
  COL_UP = RGB565(40, 200, 90);
  COL_DN = RGB565(230, 55, 55);
  COL_BLK = RGB565(0, 0, 0);
}

void centre(const char *s, int cx, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(cx - (int)w / 2, y);
  gfx->print(s);
}

void rightText(const char *s, int right, int y) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(right - (int)w, y);
  gfx->print(s);
}

bool hit(int x, int y, int x0, int y0, int x1, int y1) {
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

bool hitBox(int x, int y, int bx, int by, int bw, int bh) {
  return hit(x, y, bx, by, bx + bw, by + bh);
}

void chip(int x, int y, int w, int h, const char *t, bool on) {
  gfx->fillRoundRect(x, y, w, h, 6, on ? COL_HOT : COL_DIM);
  gfx->setTextColor(on ? COL_BG : COL_TXT, on ? COL_HOT : COL_DIM);
  gfx->setTextSize(2);
  centre(t, x + w / 2, y + (h / 2) - 8);
}

void applyTz() {
  if (tzIx < 0 || tzIx > 3) tzIx = 0;
  setenv("TZ", TZ_POSIX[tzIx], 1);
  tzset();
}

void applyBl() {
  pinMode(GFX_BL, OUTPUT);
  analogWrite(GFX_BL, blLevel == 0 ? 70 : (blLevel == 1 ? 150 : 255));
}

void savePrefs() {
  prefs.begin("deskcoin", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putInt("coin", coinIx);
  prefs.putInt("rng", rangeIx);
  prefs.putInt("tz", tzIx);
  prefs.putInt("bl", blLevel);
  prefs.putBool("auto", autoOn);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("deskcoin", true);
  strlcpy(wifiSsid, prefs.getString("ssid", "").c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, prefs.getString("pass", "").c_str(), sizeof(wifiPass));
  coinIx = prefs.getInt("coin", 0);
  rangeIx = prefs.getInt("rng", 0);
  tzIx = prefs.getInt("tz", 0);
  blLevel = prefs.getInt("bl", 2);
  autoOn = prefs.getBool("auto", true);
  prefs.end();
  if (coinIx < 0 || coinIx >= NCOIN) coinIx = 0;
  if (rangeIx < 0 || rangeIx > 2) rangeIx = 0;
  if (tzIx < 0 || tzIx > 3) tzIx = 0;
  if (blLevel < 0 || blLevel > 2) blLevel = 2;
  applyTz();
  cols();
}

bool clockReady() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

void money(char *out, int n, float v) {
  if (!(v > 0.f)) {
    strlcpy(out, "--", n);
    return;
  }
  if (v >= 100.f) {
    long d = (long)(v + 0.5f);
    char raw[16], num[20];
    snprintf(raw, sizeof(raw), "%ld", d);
    int len = (int)strlen(raw);
    int commas = (len - 1) / 3;
    int pos = 0;
    for (int i = 0; i < len && pos < (int)sizeof(num) - 1; i++) {
      num[pos++] = raw[i];
      int left = len - i - 1;
      if (left > 0 && left % 3 == 0 && pos < (int)sizeof(num) - 1) num[pos++] = ',';
    }
    num[pos] = 0;
    snprintf(out, n, "$%s", num);
  } else if (v >= 1.f) {
    long cents = (long)(v * 100.f + 0.5f);
    snprintf(out, n, "$%ld.%02ld", cents / 100, cents % 100);
  } else if (v >= 0.001f) {
    long u = (long)(v * 10000.f + 0.5f);
    snprintf(out, n, "$0.%04ld", u);
  } else {
    long u = (long)(v * 100000000.f + 0.5f);
    snprintf(out, n, "$0.%08ld", u);
  }
}

void volWords(char *out, int n, float v) {
  const char *name = COINS[coinIx].name;
  if (v >= 1000000000.f) {
    int whole = (int)(v / 1000000000.f);
    int frac = (int)((v / 1000000000.f - whole) * 10.f + 0.5f);
    snprintf(out, n, "%d.%dB %s traded", whole, frac, name);
  } else if (v >= 1000000.f) {
    int whole = (int)(v / 1000000.f);
    int frac = (int)((v / 1000000.f - whole) * 10.f + 0.5f);
    snprintf(out, n, "%d.%dM %s traded", whole, frac, name);
  } else if (v >= 100.f) {
    char num[20];
    money(num, sizeof(num), v);
    if (num[0] == '$') snprintf(out, n, "%s %s traded", num + 1, name);
    else snprintf(out, n, "%s %s traded", num, name);
  } else {
    snprintf(out, n, "%d %s traded", (int)(v + 0.5f), name);
  }
}

void drawClock() {
  gfx->fillRect(500, 4, 292, 74, COL_BG);
  gfx->setTextColor(COL_TXT, COL_BG);
  if (!clockReady()) {
    gfx->setTextSize(2);
    rightText("internet clock", 788, 28);
    return;
  }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int h = t.tm_hour % 12;
  if (h == 0) h = 12;
  char line[24];
  snprintf(line, sizeof(line), "%d:%02d %s", h, t.tm_min, t.tm_hour >= 12 ? "PM" : "AM");
  gfx->setTextSize(3);
  rightText(line, 788, 8);
  snprintf(line, sizeof(line), "%s %s %d", WDAY[t.tm_wday], MON[t.tm_mon], t.tm_mday);
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setTextSize(2);
  rightText(line, 788, 46);
}

void drawStatus() {
  gfx->fillRect(8, 112, 784, 24, COL_BG);
  gfx->setTextSize(2);
  char left[48];
  if (quoteOk && vol24 > 0.f) volWords(left, sizeof(left), vol24);
  else strlcpy(left, quoteErr[0] ? quoteErr : "waiting", sizeof(left));
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setCursor(12, 114);
  gfx->print(left);

  char right[28] = "";
  if (!wifiOk) strlcpy(right, "no wifi", sizeof(right));
  else if (autoOn)
    snprintf(right, sizeof(right), "next %s", COINS[(coinIx + 1) % NCOIN].name);
  else if (quoteOk) {
    int age = (int)((millis() - quoteAt) / 1000UL);
    snprintf(right, sizeof(right), "live %ds", age);
  }
  if (right[0]) {
    gfx->setTextColor(autoOn ? COL_HOT : COL_MID, COL_BG);
    rightText(right, 788, 114);
  }
}

void drawDayBar() {
  gfx->fillRect(8, 140, 784, 44, COL_BG);
  if (!quoteOk || !(hiPx > loPx)) return;
  float t = (lastPx - loPx) / (hiPx - loPx);
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  int x0 = 16, y0 = 142, w = 768, h = 16;
  gfx->fillRoundRect(x0, y0, w, h, 6, COL_DIM);
  int fill = (int)(t * (float)w);
  if (fill < 8) fill = 8;
  uint16_t col = lastPx >= openPx ? COL_UP : COL_DN;
  gfx->fillRoundRect(x0, y0, fill, h, 6, col);
  char lo[20], hi[20];
  money(lo, sizeof(lo), loPx);
  money(hi, sizeof(hi), hiPx);
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setTextSize(2);
  gfx->setCursor(16, 162);
  gfx->print(lo);
  rightText(hi, 784, 162);
}

static int candleY(float p, float hi, float lo, int plotY, int plotH) {
  float span = hi - lo;
  if (!(span > 0.f)) span = 1.f;
  float u = (hi - p) / span;
  int y = plotY + (int)(u * (float)plotH);
  if (y < plotY) y = plotY;
  if (y > plotY + plotH) y = plotY + plotH;
  return y;
}

void drawCandles() {
  gfx->fillRoundRect(CHART_X, CHART_Y, CHART_W, CHART_H, 8, COL_DIM);
  gfx->setTextSize(2);
  gfx->setTextColor(COL_MID, COL_DIM);
  gfx->setCursor(CHART_X + 12, CHART_Y + 8);
  gfx->print(RANGES[rangeIx].title);
  if (nCandles < 2) {
    gfx->setTextColor(COL_TXT, COL_DIM);
    centre(quoteOk ? "waiting for candles" : (quoteErr[0] ? quoteErr : "waiting"), CX, CHART_Y + CHART_H / 2 - 8);
    return;
  }
  float lo = candles[0].l, hi = candles[0].h;
  for (int i = 1; i < nCandles; i++) {
    if (candles[i].l < lo) lo = candles[i].l;
    if (candles[i].h > hi) hi = candles[i].h;
  }
  if (!(hi > lo)) hi = lo + 1.f;
  float pad = (hi - lo) * 0.08f;
  lo -= pad;
  hi += pad;
  int plotX = CHART_X + 8;
  int plotW = CHART_W - 16;
  int plotY = CHART_Y + 28;
  int plotH = CHART_H - 40;
  for (int g = 1; g <= 3; g++) {
    int gy = plotY + (plotH * g) / 4;
    gfx->drawFastHLine(plotX, gy, plotW, COL_BG);
  }
  float slot = (float)plotW / (float)nCandles;
  for (int i = 0; i < nCandles; i++) {
    float px = candles[i].o, ph = candles[i].h, pl = candles[i].l, pc = candles[i].c;
    bool up = pc >= px;
    uint16_t col = up ? COL_UP : COL_DN;
    int cx = plotX + (int)((i + 0.5f) * slot);
    int yH = candleY(ph, hi, lo, plotY, plotH);
    int yL = candleY(pl, hi, lo, plotY, plotH);
    int yO = candleY(px, hi, lo, plotY, plotH);
    int yC = candleY(pc, hi, lo, plotY, plotH);
    gfx->drawLine(cx, yH, cx, yL, col);
    int top = yO < yC ? yO : yC;
    int bh = yO > yC ? yO - yC : yC - yO;
    if (bh < 2) bh = 2;
    int bw = (int)(slot * 0.62f);
    if (bw < 4) bw = 4;
    gfx->fillRect(cx - bw / 2, top, bw, bh, col);
  }
}

void drawChips() {
  const int xs[7] = {8, 92, 176, 268, 386, 524, 632};
  const int ws[7] = {76, 76, 76, 110, 130, 100, 160};
  const char *labs[7] = {RANGES[0].chip, RANGES[1].chip, RANGES[2].chip, "AUTO", "COINS", TZ_NAME[tzIx], "WIFI"};
  bool on[7] = {rangeIx == 0, rangeIx == 1, rangeIx == 2, autoOn, false, false, false};
  for (int i = 0; i < 7; i++) chip(xs[i], CHIP_Y, ws[i], CHIP_H, labs[i], on[i]);
}

void drawBoard() {
  page = PG_BOARD;
  gfx->fillScreen(COL_BG);
  gfx->setTextSize(3);
  gfx->setTextColor(COL_HOT, COL_BG);
  gfx->setCursor(12, 8);
  gfx->print(COINS[coinIx].name);

  char px[24];
  money(px, sizeof(px), quoteOk ? lastPx : 0);
  uint16_t ink = COL_TXT;
  if (quoteOk && openPx > 0.f) ink = lastPx >= openPx ? COL_UP : COL_DN;
  gfx->setTextColor(ink, COL_BG);
  gfx->setTextSize(4);
  gfx->setCursor(12, 40);
  gfx->print(px);

  gfx->setTextSize(3);
  if (quoteOk && openPx > 0.f) {
    float pct = (lastPx - openPx) / openPx * 100.f;
    int hundred = (int)(fabsf(pct) * 100.f + 0.5f);
    char words[32];
    if (hundred < 5) strlcpy(words, "flat today", sizeof(words));
    else
      snprintf(words, sizeof(words), "%s %d.%02d%% today", pct >= 0.f ? "up" : "down", hundred / 100, hundred % 100);
    gfx->setTextColor(pct >= 0.f ? COL_UP : COL_DN, COL_BG);
    if (hundred < 5) gfx->setTextColor(COL_MID, COL_BG);
    gfx->setCursor(12, 80);
    gfx->print(words);
  } else if (!wifiOk) {
    gfx->setTextColor(COL_DN, COL_BG);
    gfx->setCursor(12, 80);
    gfx->print("join wifi");
  }

  drawStatus();
  drawDayBar();
  drawCandles();
  drawClock();
  drawChips();
}

void drawCoins() {
  page = PG_COINS;
  gfx->fillScreen(COL_BG);
  chip(8, 8, 140, 36, "< BACK", false);
  gfx->setTextColor(COL_HOT, COL_BG);
  gfx->setTextSize(2);
  centre("PICK A COIN", CX, 16);
  for (int i = 0; i < NCOIN; i++) {
    int col = i % 4;
    int row = i / 4;
    chip(16 + col * 196, 64 + row * 72, 184, 60, COINS[i].name, i == coinIx);
  }
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setTextSize(2);
  gfx->setCursor(16, 366);
  gfx->print("TIME ZONE");
  for (int i = 0; i < 4; i++) chip(200 + i * 100, 354, 92, 44, TZ_NAME[i], i == tzIx);
  const char *light = blLevel == 0 ? "DIM" : (blLevel == 1 ? "MID" : "BRIGHT");
  chip(616, 354, 168, 44, light, false);
}

void drawWifiField(int focus, const char *raw, const char *empty, int y) {
  bool on = wifiFocus == focus;
  gfx->fillRoundRect(120, y, 664, 40, 6, on ? COL_HOT : COL_DIM);
  gfx->setTextColor(on ? COL_BG : COL_TXT, on ? COL_HOT : COL_DIM);
  gfx->setTextSize(2);
  gfx->setCursor(132, y + 10);
  gfx->print(raw[0] ? raw : empty);
}

void drawKey(int x, int y, int w, int h, const char *t) {
  gfx->fillRoundRect(x, y, w, h, 4, COL_DIM);
  gfx->setTextColor(COL_TXT, COL_DIM);
  gfx->setTextSize(2);
  centre(t, x + w / 2, y + h / 2 - 8);
}

void drawRow(const char *s, int y) {
  int n = (int)strlen(s);
  for (int i = 0; i < n; i++) {
    char t[2] = {s[i], 0};
    drawKey(i * KW + 2, y, KW - 6, 40, t);
  }
}

void startWifiScan() {
  nWifiScan = 0;
  wifiScanTop = 0;
  wifiScanBusy = true;
  wifiScanWait = false;
  strlcpy(wifiMsg, "scanning", sizeof(wifiMsg));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}

void drawWifi() {
  page = PG_WIFI;
  gfx->fillScreen(COL_BG);
  chip(8, 8, 140, 36, "< BACK", false);
  gfx->setTextColor(COL_HOT, COL_BG);
  gfx->setTextSize(2);
  centre("JOIN WIFI", CX, 16);
  chip(660, 8, 132, 36, "SCAN", wifiScanBusy);
  chip(8, 52, 56, 36, "<", false);
  chip(736, 52, 56, 36, ">", false);
  for (int i = 0; i < 2; i++) {
    int idx = wifiScanTop + i;
    if (idx >= nWifiScan) break;
    char shown[18];
    strlcpy(shown, wifiScan[idx], sizeof(shown));
    chip(72 + i * 328, 52, 320, 36, shown, !strcmp(wifiScan[idx], wifiSsid));
  }
  if (wifiMsg[0] && nWifiScan == 0) {
    gfx->setTextColor(COL_HOT, COL_BG);
    gfx->setTextSize(2);
    centre(wifiMsg, CX, 56);
  }
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setTextSize(2);
  gfx->setCursor(16, 110);
  gfx->print("SSID");
  drawWifiField(0, wifiSsid, "tap net or type", 100);
  gfx->setTextColor(COL_MID, COL_BG);
  gfx->setCursor(16, 158);
  gfx->print("PASS");
  drawWifiField(1, wifiPass, "type password", 148);
  const char *r0 = "1234567890";
  const char *r1 = wifiSym ? "!@#$%^&*()" : (wifiShift ? "QWERTYUIOP" : "qwertyuiop");
  const char *r2 = wifiSym ? "-_=+[]{};'" : (wifiShift ? "ASDFGHJKL" : "asdfghjkl");
  const char *r3 = wifiSym ? ",.<>/?\\|~`" : (wifiShift ? "ZXCVBNM.@-" : "zxcvbnm.@-");
  drawRow(r0, 200);
  drawRow(r1, 248);
  drawRow(r2, 296);
  drawRow(r3, 344);
  drawKey(722, 296, 70, 40, "DEL");
  chip(8, 392, 120, 36, wifiShift ? "ABC" : "abc", wifiShift);
  chip(140, 392, 400, 36, "SPACE", false);
  chip(552, 392, 120, 36, wifiSym ? "ABC" : "!#", wifiSym);
  chip(8, 436, 252, 36, "SAVE", true);
  chip(274, 436, 252, 36, "RESET WIFI", false);
  chip(540, 436, 252, 36, "FACTORY", false);
}

void openWifi() {
  wifiFocus = wifiSsid[0] ? 1 : 0;
  wifiMsg[0] = 0;
  if (!netBusy) startWifiScan();
  else wifiScanWait = true;
  drawWifi();
}

void wifiAppend(char c) {
  char *d = wifiFocus ? wifiPass : wifiSsid;
  int n = (int)strlen(d);
  if (n < 62) {
    d[n] = c;
    d[n + 1] = 0;
  }
}

void wifiDel() {
  char *d = wifiFocus ? wifiPass : wifiSsid;
  int n = (int)strlen(d);
  if (n) d[n - 1] = 0;
}

bool hitRow(int x, int y, const char *s, int y0) {
  if (y < y0 || y >= y0 + 44) return false;
  int i = x / KW;
  int n = (int)strlen(s);
  if (i < 0 || i >= n) return false;
  wifiAppend(s[i]);
  if (wifiShift && s[i] >= 'A' && s[i] <= 'Z') wifiShift = false;
  return true;
}

void pollWifiScan() {
  if (wifiScanWait && !netBusy && page == PG_WIFI) startWifiScan();
  if (!wifiScanBusy) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  wifiScanBusy = false;
  if (n < 0) n = 0;
  nWifiScan = 0;
  for (int i = 0; i < n && nWifiScan < 16; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    strlcpy(wifiScan[nWifiScan], s.c_str(), 33);
    nWifiScan++;
  }
  WiFi.scanDelete();
  wifiScanTop = 0;
  if (nWifiScan) wifiMsg[0] = 0;
  else strlcpy(wifiMsg, "none found  type name", sizeof(wifiMsg));
  if (page == PG_WIFI) drawWifi();
}

void applyWifiSave() {
  if (!wifiSsid[0]) {
    strlcpy(wifiMsg, "need wifi name", sizeof(wifiMsg));
    drawWifi();
    return;
  }
  savePrefs();
  gfx->fillScreen(COL_BG);
  gfx->setTextColor(COL_HOT, COL_BG);
  gfx->setTextSize(2);
  centre("saved", CX, 160);
  gfx->setTextColor(COL_TXT, COL_BG);
  centre(wifiSsid, CX, 200);
  centre("linking", CX, 240);
  delay(600);
  ESP.restart();
}

void clearWifi() {
  wifiSsid[0] = wifiPass[0] = 0;
  wifiOk = false;
  savePrefs();
}

void factoryReset() {
  prefs.begin("deskcoin", false);
  prefs.clear();
  prefs.end();
}

void armFetch(bool candlesToo) {
  if (candlesToo) forceCandles = true;
  autoArm = false;
  nextPxAt = millis();
  netPend = 1;
}

void pickCoin(int ix) {
  if (ix < 0 || ix >= NCOIN) return;
  coinIx = ix;
  autoOn = false;
  nCandles = 0;
  quoteOk = false;
  strlcpy(quoteErr, "waiting", sizeof(quoteErr));
  savePrefs();
  armFetch(true);
  drawBoard();
}

void pickRange(int ix) {
  if (ix < 0 || ix > 2 || ix == rangeIx) return;
  rangeIx = ix;
  nCandles = 0;
  savePrefs();
  armFetch(true);
  drawBoard();
}

void pickZone(int ix) {
  if (ix < 0 || ix > 3) return;
  tzIx = ix;
  applyTz();
  savePrefs();
}

void onTap(int x, int y) {
  if (page == PG_WIFI) {
    if (hit(x, y, 8, 8, 148, 44)) {
      drawBoard();
      return;
    }
    if (hit(x, y, 660, 8, 792, 44)) {
      if (!netBusy) startWifiScan();
      else wifiScanWait = true;
      drawWifi();
      return;
    }
    if (hit(x, y, 8, 52, 64, 88) && wifiScanTop > 0) {
      wifiScanTop -= 2;
      if (wifiScanTop < 0) wifiScanTop = 0;
      drawWifi();
      return;
    }
    if (hit(x, y, 736, 52, 792, 88) && wifiScanTop + 2 < nWifiScan) {
      wifiScanTop += 2;
      drawWifi();
      return;
    }
    for (int i = 0; i < 2; i++) {
      int idx = wifiScanTop + i;
      if (idx >= nWifiScan) break;
      if (hit(x, y, 72 + i * 328, 52, 392 + i * 328, 88)) {
        strlcpy(wifiSsid, wifiScan[idx], sizeof(wifiSsid));
        wifiFocus = 1;
        drawWifi();
        return;
      }
    }
    if (hit(x, y, 120, 100, 784, 140)) {
      wifiFocus = 0;
      drawWifi();
      return;
    }
    if (hit(x, y, 120, 148, 784, 188)) {
      wifiFocus = 1;
      drawWifi();
      return;
    }
    if (hit(x, y, 8, 436, 260, 472)) {
      applyWifiSave();
      return;
    }
    if (hit(x, y, 274, 436, 526, 472)) {
      clearWifi();
      strlcpy(wifiMsg, "wifi cleared", sizeof(wifiMsg));
      drawWifi();
      return;
    }
    if (hit(x, y, 540, 436, 792, 472)) {
      factoryReset();
      delay(200);
      ESP.restart();
      return;
    }
    if (hit(x, y, 8, 392, 128, 428)) {
      wifiShift = !wifiShift;
      wifiSym = false;
      drawWifi();
      return;
    }
    if (hit(x, y, 140, 392, 540, 428)) {
      wifiAppend(' ');
      drawWifi();
      return;
    }
    if (hit(x, y, 552, 392, 672, 428)) {
      wifiSym = !wifiSym;
      drawWifi();
      return;
    }
    if (hit(x, y, 722, 296, 792, 336)) {
      wifiDel();
      drawWifi();
      return;
    }
    const char *r0 = "1234567890";
    const char *r1 = wifiSym ? "!@#$%^&*()" : (wifiShift ? "QWERTYUIOP" : "qwertyuiop");
    const char *r2 = wifiSym ? "-_=+[]{};'" : (wifiShift ? "ASDFGHJKL" : "asdfghjkl");
    const char *r3 = wifiSym ? ",.<>/?\\|~`" : (wifiShift ? "ZXCVBNM.@-" : "zxcvbnm.@-");
    if (hitRow(x, y, r0, 200) || hitRow(x, y, r1, 248) || hitRow(x, y, r2, 296) || hitRow(x, y, r3, 344))
      drawWifi();
    return;
  }

  if (page == PG_COINS) {
    if (hit(x, y, 8, 8, 148, 44)) {
      drawBoard();
      return;
    }
    for (int i = 0; i < NCOIN; i++) {
      int col = i % 4;
      int row = i / 4;
      if (hitBox(x, y, 16 + col * 196, 64 + row * 72, 184, 60)) {
        pickCoin(i);
        return;
      }
    }
    for (int i = 0; i < 4; i++) {
      if (hitBox(x, y, 200 + i * 100, 354, 92, 44)) {
        pickZone(i);
        drawCoins();
        return;
      }
    }
    if (hitBox(x, y, 616, 354, 168, 44)) {
      blLevel = (blLevel + 1) % 3;
      applyBl();
      savePrefs();
      drawCoins();
    }
    return;
  }

  const int xs[7] = {8, 92, 176, 268, 386, 524, 632};
  const int ws[7] = {76, 76, 76, 110, 130, 100, 160};
  for (int i = 0; i < 3; i++) {
    if (hitBox(x, y, xs[i], CHIP_Y, ws[i], CHIP_H)) {
      pickRange(i);
      return;
    }
  }
  if (hitBox(x, y, xs[3], CHIP_Y, ws[3], CHIP_H)) {
    autoOn = !autoOn;
    autoArm = autoOn;
    if (autoOn) nextPxAt = millis() + COIN_MS;
    savePrefs();
    drawBoard();
    return;
  }
  if (hitBox(x, y, xs[4], CHIP_Y, ws[4], CHIP_H)) {
    drawCoins();
    return;
  }
  if (hitBox(x, y, xs[5], CHIP_Y, ws[5], CHIP_H)) {
    pickZone((tzIx + 1) % 4);
    drawBoard();
    return;
  }
  if (hitBox(x, y, xs[6], CHIP_Y, ws[6], CHIP_H)) openWifi();
}

static void xptBegin() {
  digitalWrite(LCD_CS, HIGH);
  lcdSpi.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
  digitalWrite(TP_CS, LOW);
}

static void xptEnd() {
  digitalWrite(TP_CS, HIGH);
  lcdSpi.endTransaction();
}

static void xptReadXY(uint16_t &x, uint16_t &y) {
  xptBegin();
  lcdSpi.transfer(0xD0);
  lcdSpi.transfer(0);
  lcdSpi.transfer(0xD0);
  lcdSpi.transfer(0);
  lcdSpi.transfer(0xD0);
  lcdSpi.transfer(0);
  lcdSpi.transfer(0xD0);
  uint16_t tmp = lcdSpi.transfer(0);
  tmp = tmp << 5;
  tmp |= 0x1F & (lcdSpi.transfer(0x90) >> 3);
  x = tmp;
  lcdSpi.transfer(0);
  lcdSpi.transfer(0x90);
  lcdSpi.transfer(0);
  lcdSpi.transfer(0x90);
  lcdSpi.transfer(0);
  lcdSpi.transfer(0x90);
  tmp = lcdSpi.transfer(0);
  tmp = tmp << 5;
  tmp |= 0x1F & (lcdSpi.transfer(0) >> 3);
  y = tmp;
  xptEnd();
}

static uint16_t xptReadZ() {
  xptBegin();
  int16_t tz = 0xFFF;
  lcdSpi.transfer(0xB0);
  tz += (int16_t)(lcdSpi.transfer16(0xC0) >> 3);
  tz -= (int16_t)(lcdSpi.transfer16(0) >> 3);
  xptEnd();
  if (tz == 4095 || tz < 0) tz = 0;
  return (uint16_t)tz;
}

static bool xptReadRaw(uint16_t &rx, uint16_t &ry, uint16_t &z) {
  uint16_t z1 = 1, z2 = 0;
  for (int n = 0; n < 8 && z1 > z2; n++) {
    z2 = z1;
    z1 = xptReadZ();
    delay(1);
  }
  if (z1 <= 100) return false;
  uint16_t x1, y1, x2, y2;
  xptReadXY(x1, y1);
  delay(1);
  if (xptReadZ() <= 100) return false;
  delay(2);
  xptReadXY(x2, y2);
  if (abs((int)x1 - (int)x2) > 20 || abs((int)y1 - (int)y2) > 20) return false;
  rx = x1;
  ry = y1;
  z = z1;
  return true;
}

static bool mapRawToScreen(uint16_t rx, uint16_t ry, int &sx, int &sy) {
  if (!touchCal.valid || !touchCal.x1 || !touchCal.y1) return false;
  int xx, yy;
  if (touchCal.flags & 1) {
    xx = ((int)ry - (int)touchCal.x0) * PW / (int)touchCal.x1;
    yy = ((int)rx - (int)touchCal.y0) * PH / (int)touchCal.y1;
  } else {
    xx = ((int)rx - (int)touchCal.x0) * PW / (int)touchCal.x1;
    yy = ((int)ry - (int)touchCal.y0) * PH / (int)touchCal.y1;
  }
  if (touchCal.flags & 2) xx = PW - xx;
  if (touchCal.flags & 4) yy = PH - yy;
  if (xx < 0) xx = 0;
  if (yy < 0) yy = 0;
  if (xx > PW - 1) xx = PW - 1;
  if (yy > PH - 1) yy = PH - 1;
  sx = xx;
  sy = yy;
  return true;
}

static bool loadTouchCal() {
  prefs.begin("xptcal", true);
  bool ok = prefs.getBytesLength("e32") == sizeof(touchCal);
  if (ok) prefs.getBytes("e32", &touchCal, sizeof(touchCal));
  prefs.end();
  return ok && touchCal.valid && touchCal.x1 && touchCal.y1;
}

static void saveTouchCal() {
  touchCal.valid = true;
  prefs.begin("xptcal", false);
  prefs.putBytes("e32", &touchCal, sizeof(touchCal));
  prefs.end();
}

static void drawCornerArrow(int which) {
  const int s = 28;
  gfx->fillScreen(COL_BLK);
  gfx->setTextColor(COL_WHT, COL_BLK);
  gfx->setTextSize(2);
  char step[20];
  snprintf(step, sizeof(step), "CORNER %d OF 4", which + 1);
  centre(step, CX, 176 - 20);
  centre("TOUCH THE ARROW", CX, 176 + 16);
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  if (which == 0) {
    x0 = 0;
    y0 = 0;
    x1 = s;
    y1 = 0;
    x2 = 0;
    y2 = s;
  } else if (which == 1) {
    x0 = 0;
    y0 = PH - 1;
    x1 = s;
    y1 = PH - 1;
    x2 = 0;
    y2 = PH - 1 - s;
  } else if (which == 2) {
    x0 = PW - 1;
    y0 = 0;
    x1 = PW - 1 - s;
    y1 = 0;
    x2 = PW - 1;
    y2 = s;
  } else {
    x0 = PW - 1;
    y0 = PH - 1;
    x1 = PW - 1 - s;
    y1 = PH - 1;
    x2 = PW - 1;
    y2 = PH - 1 - s;
  }
  gfx->drawLine(x0, y0, x1, y1, COL_WHT);
  gfx->drawLine(x0, y0, x2, y2, COL_WHT);
  gfx->drawLine(x1, y1, x2, y2, COL_WHT);
}

static bool calibrateTouchScreen() {
  int values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint16_t rrx, rry, rz;
  for (int i = 0; i < 4; i++) {
    drawCornerArrow(i);
    if (i > 0) delay(1000);
    for (int j = 0; j < 8; j++) {
      while (!xptReadRaw(rrx, rry, rz)) delay(4);
      values[i * 2] += rrx;
      values[i * 2 + 1] += rry;
    }
    values[i * 2] /= 8;
    values[i * 2 + 1] /= 8;
  }
  bool rotate = false;
  int x0, x1, y0, y1;
  if (abs(values[0] - values[2]) > abs(values[1] - values[3])) {
    rotate = true;
    x0 = (values[1] + values[3]) / 2;
    x1 = (values[5] + values[7]) / 2;
    y0 = (values[0] + values[4]) / 2;
    y1 = (values[2] + values[6]) / 2;
  } else {
    x0 = (values[0] + values[2]) / 2;
    x1 = (values[4] + values[6]) / 2;
    y0 = (values[1] + values[5]) / 2;
    y1 = (values[3] + values[7]) / 2;
  }
  bool invx = false, invy = false;
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
    invx = true;
  }
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
    invy = true;
  }
  x1 -= x0;
  y1 -= y0;
  if (x0 == 0) x0 = 1;
  if (x1 == 0) x1 = 1;
  if (y0 == 0) y0 = 1;
  if (y1 == 0) y1 = 1;
  touchCal.x0 = (uint16_t)x0;
  touchCal.x1 = (uint16_t)x1;
  touchCal.y0 = (uint16_t)y0;
  touchCal.y1 = (uint16_t)y1;
  touchCal.flags = (rotate ? 1 : 0) | (invx ? 2 : 0) | (invy ? 4 : 0);
  saveTouchCal();
  return true;
}

int readTouch(int *sx, int *sy) {
  uint16_t rx, ry, z;
  if (!xptReadRaw(rx, ry, z)) return 0;
  int x, y;
  if (!mapRawToScreen(rx, ry, x, y)) return 0;
  *sx = x;
  *sy = y;
  return 1;
}

static void noteHttp(HTTPClient &http) {
  http.setTimeout(15000);
  http.setConnectTimeout(4000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent("DeskCoin/1.0");
}

static bool quoteField(const char *body, const char *key, int which, float *out) {
  const char *p = strstr(body, key);
  if (!p) return false;
  p += strlen(key);
  for (int i = 0; i <= which; i++) {
    const char *a = strchr(p, '"');
    if (!a) return false;
    if (i == which) {
      *out = (float)atof(a + 1);
      return *out > 0.f;
    }
    const char *b = strchr(a + 1, '"');
    if (!b) return false;
    p = b + 1;
  }
  return false;
}

static bool pullTicker(const char *pair, float *last, float *openPxOut, float *hi, float *lo, float *vol) {
  char url[96];
  snprintf(url, sizeof(url), "https://api.kraken.com/0/public/Ticker?pair=%s", pair);
  char body[1400];
  body[0] = 0;
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  noteHttp(http);
  bool ok = false;
  if (http.begin(cli, url)) {
    int code = http.GET();
    if (code == 200) {
      int got = http.getStream().readBytes(body, sizeof(body) - 1);
      if (got < 0) got = 0;
      body[got] = 0;
      if (got > 20 && !strstr(body, "\"error\":[\"")) {
        ok = quoteField(body, "\"c\":", 0, last) && quoteField(body, "\"o\":", 0, openPxOut) &&
             quoteField(body, "\"h\":", 1, hi) && quoteField(body, "\"l\":", 1, lo);
        if (ok) quoteField(body, "\"v\":", 1, vol);
      }
    }
    http.end();
  }
  cli.stop();
  return ok;
}

static void takeCandles(char *win, int *fill, Candle *out, int *n) {
  int f = *fill;
  int i = 0;
  while (i < f) {
    if (win[i] != '[') {
      i++;
      continue;
    }
    if (i + 1 >= f) break;
    if (win[i + 1] < '0' || win[i + 1] > '9') {
      i++;
      continue;
    }
    int q = i + 1;
    float v[4];
    int got = 0;
    bool shortBuf = false;
    while (got < 4) {
      int a = -1;
      for (int k = q; k < f; k++) {
        if (win[k] == '"') {
          a = k;
          break;
        }
      }
      if (a < 0) {
        shortBuf = true;
        break;
      }
      int b = -1;
      for (int k = a + 1; k < f; k++) {
        if (win[k] == '"') {
          b = k;
          break;
        }
      }
      if (b < 0) {
        shortBuf = true;
        break;
      }
      int len = b - (a + 1);
      if (len <= 0 || len >= 20) {
        q = b + 1;
        got = -1;
        break;
      }
      char num[20];
      memcpy(num, win + a + 1, (size_t)len);
      num[len] = 0;
      v[got++] = (float)atof(num);
      q = b + 1;
    }
    if (shortBuf) break;
    if (got == 4 && v[1] > 0.f && v[2] > 0.f) {
      Candle c;
      c.o = v[0];
      c.h = v[1];
      c.l = v[2];
      c.c = v[3];
      if (*n < MAX_C) out[(*n)++] = c;
      else {
        memmove(out, out + 1, (MAX_C - 1) * sizeof(Candle));
        out[MAX_C - 1] = c;
      }
      i = q;
    } else
      i = (q > i) ? q : i + 1;
  }
  if (i > 0 && i <= f) {
    memmove(win, win + i, (size_t)(f - i));
    *fill = f - i;
    win[*fill] = 0;
  }
}

static bool pullCandles(const char *pair, int rng, Candle *out, int *nout) {
  *nout = 0;
  char url[128];
  time_t now = time(nullptr);
  if (now > 1700000000)
    snprintf(url, sizeof(url), "https://api.kraken.com/0/public/OHLC?pair=%s&interval=%d&since=%ld", pair,
             RANGES[rng].interval, (long)(now - RANGES[rng].span));
  else
    snprintf(url, sizeof(url), "https://api.kraken.com/0/public/OHLC?pair=%s&interval=%d", pair, RANGES[rng].interval);

  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  noteHttp(http);
  bool bad = false;
  bool opened = http.begin(cli, url);
  if (opened) {
    int code = http.GET();
    if (code == 200) {
      WiFiClient *stream = http.getStreamPtr();
      stream->setTimeout(2000);
      char win[768];
      int fill = 0;
      bool checked = false;
      uint32_t t0 = millis();
      while (millis() - t0 < 18000) {
        int room = (int)sizeof(win) - 1 - fill;
        if (room < 64) {
          takeCandles(win, &fill, out, nout);
          if (fill > 500) {
            int drop = 200;
            if (drop > fill) drop = fill;
            memmove(win, win + drop, (size_t)(fill - drop));
            fill -= drop;
            win[fill] = 0;
          }
          room = (int)sizeof(win) - 1 - fill;
          if (room < 64) break;
        }
        int av = stream->available();
        if (av <= 0) {
          if (!http.connected()) break;
          delay(2);
          continue;
        }
        if (av > room) av = room;
        int r = stream->read((uint8_t *)win + fill, (size_t)av);
        if (r <= 0) break;
        fill += r;
        win[fill] = 0;
        if (!checked && fill > 24) {
          checked = true;
          if (strstr(win, "\"error\":[\"")) bad = true;
        }
        if (bad) break;
        takeCandles(win, &fill, out, nout);
      }
      if (!bad) takeCandles(win, &fill, out, nout);
    } else
      bad = true;
    http.end();
  }
  cli.stop();
  return opened && !bad && *nout >= 2;
}

static bool pullLive() {
  int coin = coinIx;
  int rng = rangeIx;
  bool wantC = forceCandles || nCandles < 2 || (uint32_t)millis() >= nextCandleAt;
  forceCandles = false;
  if (coin < 0 || coin >= NCOIN || rng < 0 || rng > 2) return false;
  const char *pair = COINS[coin].pair;
  float last = 0, op = 0, hi = 0, lo = 0, vol = 0;
  bool tok = pullTicker(pair, &last, &op, &hi, &lo, &vol);
  Candle tmp[MAX_C];
  int nc = 0;
  bool cok = false;
  if (tok && wantC) cok = pullCandles(pair, rng, tmp, &nc);
  if (coin != coinIx || rng != rangeIx) return false;
  if (!tok) {
    nextPxAt = millis() + FAIL_MS;
    nextCandleAt = millis() + FAIL_MS;
    autoArm = true;
    Serial.println("price failed");
    if (quoteOk) return false;
    strlcpy(quoteErr, "price failed", sizeof(quoteErr));
    return true;
  }
  lastPx = last;
  openPx = op;
  hiPx = hi;
  loPx = lo;
  vol24 = vol;
  quoteOk = true;
  quoteAt = millis();
  quoteErr[0] = 0;
  if (cok && nc >= 2) {
    memcpy(candles, tmp, (size_t)nc * sizeof(Candle));
    nCandles = nc;
    nextCandleAt = millis() + CANDLE_MS;
  } else if (wantC) {
    nextCandleAt = millis() + FAIL_MS;
    if (nCandles < 2) strlcpy(quoteErr, "candles failed", sizeof(quoteErr));
  }
  nextPxAt = millis() + (autoOn ? COIN_MS : PRICE_MS);
  autoArm = true;
  Serial.printf("live %s %s candles %d\n", COINS[coin].name, RANGES[rng].chip, nCandles);
  return true;
}

void netTask(void *) {
  for (;;) {
    if (netPend && wifiOk) {
      netPend = 0;
      netBusy = true;
      bool drew = pullLive();
      netBusy = false;
      if (drew && page == PG_BOARD) boardDirty = true;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

void serviceWifi() {
  if (wifiSsid[0] == 0) {
    wifiOk = false;
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiOk) {
      wifiOk = true;
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      applyTz();
      autoArm = false;
      forceCandles = true;
      nextPxAt = millis();
      netPend = 1;
    }
    return;
  }
  wifiOk = false;
  if (page == PG_WIFI) return;
  if (millis() - lastWifiTry < 30000UL) return;
  lastWifiTry = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
}

void serviceBoard() {
  if (page != PG_BOARD || !wifiOk || netBusy || netPend) return;
  if ((uint32_t)millis() < nextPxAt) return;
  if (autoOn && autoArm) {
    coinIx = (coinIx + 1) % NCOIN;
    nCandles = 0;
    quoteOk = false;
    strlcpy(quoteErr, "waiting", sizeof(quoteErr));
    forceCandles = true;
    boardDirty = true;
  } else if ((uint32_t)millis() >= nextCandleAt || nCandles < 2)
    forceCandles = true;
  autoArm = true;
  nextPxAt = millis() + 30000UL;
  netPend = 1;
}

static int sx4(int x) { return (int)((int32_t)x * PANEL_W / PW); }
static int sy4(int y) { return (int)((int32_t)y * PANEL_H / PH); }
static int sw4(int x, int w) {
  int d = sx4(x + w) - sx4(x);
  return d > 0 ? d : (w > 0 ? 1 : 0);
}
static int sh4(int y, int h) {
  int d = sy4(y + h) - sy4(y);
  return d > 0 ? d : (h > 0 ? 1 : 0);
}

class FitGfx : public Arduino_GFX {
 public:
  explicit FitGfx(Arduino_GFX *p) : Arduino_GFX(PW, PH), panel(p) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    return ((Arduino_ST7796 *)panel)->begin(speed, SPI_MODE0);
  }
  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    panel->drawPixel(sx4(x), sy4(y), color);
  }
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override {
    panel->fillRect(sx4(x), sy4(y), sw4(x, w), sh4(y, h), color);
  }
  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    panel->drawFastHLine(sx4(x), sy4(y), sw4(x, w), color);
  }
  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    panel->drawFastVLine(sx4(x), sy4(y), sh4(y, h), color);
  }
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t *bitmap, int16_t w, int16_t h) override {
    int dx = sx4(x), dy = sy4(y), dw = sw4(x, w), dh = sh4(y, h);
    if (dw < 1 || dh < 1 || dw > PANEL_W) return;
    uint16_t row[PANEL_W];
    for (int j = 0; j < dh; j++) {
      int sy = (j * h) / dh;
      if (sy >= h) sy = h - 1;
      for (int i = 0; i < dw; i++) {
        int sx = (i * w) / dw;
        if (sx >= w) sx = w - 1;
        row[i] = bitmap[sy * w + sx];
      }
      panel->draw16bitRGBBitmap(dx, dy + j, row, dw, 1);
    }
  }
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) override {
    draw16bitRGBBitmap(x, y, (uint16_t *)bitmap, w, h);
  }
  Arduino_GFX *panel;
};

void connectWifi() {
  loadPrefs();
  if (wifiSsid[0] == 0) {
    openWifi();
    return;
  }
  gfx->fillScreen(COL_BG);
  gfx->setTextColor(COL_HOT, COL_BG);
  gfx->setTextSize(2);
  centre("linking", CX, 200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(50);
  wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    applyTz();
    nextPxAt = millis();
    forceCandles = true;
    netPend = 1;
  } else {
    strlcpy(quoteErr, "no wifi", sizeof(quoteErr));
    nextPxAt = millis() + FAIL_MS;
  }
  lastWifiTry = millis();
  drawBoard();
}

void setup() {
  cols();
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  pinMode(TP_CS, OUTPUT);
  digitalWrite(TP_CS, HIGH);
  pinMode(TP_IRQ, INPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  Serial.begin(115200);
  delay(200);
  lcdBus = new Arduino_HWSPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, LCD_MISO, &lcdSpi, true);
  panel = new Arduino_ST7796(lcdBus, GFX_NOT_DEFINED, 3, false);
  gfx = new FitGfx(panel);
  if (!gfx->begin(20000000)) {
    Serial.println("gfx begin failed");
    return;
  }
  digitalWrite(GFX_BL, HIGH);
  gfx->fillScreen(COL_BG);
  gfx->setTextWrap(false);
  gpio_pullup_en((gpio_num_t)LCD_MISO);
  if (!loadTouchCal()) {
    while (!calibrateTouchScreen()) delay(200);
  }
  xTaskCreatePinnedToCore(netTask, "dcnet", 32768, nullptr, 1, nullptr, 0);
  connectWifi();
  applyBl();
}

void loop() {
  pollWifiScan();
  serviceWifi();
  serviceBoard();
  if (boardDirty && !netBusy && page == PG_BOARD) {
    boardDirty = false;
    drawBoard();
  }
  int x = 0, y = 0;
  int n = readTouch(&x, &y);
  static bool wasDown = false;
  if (n == 1 && !wasDown) {
    wasDown = true;
    if (millis() - lastTap > 280UL) {
      lastTap = millis();
      onTap(x, y);
    }
  } else if (n == 0)
    wasDown = false;
  delay(8);
}
