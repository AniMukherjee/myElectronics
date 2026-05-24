/*
 * Hexa Clock: Digital + Analog + Mario + Tetris + Word Clock + LIVE NEWS Ticker
 * ESP32 + 64x64 HUB75 RGB Matrix
 *
 * Touch GPIO 33 (T8) to cycle modes.
 * Double-buffering ALWAYS ON — zero flicker.
 */

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <Fonts/Org_01.h>
#include <TetrisMatrixDraw.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "esp_timer.h"

  
// =====================================================================
//  DATA STRUCTURES & ENUMS (Placed at top to prevent Arduino IDE prototype errors)
// =====================================================================
enum ClockMode { MODE_DIGITAL, MODE_ANALOG, MODE_MARIO, MODE_TETRIS, MODE_WORD, MODE_NEWS };

struct SegRect { int8_t x, y, w, h; };
struct Digit { int val, target, morphStep, px, py; bool sm; uint16_t col; };

enum MarioState { M_IDLE, M_JUMPING, M_HIT };
enum MarioDir   { M_UP, M_DOWN };

struct WordLoc { int r, c, len; };

struct NewsItem { String source; String text; };

// =====================================================================
//  PINS
// =====================================================================
#define R1_PIN  25
#define G1_PIN  26
#define B1_PIN  27
#define R2_PIN  14
#define G2_PIN  12
#define B2_PIN  13
#define A_PIN   23
#define B_PIN   19
#define C_PIN    5
#define D_PIN   17
#define E_PIN   32
#define LAT_PIN  4
#define OE_PIN  15
#define CLK_PIN 16

// =====================================================================
//  TOUCH
// =====================================================================
#define TOUCH_PIN        T8    // GPIO 33
#define TOUCH_THRESHOLD  40    // below = touched (tune if needed)
#define TOUCH_DEBOUNCE   1000  // ms

// =====================================================================
//  DISPLAY
// =====================================================================
HUB75_I2S_CFG::i2s_pins _pins = {
  R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
  A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
  LAT_PIN, OE_PIN, CLK_PIN
};
HUB75_I2S_CFG mxconfig(64, 64, 1, _pins);
MatrixPanel_I2S_DMA *display = nullptr;

// =====================================================================
//  WiFi / NTP
// =====================================================================
const char* ssid      = "BATTERYROCKS_5G";
const char* pass      = "b@tteryis@b@dboy";
const long  gmtOffset = 19800;   // IST = UTC+5:30
const int   dstOffset = 0;
const char* ntpServer = "pool.ntp.org";

struct tm     timeinfo;
bool          timeSynced = false;
unsigned long lastNTPSync = 0;
#define NTP_RESYNC_MS 3600000UL

void syncTime() {
  configTime(gmtOffset, dstOffset, ntpServer);
  
  int attempts = 0;
  // getLocalTime default timeout is 5000ms. We override it to 500ms per attempt.
  // 10 attempts = maximum 5 seconds of waiting before moving on.
  while (!getLocalTime(&timeinfo, 500) && attempts < 10) {
    attempts++;
  }
  
  // If we broke the loop before 10 attempts, it successfully synced
  timeSynced = (attempts < 10);
}

ClockMode     currentMode   = MODE_DIGITAL;
unsigned long lastTouchTime = 0;
unsigned long lastTouchPoll = 0;

// =====================================================================
// 1. DIGITAL CLOCK ENGINE
// =====================================================================
uint16_t CLR_BLACK, CLR_DIGIT, CLR_COLON, CLR_DATE, CLR_SYNC;

#define ANIM_STEPS  8
#define FRAME_MS    25   

#define CLOCK_Y   8
#define DX_H1     4
#define DX_H2    14
#define DX_COL1  25
#define DX_M1    29
#define DX_M2    39
#define DX_S1    51
#define DX_S2    57

const uint8_t SEGMENTS[10] = { 0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011, 0b1011011, 0b1011111, 0b1110000, 0b1111111, 0b1111011 };
const SegRect SEG_BIG[7] = { {1,0,7,2}, {7,1,2,6}, {7,8,2,6}, {1,13,7,2}, {0,8,2,6}, {0,1,2,6}, {1,6,7,2} };
const SegRect SEG_SMALL[7] = { {1,0,3,1}, {4,1,1,3}, {4,5,1,3}, {1,8,3,1}, {0,5,1,3}, {0,1,1,3}, {1,4,3,1} };

void drawSeg(int px, int py, int s, bool sm, uint16_t col) {
  const SegRect& r = sm ? SEG_SMALL[s] : SEG_BIG[s];
  display->fillRect(px+r.x, py+r.y, r.w, r.h, col);
}

void drawSegMorph(int px, int py, int s, bool appearing, int step, bool sm, uint16_t col) {
  const SegRect& r = sm ? SEG_SMALL[s] : SEG_BIG[s];
  int sx=px+r.x, sy=py+r.y, sw=r.w, sh=r.h;
  bool horiz = (sw > sh);
  display->fillRect(sx, sy, sw, sh, CLR_BLACK); 
  if (horiz) {
    int w = appearing ? (sw*step + ANIM_STEPS-1)/ANIM_STEPS : (sw*(ANIM_STEPS-step) + ANIM_STEPS-1)/ANIM_STEPS;
    w = constrain(w, 0, sw);
    if (w > 0) display->fillRect(sx+(sw-w)/2, sy, w, sh, col);
  } else {
    int h = appearing ? (sh*step + ANIM_STEPS-1)/ANIM_STEPS : (sh*(ANIM_STEPS-step) + ANIM_STEPS-1)/ANIM_STEPS;
    h = constrain(h, 0, sh);
    if (h > 0) display->fillRect(sx, sy+(sh-h)/2, sw, h, col);
  }
}

void drawDigit(int px, int py, int val, bool sm, uint16_t col) {
  uint8_t segs = SEGMENTS[val];
  for (int s=0; s<7; s++) drawSeg(px, py, s, sm, ((segs>>(6-s))&1) ? col : CLR_BLACK);
}

void morphDigit(int px, int py, int from, int to, int step, bool sm, uint16_t col) {
  if (from == to) { drawDigit(px, py, to, sm, col); return; }
  uint8_t sF=SEGMENTS[from], sT=SEGMENTS[to];
  for (int s=0; s<7; s++) {
    bool wOn=(sF>>(6-s))&1, iOn=(sT>>(6-s))&1;
    if (wOn && iOn) drawSeg(px,py,s,sm,col);
    else if (!wOn && !iOn) drawSeg(px,py,s,sm,CLR_BLACK);
    else if (!wOn && iOn) drawSegMorph(px,py,s,true, step,sm,col);
    else drawSegMorph(px,py,s,false,step,sm,col);
  }
}

void drawColon(int cx, int py, uint16_t col) {
  display->fillRect(cx, py+4, 2, 2, col);
  display->fillRect(cx, py+10, 2, 2, col);
}

void drawCorner() {
  uint16_t w = display->color565(255,255,255);
  display->drawLine(0,0, 5,0, w); display->drawLine(0,1, 4,1, w);
  display->drawLine(0,0, 0,5, w); display->drawLine(1,0, 1,4, w);
  display->drawLine(58,0, 63,0, w); display->drawLine(59,1, 63,1, w);
  display->drawLine(63,0, 63,5, w); display->drawLine(62,1, 62,4, w);
  display->drawLine(0,62, 4,62, w); display->drawLine(0,63, 5,63, w);
  display->drawLine(0,58, 0,63, w); display->drawLine(1,59, 1,63, w);
  display->drawLine(59,62,63,62,w); display->drawLine(58,63,63,63,w);
  display->drawLine(63,58,63,63,w); display->drawLine(62,59,62,63,w);
}

Digit digits[6];
int lastSecDig = -1;

