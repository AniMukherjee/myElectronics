#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <Fonts/Org_01.h> 
#include <WiFi.h>
#include <time.h>

// ==================== PIN DEFINITIONS ====================
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13
#define A_PIN  23
#define B_PIN  19
#define C_PIN   5
#define D_PIN  17
#define E_PIN  32
#define LAT_PIN 4
#define OE_PIN 15
#define CLK_PIN 16

HUB75_I2S_CFG::i2s_pins _pins = {
  R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
  A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
  LAT_PIN, OE_PIN, CLK_PIN
};

// ==================== CONFIGURATION ====================
HUB75_I2S_CFG mxconfig(64, 64, 1, _pins);
MatrixPanel_I2S_DMA *display = nullptr;

const char* wifiSsid = "AniDGreat";
const char* wifiPass = "Ani@1149";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;   
const int   daylightOffset_sec = 0;

const int centerX = 32;
const int centerY = 32;
const int clockRadius = 28;
unsigned long lastSecond = 0;

void connectWiFi() {
  WiFi.begin(wifiSsid, wifiPass);
  while (WiFi.status() != WL_CONNECTED) delay(500);
}

void syncTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void drawClockFace() {
  // Thick Rim
  display->drawCircle(centerX, centerY, clockRadius,     display->color565(255, 180, 0));
  display->drawCircle(centerX, centerY, clockRadius - 1, display->color565(200, 140, 0));
  display->drawCircle(centerX, centerY, clockRadius - 2, display->color565(150, 100, 0));

  display->setFont(&Org_01); 
  display->setTextColor(display->color565(255, 255, 255));

  display->setCursor(centerX - 3, 12);    display->print("12");
  display->setCursor(centerX + 20, 34);   display->print("3");
  display->setCursor(centerX - 1, 56);    display->print("6");
  display->setCursor(centerX - 24, 34);   display->print("9");

  display->setFont(); 

  for (int i = 0; i < 12; i++) {
    if (i % 3 != 0) { 
      float angle = (i * 30.0 - 90.0) * PI / 180.0;
      int mx = centerX + (clockRadius - 4) * cos(angle);
      int my = centerY + (clockRadius - 4) * sin(angle);
      display->fillCircle(mx, my, 1, display->color565(80, 80, 80));
    }
  }
}

void drawHand(float angle, int length, uint16_t color, int thickness) {
  float rad = (angle - 90.0) * PI / 180.0;
  int x = centerX + length * cos(rad);
  int y = centerY + length * sin(rad);

  if (thickness <= 1) {
    display->drawLine(centerX, centerY, x, y, color);
  } else {
    for (int t = -thickness / 2; t <= thickness / 2; t++) {
      display->drawLine(centerX + t, centerY, x + t, y, color);
      display->drawLine(centerX, centerY + t, x, y + t, color);
    }
  }
}

void drawDateInfo(struct tm &timeinfo) {
  const char* weekdays[7] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
  const char* months[12]  = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  char dayStr[3], yearStr[3];
  sprintf(dayStr, "%02d", timeinfo.tm_mday);
  sprintf(yearStr, "%02d", (timeinfo.tm_year + 1900) % 100);

  display->setTextSize(1);
  display->setTextColor(display->color565(100, 100, 100)); 

  display->setCursor(1, 1);           display->print(months[timeinfo.tm_mon]);    
  display->setCursor(64 - 12, 1);     display->print(dayStr);
  display->setCursor(1, 56);          display->print(weekdays[timeinfo.tm_wday]); 
  display->setCursor(64 - 12, 56);    display->print(yearStr);
}

void setup() {
  Serial.begin(115200);

  // 1. ENABLE DOUBLE BUFFERING IN CONFIG
  mxconfig.double_buff = true; 
  
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(255);

  connectWiFi();
  syncTime();
}

void loop() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  // Only update when the second changes to save processing
  if (timeinfo.tm_sec == lastSecond) {
    delay(10);
    return;
  }
  lastSecond = timeinfo.tm_sec;

  // 2. EVERYTHING BELOW DRAWS TO THE "BACK BUFFER"
  display->fillScreen(0); 
  drawClockFace();

  int hours   = timeinfo.tm_hour % 12;
  int minutes = timeinfo.tm_min;
  int seconds = timeinfo.tm_sec;

  float secondAngle = seconds * 6.0;
  float minuteAngle = minutes * 6.0 + seconds * 0.1;
  float hourAngle   = hours * 30.0 + minutes * 0.5;

  drawHand(hourAngle,   14, display->color565(0, 255, 220), 3);   
  drawHand(minuteAngle, 19, display->color565(255, 255, 255), 2); 
  drawHand(secondAngle, 22, display->color565(255, 0, 0), 1);     

  display->fillCircle(centerX, centerY, 3, display->color565(255, 200, 50));
  display->fillCircle(centerX, centerY, 1, 0);

  drawDateInfo(timeinfo);

  // 3. FLIP THE BUFFER (Instantly shows the completed drawing)
  display->flipDMABuffer(); 
}