void initDigits() {
  CLR_DIGIT = display->color565(0, 150, 255);
  digits[0] = {0,0,0, DX_H1, CLOCK_Y,   false, CLR_DIGIT};
  digits[1] = {0,0,0, DX_H2, CLOCK_Y,   false, CLR_DIGIT};
  digits[2] = {0,0,0, DX_M1, CLOCK_Y,   false, CLR_DIGIT};
  digits[3] = {0,0,0, DX_M2, CLOCK_Y,   false, CLR_DIGIT};
  digits[4] = {0,0,0, DX_S1, CLOCK_Y+6, true,  CLR_DIGIT};
  digits[5] = {0,0,0, DX_S2, CLOCK_Y+6, true,  CLR_DIGIT};
}

void renderDateDigital() {
  if (!timeSynced) return;
  char d1[32], d2[32], d3[32];
  strftime(d1, sizeof(d1), "%d %b", &timeinfo);
  strftime(d2, sizeof(d2), "%A",    &timeinfo);
  strftime(d3, sizeof(d3), "%Y",    &timeinfo);
  for (int i=0; d1[i]; i++) d1[i]=toupper(d1[i]);
  for (int i=0; d2[i]; i++) d2[i]=toupper(d2[i]);

  display->fillRect(0, 26, 64, 32, CLR_BLACK);
  display->setFont(); display->setTextSize(1);
  display->setTextColor(display->color565(255, 75, 10));
  display->setCursor((64-strlen(d1)*6)/2, 28); display->print(d1);
  display->setTextColor(display->color565(250, 225, 10));
  display->setCursor((64-strlen(d2)*6)/2, 38); display->print(d2);
  display->setTextColor(display->color565(150, 255, 10));
  display->setCursor((64-strlen(d3)*6)/2, 48); display->print(d3);
}

void updateDigitalState() {
  if (!timeSynced) return;
  getLocalTime(&timeinfo);
  int h=timeinfo.tm_hour, m=timeinfo.tm_min, s=timeinfo.tm_sec;
  if (s == lastSecDig) return;
  lastSecDig = s;
  int nv[6]={h/10,h%10,m/10,m%10,s/10,s%10};
  for (int i=0; i<6; i++) {
    if (nv[i]!=digits[i].val && digits[i].morphStep==0) { 
      digits[i].target=nv[i]; digits[i].morphStep=1; 
    }
  }
}

void renderDigitalFrame() {
  for (int i=0; i<6; i++) {
    if (digits[i].morphStep > 0) {
      morphDigit(digits[i].px, digits[i].py, digits[i].val, digits[i].target, digits[i].morphStep, digits[i].sm, digits[i].col);
      digits[i].morphStep++;
      if (digits[i].morphStep > ANIM_STEPS) {
        digits[i].val = digits[i].target;
        digits[i].morphStep = 0;
      }
    } else {
      drawDigit(digits[i].px, digits[i].py, digits[i].val, digits[i].sm, digits[i].col);
    }
  }
  drawColon(DX_COL1, CLOCK_Y, CLR_COLON);
  drawCorner();
  renderDateDigital();
  display->flipDMABuffer();
}

// =====================================================================
// 2. ANALOG CLOCK ENGINE (With Retro Space Invaders)
// =====================================================================
const int CX=32, CY=25, CRADIUS=25;
int lastSecAna = -1;
int64_t secBoundaryUs = 0;

// Space Invader A (8x8) - 2 Frames for animation
const uint8_t INVADER_A1[8] = { 0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x5A, 0x81, 0x42 };
const uint8_t INVADER_A2[8] = { 0x18, 0x3C, 0x7E, 0xDB, 0xFF, 0x24, 0x5A, 0x81 };

// Space Invader B (8x8) - 2 Frames for animation
const uint8_t INVADER_B1[8] = { 0x24, 0x24, 0x7E, 0xDB, 0xFF, 0xFF, 0xA5, 0x24 };
const uint8_t INVADER_B2[8] = { 0x24, 0x42, 0x7E, 0xDB, 0xFF, 0x7E, 0x24, 0x5A };

void drawSprite8x8(int x, int y, const uint8_t *sprite, uint16_t color) {
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (sprite[i] & (1 << (7 - j))) display->drawPixel(x + j, y + i, color);
    }
  }
}

void drawAnalogDecorations() {
  // Toggle frame every second using the actual clock time
  bool altFrame = (timeinfo.tm_sec % 2 == 0);
  
  uint16_t c_green   = display->color565(50, 255, 50);
  uint16_t c_magenta = display->color565(255, 50, 255);
  uint16_t c_cyan    = display->color565(50, 255, 255);
  uint16_t c_yellow  = display->color565(255, 255, 50);
  
  // Top Left (Invader A)
  drawSprite8x8(2, 2, altFrame ? INVADER_A1 : INVADER_A2, c_green);
  // Top Right (Invader B)
  drawSprite8x8(54, 2, altFrame ? INVADER_B1 : INVADER_B2, c_magenta);
  // Bottom Left (Invader B)
  drawSprite8x8(2, 41, altFrame ? INVADER_B2 : INVADER_B1, c_cyan);
  // Bottom Right (Invader A)
  drawSprite8x8(54, 41, altFrame ? INVADER_A2 : INVADER_A1, c_yellow);
}

void drawClockFace() {
  display->drawCircle(CX,CY,CRADIUS,   display->color565(255,180, 0));
  display->drawCircle(CX,CY,CRADIUS-1, display->color565(255,180, 0));
  display->setFont(&Org_01);
  display->setTextColor(display->color565(255,255,255));
  display->setCursor(CX-3,  7); display->print("12");
  display->setCursor(CX+18, 27); display->print("3");
  display->setCursor(CX-2,  47); display->print("6");
  display->setCursor(CX-22, 27); display->print("9");
  display->setFont();
  for (int i=0; i<12; i++) {
    if (i%3 != 0) {
      float a = (i*30.0-90.0)*PI/180.0;
      int mx = CX+(CRADIUS-4)*cos(a), my = CY+(CRADIUS-4)*sin(a);
      display->fillCircle(mx, my, 1, display->color565(255,255,255));
    }
  }
}

void drawHand(float angle, int length, uint16_t col, int thickness) {
  float rad = (angle-90.0)*PI/180.0;
  int x = CX+length*cos(rad), y = CY+length*sin(rad);
  if (thickness <= 1) {
    display->drawLine(CX,CY,x,y,col);
  } else {
    for (int t=-thickness/2; t<=thickness/2; t++) {
      display->drawLine(CX+t,CY,  x+t,y,  col);
      display->drawLine(CX,  CY+t,x,  y+t,col);
    }
  }
}

void drawDateInfoAna() {
  const char* wd[7]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* mo[12]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char dayStr[3];
  sprintf(dayStr, "%02d", timeinfo.tm_mday);
  
  display->setFont(); display->setTextSize(1);
  display->setTextColor(display->color565(255, 75, 10));
  display->setCursor(2, 55);  display->print(mo[timeinfo.tm_mon]);
  display->setTextColor(display->color565(50, 255, 255));
  display->setCursor((64 - strlen(dayStr)*6)/2, 55); display->print(dayStr);
  display->setTextColor(display->color565(150, 255, 10));
  display->setCursor(45,  55); display->print(wd[timeinfo.tm_wday]);
}

void renderAnalogFrame() {
  if (!timeSynced) return;
  getLocalTime(&timeinfo);
  if (timeinfo.tm_sec != lastSecAna) {
    lastSecAna = timeinfo.tm_sec;
    secBoundaryUs = esp_timer_get_time();
  }
  float frac = constrain((esp_timer_get_time() - secBoundaryUs) / 1000000.0f, 0.0f, 0.9999f);
  float ss = timeinfo.tm_sec + frac;                    
  float mm = timeinfo.tm_min + ss / 60.0f;             
  float hh = (timeinfo.tm_hour % 12) + mm / 60.0f;       

  display->fillScreen(0);
  
  drawAnalogDecorations(); // Renders Animated Space Invaders & Stars
  drawClockFace();
  
  drawHand(hh * 30.0f,  12, display->color565(  0, 255, 220), 3);
  drawHand(mm *  6.0f,  16, display->color565(255, 50,  255), 2);
  drawHand(ss *  6.0f,  18, display->color565(255,   0,   0), 1);
  display->fillCircle(CX, CY, 2, display->color565(255, 200, 50));
  display->fillCircle(CX, CY, 1, 0);
  drawDateInfoAna();
  display->flipDMABuffer();
}

// =====================================================================
// 3. MARIO CLOCK ENGINE
// =====================================================================
const uint16_t SKY_COLOR = 0x000E;
const uint16_t _MASK     = SKY_COLOR;
const uint16_t M_RED     = 0xF801;
const uint16_t M_SKIN    = 0xfd28;
const uint16_t M_SHOES   = 0xC300;
const uint16_t M_SHIRT   = 0x0560;
const uint16_t M_HAIR    = 0x0000;

const uint8_t Super_Mario_Bros__24pt7bBitmaps[] PROGMEM = {
  0x00, 0xFF, 0xEC, 0x30, 0xDE, 0xF6, 0x6C, 0xDB, 0xFB, 0x6F, 0xED, 0x9B, 0x00, 0x10, 0xFB, 0x83, 0xE0, 0xFF, 0x84, 0x00, 0xC7, 0x9C, 0x71, 0xC7, 0x1C, 0xF1, 0x80, 0x61, 0xA3, 0x5B, 0xED, 0x99,
  0x9D, 0x80, 0xFC, 0x36, 0xCC, 0xC6, 0x30, 0xC6, 0x33, 0x36, 0xC0, 0x6C, 0x73, 0xF9, 0xC6, 0xC0, 0x30, 0xCF, 0xCC, 0x30, 0x6F, 0x00, 0xFF, 0xF0, 0xF0, 0x06, 0x1C, 0x71, 0xC7, 0x1C, 0x30, 0x00,
  0x7D, 0x9F, 0x3E, 0x7C, 0xF9, 0xDF, 0x00, 0x77, 0x9C, 0xE7, 0x3B, 0xE0, 0x7D, 0x9C, 0x39, 0xE7, 0x1C, 0x3F, 0x80, 0x7E, 0x18, 0xE0, 0x60, 0xF9, 0xDF, 0x00, 0x1C, 0x79, 0xB6, 0x6F, 0xE1, 0x83,
  0x00, 0xFD, 0x83, 0xF0, 0x70, 0xF9, 0xDF, 0x00, 0x7D, 0x83, 0xF6, 0x7C, 0xF9, 0xDF, 0x00, 0xFF, 0x9C, 0x30, 0xC3, 0x06, 0x0C, 0x00, 0x7D, 0x9F, 0x3B, 0xEC, 0xF9, 0xDF, 0x00, 0x7D, 0x9F, 0x3B,
  0xF0, 0xE1, 0xDF, 0x00, 0xF3, 0xC0, 0x6C, 0x37, 0x80, 0x19, 0x99, 0x86, 0x18, 0x60, 0xF8, 0x01, 0xF0, 0xC3, 0x0C, 0x33, 0x33, 0x00, 0x7D, 0x8F, 0x18, 0xE3, 0x00, 0x0C, 0x00, 0x7D, 0x06, 0xED,
  0x5B, 0xF0, 0x1F, 0x00, 0x38, 0xFB, 0x9F, 0x3F, 0xFC, 0xF9, 0x80, 0xFD, 0xCF, 0x9F, 0xEE, 0x7C, 0xFF, 0x00, 0x3C, 0xCF, 0x87, 0x0E, 0x0C, 0xCF, 0x00, 0xF9, 0xDB, 0x9F, 0x3E, 0x7D, 0xBE, 0x00,
  0xFF, 0xC3, 0x87, 0xEE, 0x1C, 0x3F, 0x80, 0xFF, 0xC3, 0x87, 0xEE, 0x1C, 0x38, 0x00, 0x3C, 0xC3, 0x87, 0x7E, 0x6C, 0xCF, 0x80, 0xE7, 0xCF, 0x9F, 0xFE, 0x7C, 0xF9, 0x80, 0xFB, 0x9C, 0xE7, 0x3B,
  0xE0, 0x1E, 0x0C, 0x18, 0x3E, 0x7C, 0xDF, 0x00, 0xE7, 0xDB, 0xE7, 0x8F, 0x9D, 0xB9, 0x80, 0xE3, 0x8E, 0x38, 0xE3, 0x8F, 0xC0, 0xC7, 0xDF, 0xFF, 0xFD, 0x78, 0xF1, 0x80, 0xC7, 0xCF, 0xDF, 0xFE,
  0xFC, 0xF9, 0x80, 0x7D, 0xCF, 0x9F, 0x3E, 0x7C, 0xDF, 0x00, 0xFD, 0xCF, 0x9F, 0x3F, 0xDC, 0x38, 0x00, 0x7D, 0xCF, 0x9F, 0x3F, 0xFD, 0x9E, 0x80, 0xFD, 0xCF, 0x9F, 0x6F, 0x9D, 0xB9, 0x80, 0x79,
  0xDB, 0x83, 0xE0, 0x7C, 0xDF, 0x00, 0xFE, 0x70, 0xE1, 0xC3, 0x87, 0x0E, 0x00, 0xE7, 0xCF, 0x9F, 0x3E, 0x7C, 0xDF, 0x00, 0xE7, 0xCF, 0x9F, 0x36, 0xC7, 0x04, 0x00, 0xC7, 0x8F, 0x5E, 0xBF, 0xFD,
  0xF1, 0x80, 0xC7, 0xDD, 0xF1, 0xC7, 0xDD, 0xF1, 0x80, 0xE7, 0xCF, 0x9B, 0xE3, 0x87, 0x0E, 0x00, 0xFE, 0x1C, 0x71, 0xC7, 0x1C, 0x3F, 0x80, 0xFC, 0xCC, 0xCC, 0xF0, 0xC1, 0xC1, 0xC1, 0xC1, 0xC1,
  0xC1, 0x80, 0xF3, 0x33, 0x33, 0xF0, 0x76, 0xC0, 0xFE, 0x90, 0x38, 0xFB, 0x9F, 0x3F, 0xFC, 0xF9, 0x80, 0xFD, 0xCF, 0x9F, 0xEE, 0x7C, 0xFF, 0x00, 0x3C, 0xCF, 0x87, 0x0E, 0x0C, 0xCF, 0x00, 0xF9,
  0xDB, 0x9F, 0x3E, 0x7D, 0xBE, 0x00, 0xFF, 0xC3, 0x87, 0xEE, 0x1C, 0x3F, 0x80, 0xFF, 0xC3, 0x87, 0xEE, 0x1C, 0x38, 0x00, 0x3C, 0xC3, 0x87, 0x7E, 0x6C, 0xCF, 0x80, 0xE7, 0xCF, 0x9F, 0xFE, 0x7C,
  0xF9, 0x80, 0xFB, 0x9C, 0xE7, 0x3B, 0xE0, 0x1E, 0x0C, 0x18, 0x3E, 0x7C, 0xDF, 0x00, 0xE7, 0xDB, 0xE7, 0x8F, 0x9D, 0xB9, 0x80, 0xE3, 0x8E, 0x38, 0xE3, 0x8F, 0xC0, 0xC7, 0xDF, 0xFF, 0xFD, 0x78,
  0xF1, 0x80, 0xC7, 0xCF, 0xDF, 0xFE, 0xFC, 0xF9, 0x80, 0x7D, 0xCF, 0x9F, 0x3E, 0x7C, 0xDF, 0x00, 0xFD, 0xCF, 0x9F, 0x3F, 0xDC, 0x38, 0x00, 0x7D, 0xCF, 0x9F, 0x3F, 0xFD, 0x9E, 0x80, 0xFD, 0xCF,
  0x9F, 0x6F, 0x9D, 0xB9, 0x80, 0x79, 0xDB, 0x83, 0xE0, 0x7C, 0xDF, 0x00, 0xFE, 0x70, 0xE1, 0xC3, 0x87, 0x0E, 0x00, 0xE7, 0xCF, 0x9F, 0x3E, 0x7C, 0xDF, 0x00, 0xE7, 0xCF, 0x9F, 0x36, 0xC7, 0x04,
  0x00, 0xC7, 0x8F, 0x5E, 0xBF, 0xFD, 0xF1, 0x80, 0xC7, 0xDD, 0xF1, 0xC7, 0xDD, 0xF1, 0x80, 0xE7, 0xCF, 0x9B, 0xE3, 0x87, 0x0E, 0x00, 0xFE, 0x1C, 0x71, 0xC7, 0x1C, 0x3F, 0x80, 0x36, 0x6C, 0x66,
  0x30, 0xFF, 0xFC, 0xC6, 0x63, 0x66, 0xC0, 0x71, 0x74, 0x70 };
const GFXglyph Super_Mario_Bros__24pt7bGlyphs[] PROGMEM = {
  {0,1,1,8,0,0},{1,3,7,8,2,-6},{4,5,3,8,1,-6},{6,7,7,8,0,-6},{13,7,7,8,0,-6},{20,7,7,8,0,-6},
  {27,7,7,8,0,-6},{34,2,3,8,2,-6},{35,4,7,8,2,-6},{39,4,7,8,1,-6},{43,7,5,8,0,-5},{48,6,5,8,1,-5},
  {52,3,3,8,1,-1},{54,6,2,8,1,-3},{56,2,2,8,2,-1},{57,7,7,8,0,-6},{64,7,7,8,0,-6},{71,5,7,8,1,-6},
  {76,7,7,8,0,-6},{83,7,7,8,0,-6},{90,7,7,8,0,-6},{97,7,7,8,0,-6},{104,7,7,8,0,-6},{111,7,7,8,0,-6},
  {118,7,7,8,0,-6},{125,7,7,8,0,-6},{132,2,5,8,2,-5},{134,3,6,8,1,-5},{137,5,7,8,1,-6},{142,5,4,8,1,-4},
  {145,5,7,8,1,-6},{150,7,7,8,0,-6},{157,7,7,8,0,-6},{164,7,7,8,0,-6},{171,7,7,8,0,-6},{178,7,7,8,0,-6},
  {185,7,7,8,0,-6},{192,7,7,8,0,-6},{199,7,7,8,0,-6},{206,7,7,8,0,-6},{213,7,7,8,0,-6},{220,5,7,8,1,-6},
  {225,7,7,8,0,-6},{232,7,7,8,0,-6},{239,6,7,8,1,-6},{245,7,7,8,0,-6},{252,7,7,8,0,-6},{259,7,7,8,0,-6},
  {266,7,7,8,0,-6},{273,7,7,8,0,-6},{280,7,7,8,0,-6},{287,7,7,8,0,-6},{294,7,7,8,0,-6},{301,7,7,8,0,-6},
  {308,7,7,8,0,-6},{315,7,7,8,0,-6},{322,7,7,8,0,-6},{329,7,7,8,0,-6},{336,7,7,8,0,-6},{343,4,7,8,2,-6},
  {347,7,7,8,0,-6},{354,4,7,8,1,-6},{358,5,2,8,1,-6},{360,7,1,8,0,1},{361,2,2,8,2,-6},{362,7,7,8,0,-6},
  {369,7,7,8,0,-6},{376,7,7,8,0,-6},{383,7,7,8,0,-6},{390,7,7,8,0,-6},{397,7,7,8,0,-6},{404,7,7,8,0,-6},
  {411,7,7,8,0,-6},{418,5,7,8,1,-6},{423,7,7,8,0,-6},{430,7,7,8,0,-6},{437,6,7,8,1,-6},{443,7,7,8,0,-6},
  {450,7,7,8,0,-6},{457,7,7,8,0,-6},{464,7,7,8,0,-6},{471,7,7,8,0,-6},{478,7,7,8,0,-6},{485,7,7,8,0,-6},
  {492,7,7,8,0,-6},{499,7,7,8,0,-6},{506,7,7,8,0,-6},{513,7,7,8,0,-6},{520,7,7,8,0,-6},{527,7,7,8,0,-6},
  {534,7,7,8,0,-6},{541,4,7,8,2,-6},{545,2,7,8,3,-6},{547,4,7,8,1,-6},{551,7,3,8,0,-4} };
const GFXfont Super_Mario_Bros__24pt7b PROGMEM = { (uint8_t*)Super_Mario_Bros__24pt7bBitmaps, (GFXglyph*)Super_Mario_Bros__24pt7bGlyphs, 0x20, 0x7E, 9 };

const uint16_t BLOCK[361] PROGMEM = {
  0x000E, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40, 0x9A40,
  0x9A40, 0x9A40, 0x000E, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0x0000,
  0x0000, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0x0000,
  0x9A40, 0xE4E4, 0x0000, 0x0000, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000,
  0x0000, 0xE4E4, 0x0000, 0x9A40, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4,
  0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0xE4E4, 0x0000, 0x000E, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};
const unsigned short BUSH[189] PROGMEM = {
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0x0000,
  0x000E,0x0000,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0000,0xBFE3,0x0000,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0000,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0x0560,0xBFE3,0xBFE3,0x0000,0x000E,0x0000,0xBFE3,
  0xBFE3,0xBFE3,0x0560,0xBFE3,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0xBFE3,0x0560,0x0560,0xBFE3,0xBFE3,0x0560,
  0xBFE3,0xBFE3,0x0000,0xBFE3,0x0560,0x0560,0xBFE3,0xBFE3,0x0560,0x000E,0x000E,0x000E,0x0000,0x0000,0xBFE3,0x0560,
  0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x0560,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x000E,0x000E,
  0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
  0xBFE3,0xBFE3,0xBFE3,0x000E,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
  0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0x000E,0x0000,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,
  0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3,0xBFE3
};
const unsigned short GROUND[64] PROGMEM = {
  0xE2C2,0xF6B6,0xF6B6,0xF6B6,0x0000,0xE2C2,0xF6B6,0xE2C2,0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xF6B6,0xE2C2,0x0000,
  0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xE2C2,0x0000,0xE2C2,0x0000,0xE2C2,0xE2C2,0xE2C2,0x0000,0xF6B6,0xF6B6,0x0000,
  0xF6B6,0x0000,0x0000,0xE2C2,0x0000,0xF6B6,0xE2C2,0x0000,0xF6B6,0xF6B6,0xF6B6,0x0000,0xF6B6,0xE2C2,0xE2C2,0x0000,
  0xF6B6,0xE2C2,0xE2C2,0xF6B6,0xE2C2,0xE2C2,0xE2C2,0x0000,0xE2C2,0x0000,0x0000,0xF6B6,0x0000,0x0000,0x0000,0xE2C2
};
const unsigned short HILL[440] PROGMEM = {
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0000,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0000,0x0560,
  0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x0560,0x0560,0x0000,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x0000,0x0560,0x0000,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0000,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,
  0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,
  0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0000,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x000E,0x000E,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,
  0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,0x0000,0x0560,0x0000,0x0560,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,0x0000,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,0x000E,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0000,0x000E,0x000E,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0000,0x000E,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,
  0x0560,0x0560,0x0560,0x0560,0x0560,0x0560,0x0560
};
const uint16_t MARIO_IDLE[] PROGMEM = {
  _MASK, _MASK, _MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, M_RED, 
  M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, _MASK, _MASK, M_HAIR, M_HAIR, M_HAIR, M_SKIN, 
  M_SKIN, M_HAIR, M_SKIN, M_SKIN, _MASK, _MASK, _MASK, _MASK, M_HAIR, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, 
  M_SKIN, M_SKIN, M_SKIN, _MASK, _MASK, M_HAIR, M_SKIN, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, 
  M_SKIN, _MASK, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_HAIR, M_HAIR, M_HAIR, M_HAIR, _MASK, _MASK, _MASK, 
  _MASK, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, _MASK, _MASK, _MASK, _MASK, M_SHIRT, M_SHIRT, M_RED, 
  M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, _MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_SHIRT, M_SHIRT, M_RED, 
  M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, _MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_RED, M_RED, M_RED, M_SHIRT, M_SHIRT, M_SHIRT, 
  M_SHIRT, M_SHIRT, M_SKIN, M_SKIN, M_SHIRT, M_RED, M_SKIN, M_RED, M_RED, M_SKIN, M_RED, M_SHIRT, M_SKIN, M_SKIN, M_SKIN, M_SKIN, 
  M_SKIN, M_SKIN, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_RED, M_RED, 
  M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SKIN, M_SKIN, M_SKIN, _MASK, _MASK, M_RED, M_RED, M_RED, M_RED, _MASK, 
  M_RED, M_RED, M_RED, M_RED, _MASK, _MASK, _MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, _MASK, _MASK, _MASK, M_SHOES, M_SHOES, 
  M_SHOES, M_SHOES, _MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_SHOES, _MASK, _MASK, _MASK, M_SHOES, M_SHOES, M_SHOES, M_SHOES, M_SHOES
};
const uint16_t MARIO_JUMP[] PROGMEM = {
  _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, M_SKIN, M_SKIN, M_SKIN, 
  M_SKIN, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, M_SKIN, M_SKIN, 
  M_SKIN, M_SKIN, _MASK, _MASK, _MASK, _MASK, _MASK, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, 
  M_SKIN, M_SKIN, M_SKIN, _MASK, _MASK, _MASK, _MASK, _MASK, M_HAIR, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_HAIR, M_SKIN, M_SKIN, 
  M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, M_HAIR, M_SKIN, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_HAIR, M_SKIN, 
  M_SKIN, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, M_HAIR, M_SKIN, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, 
  M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, M_HAIR, M_HAIR, M_SKIN, M_SKIN, M_SKIN, M_SKIN, 
  M_HAIR, M_HAIR, M_HAIR, M_HAIR, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, M_SKIN, M_SKIN, M_SKIN, 
  M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, 
  M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_SHIRT, M_SHIRT, _MASK, _MASK, _MASK, _MASK, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, 
  M_SHIRT, M_RED, M_SHIRT, M_SHIRT, M_SHIRT, M_RED, M_RED, _MASK, M_SHOES, M_SHOES, M_SKIN, M_SKIN, M_SHIRT, M_SHIRT, M_SHIRT, M_SHIRT, 
  M_SHIRT, M_SHIRT, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, M_SHOES, M_SHOES, M_SKIN, M_SKIN, M_SKIN, M_SKIN, M_RED, 
  M_RED, M_SHIRT, M_RED, M_RED, M_SKIN, M_RED, M_RED, M_SKIN, M_RED, M_SHOES, M_SHOES, M_SHOES, _MASK, M_SKIN, M_SKIN, M_SHOES, 
  M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SHOES, M_SHOES, M_SHOES, _MASK, _MASK, M_SHOES, 
  M_SHOES, M_SHOES, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_SHOES, M_SHOES, M_SHOES, _MASK, M_SHOES, 
  M_SHOES, M_SHOES, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, 
  M_SHOES, M_SHOES, _MASK, M_RED, M_RED, M_RED, M_RED, M_RED, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK, _MASK
};

int        mLastMin = -1;
char       hStr[3]  = "12";
char       mStr[3]  = "00";
MarioState mState   = M_IDLE;
MarioDir   mDir     = M_UP;
int        mX       = 23, mY = 40, mStartY = 40;
MarioState bState   = M_IDLE;
MarioDir   bDir     = M_UP;
int        bhY = 8, bmY = 8, bStartY = 8;
float      cloud1X  = 0.0f, cloud2X = 40.0f;

void drawPuffyCloud(float x, float y, int type) {
  int ix = (int)x, iy = (int)y;
  uint16_t C_WHITE  = 0xFFFF;
  uint16_t C_SHADOW = 0x3DFF;
  if (type == 1) {
    display->fillCircle(ix+5,  iy+6, 4, C_SHADOW);
    display->fillCircle(ix+12, iy+4, 6, C_SHADOW);
    display->fillCircle(ix+19, iy+6, 4, C_SHADOW);
    display->fillRect(ix+5, iy+4, 15, 7, C_SHADOW);
    display->fillCircle(ix+4,  iy+5, 4, C_WHITE);
    display->fillCircle(ix+11, iy+3, 6, C_WHITE);
    display->fillCircle(ix+18, iy+5, 4, C_WHITE);
    display->fillRect(ix+4, iy+3, 15, 7, C_WHITE);
  } else {
    display->fillCircle(ix+4,  iy+5, 3, C_SHADOW);
    display->fillCircle(ix+9,  iy+3, 5, C_SHADOW);
    display->fillCircle(ix+14, iy+5, 3, C_SHADOW);
    display->fillRect(ix+4, iy+3, 11, 6, C_SHADOW);
    display->fillCircle(ix+3,  iy+4, 3, C_WHITE);
    display->fillCircle(ix+8,  iy+2, 5, C_WHITE);
    display->fillCircle(ix+13, iy+4, 3, C_WHITE);
    display->fillRect(ix+3, iy+2, 11, 6, C_WHITE);
  }
}

void drawBlock(int x, int y, char* text) {
  display->drawRGBBitmap(x, y, BLOCK, 19, 19);
  display->setFont(&Super_Mario_Bros__24pt7b);
  display->setTextColor(0x0000);
  int textX = (strlen(text) == 1) ? x+6 : x+2;
  display->setCursor(textX, y+12);
  display->print(text);
  display->setFont();
}

void renderMarioFrame() {
  if (getLocalTime(&timeinfo)) {
    if (timeinfo.tm_min != mLastMin) {
      mLastMin = timeinfo.tm_min;
      mState = M_JUMPING;
      mDir   = M_UP;
    }
  }

  cloud1X -= 0.20f; if (cloud1X <= -26.0f) cloud1X = 64.0f;
  cloud2X -= 0.12f; if (cloud2X <= -18.0f) cloud2X = 64.0f;

  if (mState == M_JUMPING) {
    mY += (mDir == M_UP ? -3 : 3);
    if (mDir == M_UP) {
      if (mY <= bStartY + 19) {
        mDir   = M_DOWN;
        bState = M_HIT;
        bDir   = M_UP;
        int hr = timeinfo.tm_hour;
        if (hr > 12) hr -= 12;
        if (hr == 0) hr = 12;
        sprintf(hStr, "%d",  hr);
        sprintf(mStr, "%02d", timeinfo.tm_min);
      } else if (mStartY - mY >= 14) {
        mDir = M_DOWN;
      }
    } else if (mDir == M_DOWN && mY >= mStartY) {
      mY     = mStartY;
      mState = M_IDLE;
    }
  }

  if (bState == M_HIT) {
    int step = (bDir == M_UP ? -2 : 2);
    bhY += step; bmY += step;
    if (bDir == M_UP && bStartY - bhY >= 4) {
      bDir = M_DOWN;
    } else if (bDir == M_DOWN && bhY >= bStartY) {
      bhY = bStartY; bmY = bStartY; bState = M_IDLE;
    }
  }

  display->fillScreen(SKY_COLOR);
  drawPuffyCloud(cloud1X, 21, 1);
  drawPuffyCloud(cloud2X,  7, 2);
  display->drawRGBBitmap(0,  34, HILL,  20, 22);
  display->drawRGBBitmap(43, 47, BUSH,  21,  9);
  for (int x = 0; x < 64; x += 8) display->drawRGBBitmap(x, 56, GROUND, 8, 8);

  drawBlock(13, bhY, hStr);
  drawBlock(32, bmY, mStr);

  if (mState == M_JUMPING) display->drawRGBBitmap(mX, mY, MARIO_JUMP, 17, 16);
  else display->drawRGBBitmap(mX, mY, MARIO_IDLE, 13, 16);

  display->flipDMABuffer();
}


// =====================================================================
// 4. TETRIS CLOCK ENGINE
// =====================================================================
TetrisMatrixDraw *tetris = nullptr;
String tetrisLastTime = "";
unsigned long tetrisLastAnimTime = 0;

const uint8_t tiny_font[39][5] = {
  {0x0,0x0,0x0,0x0,0x0},{0x6,0x9,0x9,0x9,0x6},{0x2,0x6,0x2,0x2,0x7},{0x6,0x9,0x2,0x4,0xF},{0x6,0x1,0x3,0x1,0x6},
  {0x9,0x9,0xF,0x1,0x1},{0xF,0x8,0xE,0x1,0xE},{0x6,0x8,0xE,0x9,0x6},{0xF,0x1,0x2,0x4,0x4},{0x6,0x9,0x6,0x9,0x6},
  {0x6,0x9,0x7,0x1,0x6},{0x6,0x9,0xF,0x9,0x9},{0xE,0x9,0xE,0x9,0xE},{0x7,0x8,0x8,0x8,0x7},{0xE,0x9,0x9,0x9,0xE},
  {0xF,0x8,0xE,0x8,0xF},{0xF,0x8,0xE,0x8,0x8},{0x7,0x8,0xB,0x9,0x7},{0x9,0x9,0xF,0x9,0x9},{0x7,0x2,0x2,0x2,0x7},
  {0x3,0x1,0x1,0x9,0x6},{0x9,0xA,0xC,0xA,0x9},{0x8,0x8,0x8,0x8,0xF},{0x9,0xF,0xF,0x9,0x9},{0x9,0xD,0xB,0x9,0x9},
  {0x6,0x9,0x9,0x9,0x6},{0xE,0x9,0xE,0x8,0x8},{0x6,0x9,0x9,0xA,0x5},{0xE,0x9,0xE,0x9,0x9},{0x7,0x8,0x6,0x1,0xE},
  {0xF,0x4,0x4,0x4,0x4},{0x9,0x9,0x9,0x9,0x6},{0x9,0x9,0x9,0x6,0x6},{0x9,0x9,0xF,0xF,0x9},{0x9,0x6,0x6,0x9,0x9},
  {0x9,0x9,0x6,0x4,0x4},{0xF,0x2,0x4,0x8,0xF},{0x0,0x6,0x0,0x6,0x0},{0x0,0x0,0x0,0x6,0x6}
};

int getTinyFontIndex(char c) {
  if (c >= '0' && c <= '9') return c - '0' + 1;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 11;
  if (c >= 'a' && c <= 'z') return c - 'a' + 11;
  if (c == ':') return 37;
  if (c == '.') return 38;
  return 0;
}

int drawCustomText(const char* text, int xo, int yo, uint16_t col) {
  int len = strlen(text);
  for (int i = 0; i < len; i++) {
    int idx = getTinyFontIndex(text[i]);
    for (int row = 0; row < 5; row++) {
      uint8_t bits = tiny_font[idx][row];
      for (int c = 0; c < 4; c++) {
        if (bits & (1 << (3 - c))) display->drawPixel(xo + (i * 5) + c, yo + row, col);
      }
    }
  }
  return xo + (len * 5); 
}

void drawDashedLine(int y, uint16_t color) {
  for (int x = 10; x < 54; x += 4) display->drawFastHLine(x, y, 2, color);
}

void renderTetrisFrame() {
  unsigned long now = millis();

  if (timeSynced && getLocalTime(&timeinfo)) {
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    if (String(timeStr) != tetrisLastTime) {
      tetrisLastTime = timeStr;
      tetris->setTime(timeStr, false);
    }
  }

  if (now - tetrisLastAnimTime > 50) {
    tetrisLastAnimTime = now;
    display->fillScreen(0);
    
    bool showCol = (now % 1000) < 500;
    tetris->drawNumbers(2, 32, showCol);
    drawDashedLine(35, display->color565(255, 255, 255));
    
    if (timeSynced) {
      char dd[3], mmm[4], yy[3], day[16];
      strftime(dd, sizeof(dd), "%d", &timeinfo);
      strftime(mmm, sizeof(mmm), "%b", &timeinfo);
      strftime(yy, sizeof(yy), "%y", &timeinfo);
      strftime(day, sizeof(day), "%A", &timeinfo);
      for(int i = 0; mmm[i]; i++) mmm[i] = toupper(mmm[i]);
      for(int i = 0; day[i]; i++) day[i] = toupper(day[i]);

      uint16_t c_cyan    = 0x07FF; 
      uint16_t c_yellow  = 0xFFE0; 
      uint16_t c_magenta = 0xF81F; 
      uint16_t c_green   = 0x07E0; 

      int xPos = 9; 
      int yPos1 = 39;
      xPos = drawCustomText(dd, xPos, yPos1, c_cyan);
      xPos = drawCustomText(" ", xPos, yPos1, 0); 
      xPos = drawCustomText(mmm, xPos, yPos1, c_yellow);
      xPos = drawCustomText(" ", xPos, yPos1, 0); 
      xPos = drawCustomText(yy, xPos, yPos1, c_magenta);

      drawDashedLine(47, display->color565(255, 255, 255));
      int dayX = (64 - (strlen(day) * 5)) / 2;
      drawCustomText(day, dayX, 51, c_green);
    }
    display->flipDMABuffer(); 
  }
}

// =====================================================================
// 5. WORD CLOCK ENGINE
// =====================================================================
const char* WORD_GRID[11] = {
  "ITLISASANIMS",
  "ACQUARTERABC",
  "TWENTYFIVEXW",
  "HALFSTENPTOO",
  "MINUTESQPAST",
  "ONERTWOTHREE",
  "FOURFIVESIXS",
  "SEVENEIGHTAN",
  "NINEATENEFGH",
  "ELEVENTWELVE",
  "O'CLOCKZAMPM"
};
const WordLoc w_it = {0,0,2}, w_is = {0,3,2}, w_am = {10,8,2}, w_pm = {10,10,2};
const WordLoc w_a = {1,0,1}, w_quarter = {1,2,7};
const WordLoc w_twenty = {2,0,6}, w_five_m = {2,6,4};
const WordLoc w_half = {3,0,4}, w_ten_m = {3,5,3}, w_to = {3,9,2};
const WordLoc w_minutes = {4,0,7}, w_past = {4,8,4};
const WordLoc w_one = {5,0,3}, w_two = {5,4,3}, w_three = {5,7,5};
const WordLoc w_four = {6,0,4}, w_five = {6,4,4}, w_six = {6,8,3};
const WordLoc w_seven = {7,0,5}, w_eight = {7,5,5};
const WordLoc w_nine = {8,0,4}, w_ten = {8,5,3};
const WordLoc w_eleven = {9,0,6}, w_twelve = {9,6,6};
const WordLoc w_oclock = {10,0,7};
bool activeGrid[11][12];
void setWord(WordLoc w) {
  for(int c = 0; c < w.len; c++) activeGrid[w.r][w.c + c] = true;
}
void drawTinyChar(char c, int xo, int yo, uint16_t col) {
  int idx = getTinyFontIndex(c);
  for (int row = 0; row < 5; row++) {
    uint8_t bits = tiny_font[idx][row];
    for (int col_i = 0; col_i < 4; col_i++) {
      if (bits & (1 << (3 - col_i))) {
        display->drawPixel(xo + col_i, yo + row, col);
      }
    }
  }
}
uint16_t getWordColor(int r) {
  if (r == 0) return display->color565(255, 255, 255);       // White (IT IS)
  if (r >= 1 && r <= 4) return display->color565(0, 255, 255); // Cyan (Minutes/Past/To)
  if (r >= 5 && r <= 9) return display->color565(255, 255, 0); // Yellow (Hours)
  if (r == 10) return display->color565(255, 0, 255);        // Magenta (O'CLOCK AM PM)
  return display->color565(255, 255, 255);
}
void renderWordFrame() {
  if (!timeSynced) return;
  getLocalTime(&timeinfo);
  memset(activeGrid, 0, sizeof(activeGrid));
  setWord(w_it); setWord(w_is);
  int m = timeinfo.tm_min;
  int h = timeinfo.tm_hour % 12;
  if (h == 0) h = 12;
  // Round to nearest 5 minutes
  int m_rounded = ((m + 2) / 5) * 5;
  if (m_rounded == 60) {
      m_rounded = 0;
      h = (h % 12) + 1;
  }
  if (m_rounded > 30) {
      h = (h % 12) + 1; // "TO" next hour
  }
  if (h == 0) h = 12;
  // Activate Time Words
  switch(m_rounded) {
    case 0:  setWord(w_oclock); break;
    case 5:  setWord(w_five_m); setWord(w_past); break;
    case 10: setWord(w_ten_m); setWord(w_past); break;
    case 15: setWord(w_a); setWord(w_quarter); setWord(w_past); break;
    case 20: setWord(w_twenty); setWord(w_past); break;
    case 25: setWord(w_twenty); setWord(w_five_m); setWord(w_past); break;
    case 30: setWord(w_half); setWord(w_past); break;
    case 35: setWord(w_twenty); setWord(w_five_m); setWord(w_to); break;
    case 40: setWord(w_twenty); setWord(w_to); break;
    case 45: setWord(w_a); setWord(w_quarter); setWord(w_to); break;
    case 50: setWord(w_ten_m); setWord(w_to); break;
    case 55: setWord(w_five_m); setWord(w_to); break;
  }
  // Activate Hour Words
  switch(h) {
    case 1: setWord(w_one); break;
    case 2: setWord(w_two); break;
    case 3: setWord(w_three); break;
    case 4: setWord(w_four); break;
    case 5: setWord(w_five); break;
    case 6: setWord(w_six); break;
    case 7: setWord(w_seven); break;
    case 8: setWord(w_eight); break;
    case 9: setWord(w_nine); break;
    case 10: setWord(w_ten); break;
    case 11: setWord(w_eleven); break;
    case 12: setWord(w_twelve); break;
  }
  // AM / PM
  if (timeinfo.tm_hour < 12) setWord(w_am);
  else setWord(w_pm);
  // Render Display
  display->fillScreen(0);
  uint16_t dimColor = display->color565(35, 35, 35); // Dark grey for inactive
  // Draw 12x11 Grid
  for (int r = 0; r < 11; r++) {
    int y = 2 + (r * 5); // 5px row height
    for (int c = 0; c < 12; c++) {
      int x = 2 + (c * 5); // 5px col width (4px char + 1px padding)
      uint16_t color = activeGrid[r][c] ? getWordColor(r) : dimColor;
      drawTinyChar(WORD_GRID[r][c], x, y, color);
    }
  }
  // Draw Bottom Seconds Row (0-9)
  int sec = timeinfo.tm_sec;
  int s_tens = sec / 10;
  int s_units = sec % 10;
  int startX = 7; // (64 - 50)/2 = 7
  int secY = 58;
  for (int i = 0; i <= 9; i++) {
    bool isTens = (s_tens == i);
    bool isUnits = (s_units == i);
    uint16_t color = dimColor;
    if (isTens && isUnits) color = display->color565(255, 255, 255); // Overlap: Bright White
    else if (isTens) color = display->color565(0, 255, 0);           // Tens: Green
    else if (isUnits) color = display->color565(255, 120, 0);        // Units: Orange
    drawTinyChar('0' + i, startX + (i * 5), secY, color);
  }
  display->flipDMABuffer();
}

// =====================================================================
// 6. LIVE NEWS TICKER ENGINE
// =====================================================================
NewsItem newsList[40]; // Stores all available top headlines
int newsCount = 0;
int currentNewsIdx = 0;
int newsScrollX = 64; 
unsigned long lastNewsFetch = 0;

void fetchNews() {
  if(WiFi.status() != WL_CONNECTED) return;
  
  bool success = false;
  
  // Keeps trying until the API successfully returns news
  while (!success) {
    display->fillScreen(0);
    display->setCursor(2, 28);
    display->setTextColor(display->color565(0, 255, 0));
    display->print("Fetching");
    display->setCursor(2, 38);
    display->print("Live News...");
    display->flipDMABuffer();

    HTTPClient http;
    http.begin("http://newsapi.org/v2/top-headlines?country=us&apiKey=3f61588636ea44aeab1b6ac904c7323f");
    int httpCode = http.GET();
    
    if(httpCode == 200) {
      StaticJsonDocument<256> filter;
      filter["articles"][0]["source"]["name"] = true;
      filter["articles"][0]["description"] = true;
      filter["articles"][0]["title"] = true;

      // Generous memory allocation for a large API response
      DynamicJsonDocument doc(16384);
      DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      if(!error) {
        JsonArray articles = doc["articles"];
        newsCount = 0;
        for(JsonObject article : articles) {
          if(newsCount >= 40) break; 
          
          String src = article["source"]["name"].as<String>();
          String desc = article["description"].as<String>();
          
          if (desc == "null" || desc.length() < 5) desc = article["title"].as<String>();
          
          if (desc != "null" && desc.length() > 0) {
            newsList[newsCount].source = src;
            
            // Text Sanitization
            desc.replace("\n", " ");
            desc.replace("\r", "");
            desc.replace("&#39;", "'");
            desc.replace("&quot;", "\"");
            desc.replace("&amp;", "&");
            desc.replace("’", "'"); 
            desc.replace("‘", "'"); 
            desc.replace("“", "\""); 
            desc.replace("”", "\""); 
            desc.replace("—", "-"); 
            
            newsList[newsCount].text = desc;
            newsCount++;
          }
        }
        
        // If we found valid articles, break the retry loop
        if (newsCount > 0) {
          success = true;
          lastNewsFetch = millis();
        }
      }
    }
    http.end();
    
    if (!success) {
      display->fillScreen(0);
      display->setCursor(2, 28);
      display->setTextColor(display->color565(255, 0, 0));
      display->print("API Error");
      display->setCursor(2, 38);
      display->print("Retrying...");
      display->flipDMABuffer();
      delay(5000); // Wait 5 seconds before hitting the API again
    }
  }
}

void renderNewsFrame() {
  display->fillScreen(0);
  
  if(timeSynced) getLocalTime(&timeinfo);

  // --- 1. Draw "NEWS" Blinker (Top Left) ---
  bool blinkState = (millis() / 500) % 2;
  if (blinkState) {
    display->fillRect(0, 2, 3, 3, display->color565(255, 0, 0));
    drawCustomText("NEWS", 5, 1, display->color565(255, 0, 0));
  }

  // --- 2. Draw Top Right Clock ---
  char tmStr[6];
  if (blinkState) {
    sprintf(tmStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  } else {
    sprintf(tmStr, "%02d %02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
  drawCustomText(tmStr, 64 - 25, 1, display->color565(200, 200, 200));

  if (newsCount > 0) {
    String src = newsList[currentNewsIdx].source;
    String txt = newsList[currentNewsIdx].text;
    
    char srcUpper[64];
    strncpy(srcUpper, src.c_str(), 63);
    srcUpper[63] = '\0';
    for(int i=0; srcUpper[i]; i++) srcUpper[i] = toupper(srcUpper[i]);
    
    // --- 3. Draw Source Name Centered (Dynamic 1 to 3 Lines) ---
    String s = String(srcUpper);
    int maxLen = 12; // 60px max width (12 chars * 5px)
    uint16_t srcColor = display->color565(255, 255, 0); // Yellow
    
    if (s.length() <= maxLen) { 
        int x = (64 - s.length() * 5) / 2;
        drawCustomText(s.c_str(), x, 20, srcColor); 
    } else {
        int split1 = s.lastIndexOf(' ', maxLen); 
        if(split1 <= 0) split1 = maxLen; 
        
        String l1 = s.substring(0, split1);
        String remainder = s.substring(split1);
        remainder.trim(); 
        
        if(remainder.length() <= maxLen) {
            int x1 = (64 - l1.length() * 5) / 2;
            int x2 = (64 - remainder.length() * 5) / 2;
            drawCustomText(l1.c_str(), x1, 16, srcColor);
            drawCustomText(remainder.c_str(), x2, 24, srcColor);
        } else {
            // Force 3 Lines
            int split2 = remainder.lastIndexOf(' ', maxLen);
            if(split2 <= 0) split2 = maxLen;
            
            String l2 = remainder.substring(0, split2);
            String l3 = remainder.substring(split2);
            l3.trim();
            if(l3.length() > maxLen) l3 = l3.substring(0, maxLen); // Final truncate
            
            int x1 = (64 - l1.length() * 5) / 2;
            int x2 = (64 - l2.length() * 5) / 2;
            int x3 = (64 - l3.length() * 5) / 2;
            drawCustomText(l1.c_str(), x1, 12, srcColor);
            drawCustomText(l2.c_str(), x2, 18, srcColor);
            drawCustomText(l3.c_str(), x3, 24, srcColor);
        }
    }

    // Borders for Ticker (Re-centered lower down)
    display->drawFastHLine(0, 36, 64, display->color565(50, 50, 50));
    display->drawFastHLine(0, 54, 64, display->color565(50, 50, 50));

    // --- 4. Draw Scrolling News Text ---
    display->setTextSize(1);
    display->setFont(); 
    display->setTextWrap(false); 
    display->setTextColor(display->color565(0, 255, 255)); // Cyan
    
    display->setCursor(newsScrollX, 42); // Placed safely between the borders
    display->print(txt);
    
    newsScrollX -= 1; // Exactly 1px per frame for butter-smooth scroll
    
    int textPixelLen = txt.length() * 6; 
    if (newsScrollX < -textPixelLen) {
      newsScrollX = 64;
      currentNewsIdx = (currentNewsIdx + 1) % newsCount;
    }

  } else {
    // Failsafe / Loading state inside frame
    display->setCursor(2, 42);
    display->setTextColor(display->color565(150, 150, 150));
    display->print("Waiting...");
  }

  display->flipDMABuffer();
}

// =====================================================================
//  TOUCH & MODE SWITCH
// =====================================================================
void switchToDigital() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); display->flipDMABuffer();
  lastSecDig = -1;
  initDigits();
  if (timeSynced) {
    getLocalTime(&timeinfo);
    int h=timeinfo.tm_hour,m=timeinfo.tm_min,s=timeinfo.tm_sec;
    int iv[6]={h/10,h%10,m/10,m%10,s/10,s%10};
    for (int i=0;i<6;i++) digits[i].val=iv[i];
    lastSecDig = s;
  }
}

void switchToAnalog() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); display->flipDMABuffer();
  lastSecAna = -1;  
}

void switchToMario() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); display->flipDMABuffer();
  mState  = M_IDLE;  mDir   = M_UP;
  bState  = M_IDLE;  bDir   = M_UP;
  mY      = mStartY; bhY    = bStartY; bmY = bStartY;
  cloud1X = 0.0f;    cloud2X = 40.0f;
  if (timeSynced) {
    getLocalTime(&timeinfo);
    int hr = timeinfo.tm_hour;
    if (hr > 12) hr -= 12;
    if (hr == 0) hr = 12;
    sprintf(hStr, "%d",  hr);
    sprintf(mStr, "%02d", timeinfo.tm_min);
    mLastMin = timeinfo.tm_min;
  }
}

void switchToTetris() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); display->flipDMABuffer();
  tetrisLastTime = "";
  if (timeSynced) {
    getLocalTime(&timeinfo);
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    tetrisLastTime = timeStr;
    tetris->setTime(timeStr, true); // True forces initial drop
  }
}

void switchToWord() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); display->flipDMABuffer();
}

void switchToNews() {
  display->fillScreen(0); display->flipDMABuffer();
  display->fillScreen(0); 
  display->setFont(); display->setTextSize(1);
  display->setTextColor(display->color565(0, 255, 0)); 
  display->setCursor(8,28); 
  display->print("Fetching");
  display->setCursor(8,38); 
  display->print("Live News..");
  display->flipDMABuffer();
  
  // Only fetch if we have 0 news items, OR if 1 hour (3600000ms) has passed
  if(newsCount == 0 || millis() - lastNewsFetch > 3600000UL) { 
     fetchNews();
  }
  newsScrollX = 64.0;
  currentNewsIdx = 0;
}

void checkTouch() {
  // THROTTLING FIX: Only poll the touch pin every 50ms.
  // Constantly polling touchRead() halts the processor and starves the Matrix DMA, causing flicker!
  if (millis() - lastTouchPoll < 50) return; 
  lastTouchPoll = millis();

  if (millis() - lastTouchTime < TOUCH_DEBOUNCE) return;
  
  if (touchRead(TOUCH_PIN) < TOUCH_THRESHOLD) {
    lastTouchTime = millis();
    ClockMode next = (ClockMode)((currentMode + 1) % 6);
    currentMode = next;
    
    if      (next == MODE_DIGITAL) { switchToDigital(); Serial.println("→ Digital"); }
    else if (next == MODE_ANALOG)  { switchToAnalog();  Serial.println("→ Analog");  }
    else if (next == MODE_MARIO)   { switchToMario();   Serial.println("→ Mario");   }
    else if (next == MODE_TETRIS)  { switchToTetris();  Serial.println("→ Tetris");  }
    else if (next == MODE_WORD)    { switchToWord();    Serial.println("→ Word");    }
    else                           { switchToNews();    Serial.println("→ News");    }
  }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);

  mxconfig.double_buff = true;
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(80);
  display->fillScreen(0);

  // Initialize Tetris Object
  tetris = new TetrisMatrixDraw(*display);
  tetris->scale = 2;

  CLR_BLACK = display->color565(  0,  0,  0);
  CLR_DIGIT = display->color565(  0,150,255);
  CLR_COLON = display->color565(255,255,  0);
  CLR_DATE  = display->color565(200,200,200);
  CLR_SYNC  = display->color565(  0,  0,180);

  WiFi.begin(ssid, pass);
  
  int frame = 0;
  uint16_t loaderColors[6] = {
    display->color565(255,   0,   0),  // Red
    display->color565(255, 128,   0),  // Orange
    display->color565(255, 255,   0),  // Yellow
    display->color565(  0, 255,   0),  // Green
    display->color565(  0, 255, 255),  // Cyan
    display->color565(255,   0, 255)   // Magenta
  };

  // Infinite loop until connected with a smooth colorful spinning animation
  while (WiFi.status() != WL_CONNECTED) { 
    display->fillScreen(0);
    display->setTextColor(display->color565(200, 200, 200)); 
    display->setTextSize(1);
    display->setCursor(2, 15); 
    display->print("Connecting");

    // Draw a spinning rainbow progress ring
    for (int i = 0; i < 6; i++) {
      float angle = (frame + i * 60) * PI / 180.0;
      int x = 32 + 10 * cos(angle);
      int y = 42 + 10 * sin(angle);
      
      // Make the leading dot slightly larger to emphasize rotation
      int dotSize = (i == 0) ? 2 : 1; 
      int colorIdx = (i + (frame / 10)) % 6; // Shift colors over time
      
      display->fillCircle(x, y, dotSize, loaderColors[colorIdx]);
    }

    display->flipDMABuffer();
    frame += 8; // Adjust to speed up or slow down the spin
    delay(30);  // Smooth ~30 FPS animation pace while yielding to WiFi task
  }

  // Once connected, sync time
  syncTime();
  lastNTPSync = millis();

  // Wait exactly 500ms without showing extra messages, then launch
  delay(500);

  // Start in Digital mode
  switchToDigital();
}

// =====================================================================
//  LOOP
// =====================================================================
void loop() {
  if (millis() - lastNTPSync > NTP_RESYNC_MS) {
    syncTime();
    lastNTPSync = millis();
  }

  checkTouch();

  // Consistent pacing delays introduced to allow DMA buffer to flush safely without tearing
  if (currentMode == MODE_DIGITAL) {
    updateDigitalState();
    renderDigitalFrame();
    delay(FRAME_MS); 
  } 
  else if (currentMode == MODE_ANALOG) {
    renderAnalogFrame();
    delay(25); // Increased from 8ms. 40FPS prevents DMA tearing and is smooth.
  } 
  else if (currentMode == MODE_MARIO) {
    renderMarioFrame();
    delay(30); 
  }
  else if (currentMode == MODE_TETRIS) {
    renderTetrisFrame();
    delay(10); // Tetris has its own internal 50ms check
  }
  else if (currentMode == MODE_WORD) {
    renderWordFrame();
    delay(30); // 33FPS for smooth seconds updating
  }
  else if (currentMode == MODE_NEWS) {
    renderNewsFrame();
    delay(25); // ~50fps for ultra-smooth scrolling text
  }
}
