/*
 * ============================================================
 * Smart Home Energy & Wellness System
 * WITH 6-DIGIT USER ID + 6-DIGIT PASSWORD LOGIN
 * ── TFT touch keypad  +  Web login form ──
 * ============================================================
 * Hardware:
 *   Arduino MKR WiFi 1010
 *   Adafruit 2.8" ILI9341 TFT (SPI) + FT6206 Capacitive Touch (I2C)
 *   SHT40 Temperature / Humidity (I2C)
 *   LDR on A0, PIR on D2, 2× buttons, 2× pots, 2× LEDs
 *
 * Login flow (BOTH screens use the same credentials):
 *   secrets.h → USER_ID      "123456"  (exactly 6 digits)
 *   secrets.h → USER_PASS    "654321"  (exactly 6 digits)
 *
 * TFT flow:
 *   Boot → splash → numeric keypad "Enter User ID" (6 digits)
 *        → numeric keypad "Enter Password" (shown as ******)
 *        → CORRECT → Dashboard
 *        → WRONG   → red "Wrong credentials!" + retry (max 3 attempts)
 *        → 3 fails → 30-second lockout, then retry
 *
 * Web flow:
 *   Browser GET / → HTML login form (UserID + Password fields)
 *   POST /login   → validate → set session cookie → redirect to /
 *   All other routes check session cookie; reject without it.
 *   3 failed POSTs → 30-second IP lockout.
 *
 * ============================================================
 * Required libraries (Library Manager):
 *   WiFiNINA, ArduinoJson (v6), Adafruit GFX, Adafruit ILI9341,
 *   Adafruit FT6206, SHT4x (Rob Tillaart)
 * ============================================================
 */

// ── secrets.h (create in sketch folder, add to .gitignore) ──
// #pragma once
// #define SECRET_SSID  "YourNetwork"
// #define SECRET_PASS  "YourWiFiPass"
// #define USER_ID      "123456"   // exactly 6 digits
// #define USER_PASS    "654321"   // exactly 6 digits
// ─────────────────────────────────────────────────────────────
#include "secrets.h"

#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_FT6206.h>
#include <SHT4x.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define TFT_CS          0
#define TFT_DC          1
#define LDR_PIN         A0
#define POT_TEMP_PIN    A1
#define PIR_PIN         2
#define BUTTON_MODE_PIN 3
#define BUTTON_ALARM_PIN 4
#define LED_RED_PIN     5
#define LED_GREEN_PIN   6

// ============================================================
// COLOURS (RGB565)
// ============================================================
#define C_BG       0x0000   // Black
#define C_TEXT     0xFFFF   // White
#define C_PRIMARY  0x04E0   // Blue-green
#define C_AMBER    0xFDA0   // Amber
#define C_RED      0xF800   // Red
#define C_GREEN    0x07E0   // Green
#define C_ORANGE   0xFC00   // Orange
#define C_TEAL     0x051D   // Teal
#define C_DGRAY    0x4208   // Dark grey  (key background)
#define C_LGRAY    0x8410   // Light grey (inactive button)
#define C_BLUE     0x001F   // Blue       (ENTER key)
#define C_DRED     0x8000   // Dark red   (DEL key)

// ============================================================
// HARDWARE OBJECTS
// ============================================================
Adafruit_ILI9341 tft   = Adafruit_ILI9341(&SPI, TFT_DC, TFT_CS, -1);
Adafruit_FT6206  touch = Adafruit_FT6206();
SHT4x            sht40;
WiFiServer       server(80);

// ============================================================
// LOGIN STATE – shared by TFT and web paths
// ============================================================
static constexpr uint8_t PIN_LEN       = 6;
static constexpr uint8_t MAX_ATTEMPTS  = 3;
static constexpr unsigned long LOCKOUT_MS = 30000UL;  // 30 s

// TFT login state
enum LoginStage { STAGE_USERID, STAGE_PASSWORD, STAGE_LOCKED, STAGE_OK };
LoginStage  tftStage        = STAGE_USERID;
char        tftInput[PIN_LEN + 1] = {0};  // current typed digits
uint8_t     tftInputLen     = 0;
char        tftUserEntered[PIN_LEN + 1] = {0};
uint8_t     tftFailCount    = 0;
unsigned long tftLockUntil  = 0;

// Web session
// Simple single-session token (16 hex chars).
// Generated on successful web login; cleared on logout.
static char sessionToken[17] = {0};
static bool sessionActive    = false;
static uint8_t  webFailCount = 0;
static unsigned long webLockUntil = 0;

// ============================================================
// SYSTEM STATE
// ============================================================
enum SystemMode { MODE_ECO, MODE_COMFORT, MODE_SLEEP };
SystemMode currentMode  = MODE_COMFORT;

float    temperature    = 22.0f;
float    humidity       = 50.0f;
int      lightLevel     = 300;
bool     motionDetected = false;
bool     alarmArmed     = false;
bool     heatingOn      = false;
float    targetTemp     = 21.0f;

// PID
float pidIntegral   = 0.0f;
float pidLastError  = 0.0f;
float pidDerivative = 0.0f;
static constexpr float PID_ALPHA = 0.3f;

// Energy
float dailyEnergyUsed  = 0.0f;
float dailyEnergySaved = 0.0f;
float dailyCost        = 0.0f;

// Timing
unsigned long lastMotionTime  = 0;
unsigned long lastTouchTime   = 0;
unsigned long dayStartMillis  = 0;
bool          wifiConnected   = false;
bool          alarmTriggered  = false;
unsigned long alarmBlinkLast  = 0;
bool          alarmBlinkState = false;

unsigned long lastSensorRead = 0;
unsigned long lastTftUpdate  = 0;
unsigned long lastPidUpdate  = 0;
unsigned long lastPotRead    = 0;
unsigned long lastEnergyUpd  = 0;

static constexpr unsigned long SENSOR_INTERVAL = 1000UL;
static constexpr unsigned long TFT_INTERVAL    =  500UL;
static constexpr unsigned long PID_INTERVAL    = 1000UL;
static constexpr unsigned long POT_INTERVAL    =  100UL;
static constexpr unsigned long ECO_TIMEOUT     = 300000UL;
static constexpr unsigned long DAY_MS          = 86400000UL;
static constexpr unsigned long ENERGY_INTERVAL = 60000UL;
static constexpr unsigned long TOUCH_DEBOUNCE  =  200UL;

// ============================================================
// TOUCH REGIONS – dashboard (only active when logged in)
// ============================================================
struct Rect { int x, y, w, h; };
const Rect btnEco      = {  20, 145,  80, 45 };
const Rect btnComfort  = { 120, 145,  80, 45 };
const Rect btnSleep    = { 220, 145,  80, 45 };
const Rect btnAlarm    = { 280,   5,  35, 30 };
const Rect btnTempUp   = { 270, 210,  45, 25 };
const Rect btnTempDown = {  10, 210,  45, 25 };

// ============================================================
// NUMERIC KEYPAD LAYOUT  (displayed on TFT during login)
//
//  [ 1 ] [ 2 ] [ 3 ]
//  [ 4 ] [ 5 ] [ 6 ]
//  [ 7 ] [ 8 ] [ 9 ]
//  [DEL] [ 0 ] [OK ]
//
// Screen = 320×240 (landscape, rotation=1).
// Layout budget:
//   Header   0–37   (38px)
//   Prompt  38–49   (12px)
//   Input   50–87   (38px)
//   Keypad  88–237  (150px available)
//
// KEY_H = 32, KEY_GAP = 4  → 4×32 + 3×4 = 140px  (fits with 10px spare)
// KEY_W = 98, KEY_GAP = 4  → 3×98 + 2×4 = 302px  (centred in 320px)
// KEY_X0 = (320 - 302) / 2 = 9
// KEY_Y0 = 90
// Bottom of last key = 90 + 4×32 + 3×4 = 90+128+12 = 230  ✓ inside screen
// ============================================================
static constexpr int KEY_W   = 98;
static constexpr int KEY_H   = 32;
static constexpr int KEY_X0  =  9;
static constexpr int KEY_Y0  = 90;
static constexpr int KEY_GAP =  4;

// Returns digit 0-9 for keys, 10=DEL, 11=ENTER, -1=none
static int keypadHit(int tx, int ty) {
  // Label map: row 0→{1,2,3}, row1→{4,5,6}, row2→{7,8,9}, row3→{DEL,0,OK}
  static const int8_t labels[4][3] = {
    {1, 2, 3},
    {4, 5, 6},
    {7, 8, 9},
    {10, 0, 11}   // 10=DEL, 11=ENTER
  };
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 3; col++) {
      int kx = KEY_X0 + col * (KEY_W + KEY_GAP);
      int ky = KEY_Y0 + row * (KEY_H + KEY_GAP);
      if (tx >= kx && tx < kx + KEY_W &&
          ty >= ky && ty < ky + KEY_H) {
        return labels[row][col];
      }
    }
  }
  return -1;
}

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
// Login / keypad
void drawLoginScreen(const char* prompt);
void drawKeypad();
void drawInputRow(bool masked);
void handleKeypadTouch(int key);
bool validateCredentials(const char* uid, const char* pwd);
void showLoginError(const char* msg);
void showLockoutScreen();
void generateSessionToken();

// Dashboard display
void drawDashboard();
void drawSensorPanel();
void drawEnergyPanel();
void drawModeButtons();
void drawAlarmButton();
void drawTempButtons();
void updateDashboard();
void updateSensorDisplay();
void updateEnergyDisplay();

// Sensors / control
void readSensors();
void checkTouch();
bool rectHit(int x, int y, const Rect& r);
void handleTouchMode(SystemMode m);
void handleTouchAlarm();
void handleTouchTempAdjust(float delta);
void checkButtons();
void handleModeButton();
void handleAlarmButton();
void readPotentiometers();
void updatePIDControl();
void updateStatusLEDs();
void checkEcoMode(unsigned long now);
void checkAlarmTrigger();
void updateEnergyTracking(unsigned long now);

// WiFi / web
void connectWiFi();
void handleWebClients();
void sendLoginPage(WiFiClient& client, const char* errMsg);
void sendDashboardPage(WiFiClient& client);
void sendJsonResponse(WiFiClient& client);
void send302(WiFiClient& client, const char* loc);
void send401Page(WiFiClient& client);
void sendLockoutPage(WiFiClient& client);
bool checkSessionCookie(const char* headers);
const char* getModeString();

// ============================================================
// SIMPLE TOKEN GENERATOR (uses millis + analog noise)
// ============================================================
void generateSessionToken() {
  // 16 hex characters from millis + ADC noise
  uint32_t a = (uint32_t)millis() ^ ((uint32_t)analogRead(A3) << 16);
  uint32_t b = (uint32_t)millis() ^ ((uint32_t)analogRead(A4) << 8);
  snprintf(sessionToken, sizeof(sessionToken), "%08lX%08lX",
           (unsigned long)a, (unsigned long)b);
  sessionActive = true;
}

// ============================================================
// CREDENTIAL VALIDATION
// ============================================================
bool validateCredentials(const char* uid, const char* pwd) {
  return (strncmp(uid, USER_ID,   PIN_LEN) == 0 &&
          strncmp(pwd, USER_PASS, PIN_LEN) == 0);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000);
  Serial.println(F("Smart Home – PIN login build"));

  tft.begin();
  tft.setRotation(1);

  // Splash
  tft.fillScreen(C_PRIMARY);
  tft.setTextColor(C_BG); tft.setTextSize(3);
  tft.setCursor(20, 55);  tft.println(F("Smart Home"));
  tft.setTextSize(2);
  tft.setCursor(20, 100); tft.println(F("Energy & Wellness"));
  tft.setCursor(20, 130); tft.println(F("Secured v2"));
  delay(2000);

  Wire.begin();

  if (!touch.begin(40)) Serial.println(F("FT6206 not found"));
  else                   Serial.println(F("FT6206 OK"));

  Wire.beginTransmission(0x44);
  if (Wire.endTransmission() == 0) {
    for (int i = 0; i < 3; i++) {
      if (sht40.begin()) { Serial.println(F("SHT40 OK")); break; }
      delay(100);
    }
  } else { Serial.println(F("SHT40 not found")); }

  pinMode(PIR_PIN,          INPUT);
  pinMode(BUTTON_MODE_PIN,  INPUT);
  pinMode(BUTTON_ALARM_PIN, INPUT);
  pinMode(LDR_PIN,          INPUT);
  pinMode(POT_TEMP_PIN,     INPUT);
  pinMode(LED_RED_PIN,      OUTPUT);
  pinMode(LED_GREEN_PIN,    OUTPUT);
  digitalWrite(LED_RED_PIN,   LOW);
  digitalWrite(LED_GREEN_PIN, LOW);

  lastMotionTime = millis();
  dayStartMillis = millis();

  connectWiFi();

  // Start with TFT login screen
  tftStage    = STAGE_USERID;
  tftInputLen = 0;
  memset(tftInput, 0, sizeof(tftInput));
  drawLoginScreen("Enter User ID");

  Serial.println(F("Ready – waiting for login."));
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // ── TFT LOCKED OUT ──────────────────────────────────────────
  if (tftStage == STAGE_LOCKED) {
    if (now >= tftLockUntil) {
      tftFailCount = 0;
      tftStage     = STAGE_USERID;
      tftInputLen  = 0;
      memset(tftInput, 0, sizeof(tftInput));
      drawLoginScreen("Enter User ID");
    } else {
      // Update countdown every second
      static unsigned long lastCountdown = 0;
      if (now - lastCountdown >= 1000UL) {
        showLockoutScreen();
        lastCountdown = now;
      }
    }
    if (wifiConnected) handleWebClients();
    return;
  }

  // ── TFT NOT YET LOGGED IN ────────────────────────────────────
  if (tftStage != STAGE_OK) {
    checkTouchLogin(now);
    if (wifiConnected) handleWebClients();
    return;
  }

  // ── LOGGED IN – normal operation ────────────────────────────
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    readSensors(); lastSensorRead = now;
  }
  checkTouch();
  checkButtons();
  if (now - lastPotRead >= POT_INTERVAL) {
    readPotentiometers(); lastPotRead = now;
  }
  if (now - lastPidUpdate >= PID_INTERVAL) {
    updatePIDControl(); lastPidUpdate = now;
  }
  updateStatusLEDs();
  checkEcoMode(now);
  checkAlarmTrigger();
  if (now - lastTftUpdate >= TFT_INTERVAL) {
    updateDashboard(); lastTftUpdate = now;
  }
  updateEnergyTracking(now);
  if (wifiConnected) handleWebClients();
}

// ============================================================
// TFT LOGIN – KEYPAD TOUCH CHECK
// Called from loop() while tftStage != STAGE_OK
// ============================================================
void checkTouchLogin(unsigned long now) {
  if (!touch.touched()) return;
  if (now - lastTouchTime < TOUCH_DEBOUNCE) return;

  TS_Point p = touch.getPoint();
  int tx = map(p.y, 0, 320, 320, 0);
  int ty = map(p.x, 0, 240,   0, 240);

  int key = keypadHit(tx, ty);
  if (key >= 0) handleKeypadTouch(key);

  lastTouchTime = now;
}

// ============================================================
// KEYPAD TOUCH HANDLER
// ============================================================
void handleKeypadTouch(int key) {
  if (key == 10) {
    // DEL
    if (tftInputLen > 0) {
      tftInputLen--;
      tftInput[tftInputLen] = '\0';
      drawInputRow(tftStage == STAGE_PASSWORD);
    }
    return;
  }

  if (key == 11) {
    // ENTER – only act when 6 digits entered
    if (tftInputLen < PIN_LEN) return;

    if (tftStage == STAGE_USERID) {
      // Save user ID, move to password stage
      strncpy(tftUserEntered, tftInput, PIN_LEN);
      tftUserEntered[PIN_LEN] = '\0';
      tftInputLen = 0;
      memset(tftInput, 0, sizeof(tftInput));
      tftStage = STAGE_PASSWORD;
      drawLoginScreen("Enter Password");
      return;
    }

    if (tftStage == STAGE_PASSWORD) {
      if (validateCredentials(tftUserEntered, tftInput)) {
        // SUCCESS
        tftFailCount = 0;
        tftStage     = STAGE_OK;
        tftInputLen  = 0;
        memset(tftInput, 0, sizeof(tftInput));
        // Generate web session too (single login unlocks both)
        generateSessionToken();
        Serial.println(F("TFT login OK"));
        drawDashboard();
      } else {
        // FAIL
        tftFailCount++;
        Serial.print(F("TFT login fail #")); Serial.println(tftFailCount);

        if (tftFailCount >= MAX_ATTEMPTS) {
          tftStage     = STAGE_LOCKED;
          tftLockUntil = millis() + LOCKOUT_MS;
          showLockoutScreen();
        } else {
          char msg[32];
          snprintf(msg, sizeof(msg), "Wrong! %d attempt(s) left",
                   MAX_ATTEMPTS - tftFailCount);
          showLoginError(msg);
          delay(2000);
          tftStage    = STAGE_USERID;
          tftInputLen = 0;
          memset(tftInput, 0, sizeof(tftInput));
          memset(tftUserEntered, 0, sizeof(tftUserEntered));
          drawLoginScreen("Enter User ID");
        }
      }
      return;
    }
    return;
  }

  // Digit key (0–9)
  if (tftInputLen < PIN_LEN) {
    tftInput[tftInputLen++] = '0' + key;
    tftInput[tftInputLen]   = '\0';
    drawInputRow(tftStage == STAGE_PASSWORD);
  }
}

// ============================================================
// DRAW LOGIN SCREEN
// ============================================================
void drawLoginScreen(const char* prompt) {
  tft.fillScreen(C_BG);

  // Header bar
  tft.fillRect(0, 0, 320, 38, C_PRIMARY);
  tft.setTextColor(C_BG); tft.setTextSize(2);
  tft.setCursor(10, 12);
  tft.println(F("Smart Home Login"));

  // Prompt
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(10, 50);
  tft.println(prompt);

  // Input row placeholder
  drawInputRow(tftStage == STAGE_PASSWORD);

  // Keypad
  drawKeypad();
}

// ============================================================
// DRAW INPUT ROW  (6 boxes, filled or masked)
// ============================================================
void drawInputRow(bool masked) {
  // Clear input area (between prompt text and keypad)
  tft.fillRect(0, 62, 320, 26, C_BG);

  // 6 digit boxes, centred horizontally
  // boxW=34, boxH=18, gap=5 → total=(6×34)+(5×5)=204+25=229 → startX=(320-229)/2=45
  const int boxW = 34, boxH = 18, boxGap = 5;
  const int totalW = PIN_LEN * boxW + (PIN_LEN - 1) * boxGap;
  int startX = (320 - totalW) / 2;
  int y = 65;

  for (int i = 0; i < PIN_LEN; i++) {
    int bx = startX + i * (boxW + boxGap);
    tft.drawRect(bx, y, boxW, boxH, C_PRIMARY);
    if (i < tftInputLen) {
      tft.setTextColor(C_TEXT); tft.setTextSize(1);
      // Centre the character inside the box (6×8 at size 1)
      tft.setCursor(bx + (boxW - 6) / 2, y + (boxH - 8) / 2);
      tft.print(masked ? '*' : tftInput[i]);
    }
  }
}

// ============================================================
// DRAW NUMERIC KEYPAD
// ============================================================
void drawKeypad() {
  static const char* labels[4][3] = {
    {"1","2","3"},
    {"4","5","6"},
    {"7","8","9"},
    {"DEL","0","OK"}
  };
  static const uint16_t keyColors[4][3] = {
    {C_DGRAY, C_DGRAY, C_DGRAY},
    {C_DGRAY, C_DGRAY, C_DGRAY},
    {C_DGRAY, C_DGRAY, C_DGRAY},
    {C_DRED,  C_DGRAY, C_BLUE }
  };

  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 3; col++) {
      int kx = KEY_X0 + col * (KEY_W + KEY_GAP);
      int ky = KEY_Y0 + row * (KEY_H + KEY_GAP);
      tft.fillRoundRect(kx, ky, KEY_W, KEY_H, 4, keyColors[row][col]);
      tft.drawRoundRect(kx, ky, KEY_W, KEY_H, 4, C_LGRAY);

      const char* lbl = labels[row][col];
      tft.setTextColor(C_TEXT);

      // Single-char digit keys → size 2 (12×16px per char)
      // Multi-char labels DEL/OK → size 1 (6×8px per char) to fit width
      bool isMultiChar = (strlen(lbl) > 1 && row == 3);  // DEL or OK
      if (isMultiChar) {
        tft.setTextSize(1);
        int cx = kx + (KEY_W - (int)strlen(lbl) * 6) / 2;
        int cy = ky + (KEY_H - 8) / 2;
        tft.setCursor(cx, cy);
      } else {
        tft.setTextSize(2);
        int cx = kx + (KEY_W - 12) / 2;   // one char = 12px wide at size 2
        int cy = ky + (KEY_H - 16) / 2;   // char height = 16px at size 2
        tft.setCursor(cx, cy);
      }
      tft.print(lbl);
    }
  }
}

// ============================================================
// LOGIN ERROR MESSAGE
// ============================================================
void showLoginError(const char* msg) {
  tft.fillRect(0, 40, 320, 45, C_BG);
  tft.fillRoundRect(5, 42, 310, 40, 6, C_RED);
  tft.setTextColor(C_TEXT); tft.setTextSize(1);
  int cx = (320 - strlen(msg) * 6) / 2;
  tft.setCursor(cx > 0 ? cx : 5, 57);
  tft.print(msg);
}

// ============================================================
// LOCKOUT SCREEN
// ============================================================
void showLockoutScreen() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 38, C_RED);
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(10, 12); tft.println(F("ACCESS LOCKED"));

  tft.setTextColor(C_AMBER); tft.setTextSize(2);
  tft.setCursor(20, 60);
  tft.println(F("Too many failed"));
  tft.setCursor(20, 85);
  tft.println(F("attempts."));

  unsigned long remaining = (tftLockUntil > millis())
                            ? (tftLockUntil - millis()) / 1000 + 1 : 0;
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(20, 120);
  tft.print(F("Retry in "));
  tft.print(remaining);
  tft.println(F(" s   "));
}

// ============================================================
// WIFI
// ============================================================
void connectWiFi() {
  tft.fillScreen(C_BG);
  tft.setTextColor(C_PRIMARY); tft.setTextSize(2);
  tft.setCursor(20, 60); tft.println(F("Connecting WiFi..."));

  int status = WL_IDLE_STATUS, attempts = 0;
  while (status != WL_CONNECTED && attempts < 10) {
    status = WiFi.begin(SECRET_SSID, SECRET_PASS);
    attempts++;
    tft.setCursor(20, 90); tft.print(F("Attempt ")); tft.println(attempts);
    delay(3000);
  }

  if (status == WL_CONNECTED) {
    wifiConnected = true;
    server.begin();
    IPAddress ip = WiFi.localIP();
    Serial.print(F("IP: ")); Serial.println(ip);

    tft.fillScreen(C_BG);
    tft.setTextColor(C_GREEN); tft.setTextSize(2);
    tft.setCursor(20, 60); tft.println(F("WiFi Connected!"));
    tft.setTextColor(C_PRIMARY);
    tft.setCursor(20, 90); tft.print(ip);
    tft.setTextColor(C_TEXT); tft.setTextSize(1);
    tft.setCursor(20, 120); tft.println(F("Login required in browser too"));
    delay(3000);
  } else {
    wifiConnected = false;
    Serial.println(F("WiFi failed"));
    tft.fillScreen(C_BG);
    tft.setTextColor(C_RED); tft.setTextSize(2);
    tft.setCursor(20, 80); tft.println(F("WiFi Failed!"));
    tft.setTextColor(C_TEXT);
    tft.setCursor(20, 110); tft.println(F("Standalone mode"));
    delay(2000);
  }
}

// ============================================================
// WEB SERVER – NON-BLOCKING
// All routes other than /login require a valid session cookie.
// ============================================================
void handleWebClients() {
  WiFiClient client = server.available();
  if (!client) return;

  // Short wait (300 ms max)
  unsigned long t0 = millis();
  while (!client.available() && (millis() - t0) < 300) delay(1);
  if (!client.available()) { client.stop(); return; }

  // ── Read request line ──────────────────────────────────────
  char reqBuf[128] = {0};
  int idx = 0;
  while (client.available() && idx < 126) {
    char c = client.read();
    if (c == '\n') break;
    reqBuf[idx++] = c;
  }
  reqBuf[idx] = '\0';

  // ── Read headers (capture Cookie + Content-Length) ─────────
  char cookieBuf[80]  = {0};
  char bodyBuf[80]    = {0};  // for POST body
  int  contentLength  = 0;

  while (client.available()) {
    char lineBuf[128] = {0};
    int li = 0;
    while (client.available() && li < 126) {
      char c = client.read();
      if (c == '\n') break;
      lineBuf[li++] = c;
    }
    // Trim \r
    if (li > 0 && lineBuf[li-1] == '\r') lineBuf[--li] = '\0';
    if (li == 0) break;  // blank line = end of headers

    if (strncmp(lineBuf, "Cookie: ", 8) == 0)
      strncpy(cookieBuf, lineBuf + 8, sizeof(cookieBuf) - 1);

    if (strncmp(lineBuf, "Content-Length: ", 16) == 0)
      contentLength = atoi(lineBuf + 16);
  }

  // ── Read POST body if present ──────────────────────────────
  if (contentLength > 0) {
    int toRead = contentLength < (int)sizeof(bodyBuf) - 1
                 ? contentLength : (int)sizeof(bodyBuf) - 1;
    int br = 0;
    t0 = millis();
    while (br < toRead && (millis() - t0) < 500) {
      if (client.available()) bodyBuf[br++] = client.read();
    }
    bodyBuf[br] = '\0';
  }

  Serial.print(F("Web req: ")); Serial.println(reqBuf);

  // ── Route: POST /login ─────────────────────────────────────
  if (strstr(reqBuf, "POST /login ")) {
    // Check IP lockout
    if (millis() < webLockUntil) {
      sendLockoutPage(client);
      client.stop(); return;
    }

    // Parse body: uid=XXXXXX&pwd=YYYYYY
    char uid[PIN_LEN + 1] = {0};
    char pwd[PIN_LEN + 1] = {0};

    char* p = strstr(bodyBuf, "uid=");
    if (p) strncpy(uid, p + 4, PIN_LEN);
    p = strstr(bodyBuf, "pwd=");
    if (p) strncpy(pwd, p + 4, PIN_LEN);

    if (validateCredentials(uid, pwd)) {
      webFailCount = 0;
      generateSessionToken();
      // If TFT is also logged in, keep it; otherwise unlock TFT too
      if (tftStage != STAGE_OK) {
        tftStage = STAGE_OK;
        drawDashboard();
      }
      Serial.println(F("Web login OK"));
      send302(client, "/");
    } else {
      webFailCount++;
      Serial.print(F("Web login fail #")); Serial.println(webFailCount);
      if (webFailCount >= MAX_ATTEMPTS) {
        webLockUntil = millis() + LOCKOUT_MS;
        webFailCount = 0;
        sendLockoutPage(client);
      } else {
        char errMsg[40];
        snprintf(errMsg, sizeof(errMsg), "Wrong credentials. %d attempt(s) left.",
                 MAX_ATTEMPTS - webFailCount);
        sendLoginPage(client, errMsg);
      }
    }
    client.stop(); return;
  }

  // ── Route: GET /logout ─────────────────────────────────────
  if (strstr(reqBuf, "GET /logout ")) {
    sessionActive = false;
    memset(sessionToken, 0, sizeof(sessionToken));
    // Send login page with expired cookie
    client.println(F("HTTP/1.1 302 Found"));
    client.println(F("Set-Cookie: sid=; Max-Age=0; Path=/"));
    client.println(F("Location: /"));
    client.println(F("Connection: close"));
    client.println();
    client.stop(); return;
  }

  // ── For all other routes – check session cookie ────────────
  bool authenticated = sessionActive && checkSessionCookie(cookieBuf);

  if (!authenticated) {
    sendLoginPage(client, "");
    client.stop(); return;
  }

  // ── Authenticated routes ───────────────────────────────────
  if      (strstr(reqBuf, "GET /api/data "))            sendJsonResponse(client);
  else if (strstr(reqBuf, "POST /api/mode/eco "))        { handleTouchMode(MODE_ECO);     sendJsonResponse(client); }
  else if (strstr(reqBuf, "POST /api/mode/comfort "))    { handleTouchMode(MODE_COMFORT); sendJsonResponse(client); }
  else if (strstr(reqBuf, "POST /api/mode/sleep "))      { handleTouchMode(MODE_SLEEP);   sendJsonResponse(client); }
  else if (strstr(reqBuf, "POST /api/alarm "))           { handleTouchAlarm();            sendJsonResponse(client); }
  else if (strstr(reqBuf, "POST /api/temp/up "))         { handleTouchTempAdjust( 0.5f); sendJsonResponse(client); }
  else if (strstr(reqBuf, "POST /api/temp/down "))       { handleTouchTempAdjust(-0.5f); sendJsonResponse(client); }
  else                                                    sendDashboardPage(client);

  delay(1);
  client.stop();
}

// ============================================================
// CHECK SESSION COOKIE
// Cookie header value looks like: "sid=XXXXXXXXXXXXXXXX"
// or may contain multiple cookies: "sid=XXX; other=YYY"
// ============================================================
bool checkSessionCookie(const char* cookieHeader) {
  if (!sessionActive) return false;
  char search[24];
  snprintf(search, sizeof(search), "sid=%s", sessionToken);
  return (strstr(cookieHeader, search) != nullptr);
}

// ============================================================
// 302 REDIRECT
// ============================================================
void send302(WiFiClient& client, const char* loc) {
  client.println(F("HTTP/1.1 302 Found"));
  // Set session cookie – HttpOnly prevents JS access
  char cookieLine[80];
  snprintf(cookieLine, sizeof(cookieLine),
           "Set-Cookie: sid=%s; Path=/; HttpOnly", sessionToken);
  client.println(cookieLine);
  client.print(F("Location: "));
  client.println(loc);
  client.println(F("Connection: close"));
  client.println();
}

// ============================================================
// LOGIN PAGE (web)
// ============================================================
void sendLoginPage(WiFiClient& client, const char* errMsg) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html; charset=utf-8"));
  client.println(F("Transfer-Encoding: chunked"));
  client.println(F("Connection: close"));
  client.println();

  #define CK(s) { const char* _s=(s); client.print(strlen(_s),HEX); client.print(F("\r\n")); client.print(_s); client.print(F("\r\n")); }

  CK("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>")
  CK("<meta name='viewport' content='width=device-width,initial-scale=1'>")
  CK("<title>Smart Home – Login</title><style>")
  CK("*{margin:0;padding:0;box-sizing:border-box}")
  CK("body{font-family:'Segoe UI',system-ui,sans-serif;background:#0a0e17;color:#e2e8f0;min-height:100vh;display:flex;align-items:center;justify-content:center}")
  CK(".card{background:#111827;border:1px solid #1e293b;border-radius:20px;padding:40px 36px;width:100%;max-width:380px;box-shadow:0 8px 32px rgba(0,0,0,.4)}")
  CK(".card::before{content:'';display:block;height:4px;background:linear-gradient(90deg,#38bdf8,#818cf8);border-radius:4px 4px 0 0;margin:-40px -36px 32px;border-radius:20px 20px 0 0}")
  CK("h1{font-size:1.4em;font-weight:700;background:linear-gradient(90deg,#38bdf8,#818cf8);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin-bottom:6px}")
  CK(".sub{font-size:.85em;color:#64748b;margin-bottom:28px}")
  CK("label{display:block;font-size:.78em;color:#94a3b8;text-transform:uppercase;letter-spacing:.08em;margin-bottom:6px;margin-top:18px}")
  CK("input{width:100%;padding:12px 14px;background:#0f172a;border:2px solid #1e293b;border-radius:10px;color:#e2e8f0;font-size:1.1em;letter-spacing:.15em;outline:none;transition:border .2s}")
  CK("input:focus{border-color:#38bdf8}")
  CK("input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none}")
  CK(".hint{font-size:.72em;color:#475569;margin-top:5px}")
  CK(".err{background:#7f1d1d;border:1px solid #dc2626;border-radius:8px;padding:10px 14px;font-size:.85em;color:#fca5a5;margin-top:18px;display:none}")
  CK(".err.show{display:block}")
  CK("button{width:100%;margin-top:28px;padding:14px;background:linear-gradient(135deg,#0ea5e9,#6366f1);border:none;border-radius:12px;color:#fff;font-size:1em;font-weight:700;cursor:pointer;letter-spacing:.05em;transition:opacity .2s}")
  CK("button:hover{opacity:.88}")
  CK("button:active{opacity:.7}")
  CK("</style></head><body><div class='card'>")
  CK("<h1>&#9889; Smart Home</h1>")
  CK("<p class='sub'>Enter your 6-digit credentials</p>")

  // Error message (empty string = hidden)
  {
    char errBlock[120];
    snprintf(errBlock, sizeof(errBlock),
             "<div class='err%s'>%s</div>",
             (errMsg && errMsg[0]) ? " show" : "",
             (errMsg && errMsg[0]) ? errMsg : "");
    client.print(strlen(errBlock), HEX);
    client.print(F("\r\n")); client.print(errBlock); client.print(F("\r\n"));
  }

  CK("<form method='POST' action='/login' autocomplete='off'>")
  CK("<label>User ID</label>")
  CK("<input type='password' name='uid' maxlength='6' pattern='[0-9]{6}' inputmode='numeric' placeholder='&#9679;&#9679;&#9679;&#9679;&#9679;&#9679;' required>")
  CK("<p class='hint'>6 digits</p>")
  CK("<label>Password</label>")
  CK("<input type='password' name='pwd' maxlength='6' pattern='[0-9]{6}' inputmode='numeric' placeholder='&#9679;&#9679;&#9679;&#9679;&#9679;&#9679;' required>")
  CK("<p class='hint'>6 digits</p>")
  CK("<button type='submit'>UNLOCK DASHBOARD</button>")
  CK("</form></div></body></html>")

  client.print(F("0\r\n\r\n"));
  #undef CK
}

// ============================================================
// LOCKOUT PAGE (web)
// ============================================================
void sendLockoutPage(WiFiClient& client) {
  unsigned long remaining = (webLockUntil > millis())
                            ? (webLockUntil - millis()) / 1000 + 1 : 0;
  client.println(F("HTTP/1.1 429 Too Many Requests"));
  client.println(F("Content-Type: text/html; charset=utf-8"));
  client.println(F("Transfer-Encoding: chunked"));
  client.println(F("Connection: close"));
  client.println();
  #define CK(s) { const char* _s=(s); client.print(strlen(_s),HEX); client.print(F("\r\n")); client.print(_s); client.print(F("\r\n")); }
  CK("<!DOCTYPE html><html><head><meta charset='UTF-8'>")
  CK("<meta name='viewport' content='width=device-width,initial-scale=1'>")
  CK("<meta http-equiv='refresh' content='5'>")
  CK("<title>Locked</title><style>body{font-family:system-ui;background:#0a0e17;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh}.box{background:#7f1d1d;border:2px solid #dc2626;border-radius:16px;padding:40px;text-align:center}h1{color:#fca5a5;font-size:1.6em}p{color:#fcd4d4;margin-top:12px}</style></head>")
  CK("<body><div class='box'><h1>&#128274; Access Locked</h1>")
  {
    char line[60];
    snprintf(line, sizeof(line), "<p>Too many failed attempts.</p><p>Retry in %lu seconds.</p>", remaining);
    client.print(strlen(line), HEX); client.print(F("\r\n")); client.print(line); client.print(F("\r\n"));
  }
  CK("</div></body></html>")
  client.print(F("0\r\n\r\n"));
  #undef CK
}

// ============================================================
// DASHBOARD PAGE (web) – only served when authenticated
// ============================================================
void sendDashboardPage(WiFiClient& client) {
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html; charset=utf-8"));
  client.println(F("Transfer-Encoding: chunked"));
  client.println(F("Connection: close"));
  client.println();

  #define CK(s) { const char* _s=(s); client.print(strlen(_s),HEX); client.print(F("\r\n")); client.print(_s); client.print(F("\r\n")); }

  CK("<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>")
  CK("<meta name='viewport' content='width=device-width,initial-scale=1'>")
  CK("<title>Smart Home Dashboard</title><style>")
  CK("*{margin:0;padding:0;box-sizing:border-box}")
  CK("body{font-family:'Segoe UI',system-ui,sans-serif;background:#0a0e17;color:#e2e8f0;min-height:100vh}")
  CK(".header{background:linear-gradient(135deg,#0f1729,#162040);padding:16px 24px;border-bottom:1px solid #1e3a5f;display:flex;justify-content:space-between;align-items:center}")
  CK(".header h1{font-size:1.3em;font-weight:600;background:linear-gradient(90deg,#38bdf8,#818cf8);-webkit-background-clip:text;-webkit-text-fill-color:transparent}")
  CK(".logout{font-size:.78em;color:#64748b;text-decoration:none;border:1px solid #1e293b;padding:5px 12px;border-radius:8px}")
  CK(".logout:hover{color:#e2e8f0;border-color:#475569}")
  CK(".dot{width:9px;height:9px;border-radius:50%;background:#22c55e;display:inline-block;animation:pulse 2s infinite;margin-right:6px}")
  CK("@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}")
  CK(".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:14px;padding:18px 22px;max-width:1100px;margin:0 auto}")
  CK(".card{background:#111827;border:1px solid #1e293b;border-radius:14px;padding:18px;position:relative;overflow:hidden}")
  CK(".card::before{content:'';position:absolute;top:0;left:0;right:0;height:3px}")
  CK(".card.sensor::before{background:linear-gradient(90deg,#38bdf8,#06b6d4)}")
  CK(".card.energy::before{background:linear-gradient(90deg,#f59e0b,#ef4444)}")
  CK(".card.control::before{background:linear-gradient(90deg,#818cf8,#a78bfa)}")
  CK(".card-title{font-size:.72em;text-transform:uppercase;letter-spacing:.1em;color:#64748b;margin-bottom:14px;font-weight:600}")
  CK(".metric{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:12px}")
  CK(".metric-label{font-size:.83em;color:#94a3b8}")
  CK(".metric-value{font-size:1.45em;font-weight:700;font-variant-numeric:tabular-nums}")
  CK(".u{font-size:.52em;color:#64748b;margin-left:2px}")
  CK(".c-blue{color:#38bdf8}.c-teal{color:#2dd4bf}.c-amber{color:#fbbf24}.c-green{color:#22c55e}.c-red{color:#ef4444}")
  CK(".mode-btns{display:flex;gap:7px;margin-bottom:14px}")
  CK(".mode-btn{flex:1;padding:11px 6px;border:2px solid #1e293b;border-radius:11px;background:#0f172a;color:#94a3b8;font-size:.82em;font-weight:600;cursor:pointer;text-align:center;transition:all .2s}")
  CK(".mode-btn.eco.active{background:#064e3b;border-color:#059669;color:#34d399}")
  CK(".mode-btn.comfort.active{background:#451a03;border-color:#d97706;color:#fbbf24}")
  CK(".mode-btn.sleep.active{background:#1e1b4b;border-color:#6366f1;color:#a5b4fc}")
  CK(".temp-ctrl{display:flex;align-items:center;justify-content:center;gap:14px;margin:10px 0}")
  CK(".temp-btn{width:42px;height:42px;border-radius:10px;border:2px solid #1e293b;background:#0f172a;color:#e2e8f0;font-size:1.3em;cursor:pointer;transition:all .15s}")
  CK(".temp-btn:active{transform:scale(.9)}")
  CK(".temp-display{font-size:2em;font-weight:700;min-width:90px;text-align:center;color:#38bdf8}")
  CK(".alarm-btn{width:100%;padding:13px;border-radius:11px;border:2px solid #1e293b;background:#0f172a;color:#94a3b8;font-size:.88em;font-weight:600;cursor:pointer;margin-top:8px;transition:all .2s}")
  CK(".alarm-btn.armed{background:#7f1d1d;border-color:#dc2626;color:#fca5a5;animation:pulse 2s infinite}")
  CK(".badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:.73em;font-weight:600}")
  CK(".badge-on{background:#14532d;color:#4ade80}.badge-off{background:#1e293b;color:#64748b}")
  CK(".footer{text-align:center;padding:10px;color:#475569;font-size:.72em;font-family:monospace}")
  CK("</style></head><body>")
  CK("<div class='header'><h1><span class='dot'></span>Smart Home</h1>")
  CK("<a href='/logout' class='logout'>Logout</a></div>")
  CK("<div class='grid'>")

  // Sensor card
  CK("<div class='card sensor'><div class='card-title'>Environment</div>")
  CK("<div class='metric'><span class='metric-label'>Temperature</span><span class='metric-value c-blue' id='temp'>--<span class='u'>&deg;C</span></span></div>")
  CK("<div class='metric'><span class='metric-label'>Humidity</span><span class='metric-value c-teal' id='hum'>--<span class='u'>%</span></span></div>")
  CK("<div class='metric'><span class='metric-label'>Light</span><span class='metric-value c-amber' id='light'>--<span class='u'>lx</span></span></div>")
  CK("<div class='metric'><span class='metric-label'>Motion</span><span id='motion' class='badge badge-off'>--</span></div></div>")

  // Energy card
  CK("<div class='card energy'><div class='card-title'>Energy</div>")
  CK("<div class='metric'><span class='metric-label'>Used Today</span><span class='metric-value c-red' id='used'>--<span class='u'>kWh</span></span></div>")
  CK("<div class='metric'><span class='metric-label'>Saved Today</span><span class='metric-value c-green' id='saved'>--<span class='u'>kWh</span></span></div>")
  CK("<div class='metric'><span class='metric-label'>Cost</span><span class='metric-value c-amber' id='cost'>--</span></div>")
  CK("<div class='metric'><span class='metric-label'>Heating</span><span id='heating' class='badge badge-off'>--</span></div></div>")

  // Controls card
  CK("<div class='card control'><div class='card-title'>Controls</div>")
  CK("<div class='mode-btns'>")
  CK("<div class='mode-btn eco'     id='btn-eco'     onclick=\"setMode('eco')\">ECO</div>")
  CK("<div class='mode-btn comfort' id='btn-comfort' onclick=\"setMode('comfort')\">COMFORT</div>")
  CK("<div class='mode-btn sleep'   id='btn-sleep'   onclick=\"setMode('sleep')\">SLEEP</div>")
  CK("</div>")
  CK("<div class='temp-ctrl'>")
  CK("<button class='temp-btn' onclick=\"adjTemp('down')\">&minus;</button>")
  CK("<div class='temp-display' id='target'>--&deg;</div>")
  CK("<button class='temp-btn' onclick=\"adjTemp('up')\">&plus;</button>")
  CK("</div>")
  CK("<button class='alarm-btn' id='alarm-btn' onclick='toggleAlarm()'>ALARM: OFF</button>")
  CK("</div></div>")  // end controls + grid

  // Footer
  {
    IPAddress ip = WiFi.localIP();
    char footer[60];
    snprintf(footer, sizeof(footer),
             "<div class='footer'>%d.%d.%d.%d &bull; auto-refresh 2s &bull; <a href='/logout' style='color:#475569'>logout</a></div>",
             ip[0], ip[1], ip[2], ip[3]);
    client.print(strlen(footer), HEX);
    client.print(F("\r\n")); client.print(footer); client.print(F("\r\n"));
  }

  // JavaScript – cookie is sent automatically by browser
  CK("<script>")
  CK("function poll(){fetch('/api/data').then(function(r){")
  CK("if(r.status===302||r.status===200&&r.url.indexOf('/login')>=0){location.href='/';return Promise.reject('auth')}")
  CK("return r.json()}).then(function(d){")
  CK("document.getElementById('temp').innerHTML=d.temp+'<span class=\"u\">&deg;C</span>';")
  CK("document.getElementById('hum').innerHTML=d.hum+'<span class=\"u\">%</span>';")
  CK("document.getElementById('light').innerHTML=d.light+'<span class=\"u\">lx</span>';")
  CK("var mo=document.getElementById('motion');mo.textContent=d.motion?'DETECTED':'CLEAR';mo.className='badge '+(d.motion?'badge-on':'badge-off');")
  CK("var ht=document.getElementById('heating');ht.textContent=d.heating?'ON':'OFF';ht.className='badge '+(d.heating?'badge-on':'badge-off');")
  CK("document.getElementById('used').innerHTML=d.used+'<span class=\"u\">kWh</span>';")
  CK("document.getElementById('saved').innerHTML=d.saved+'<span class=\"u\">kWh</span>';")
  CK("document.getElementById('cost').innerHTML='&pound;'+d.cost;")
  CK("document.getElementById('target').innerHTML=d.target+'&deg;';")
  CK("['eco','comfort','sleep'].forEach(function(x){var b=document.getElementById('btn-'+x);b.className='mode-btn '+x+(d.mode===x?' active':'');});")
  CK("var ab=document.getElementById('alarm-btn');ab.textContent='ALARM: '+(d.alarm?'ARMED':'OFF');ab.className='alarm-btn'+(d.alarm?' armed':'');")
  CK("}).catch(function(e){if(e!=='auth')console.warn(e)})}")
  CK("function setMode(m){fetch('/api/mode/'+m,{method:'POST'}).then(poll)}")
  CK("function adjTemp(d){fetch('/api/temp/'+d,{method:'POST'}).then(poll)}")
  CK("function toggleAlarm(){fetch('/api/alarm',{method:'POST'}).then(poll)}")
  CK("poll();setInterval(poll,2000);")
  CK("</script></body></html>")

  client.print(F("0\r\n\r\n"));
  #undef CK
}

// ============================================================
// JSON API
// ============================================================
void sendJsonResponse(WiFiClient& client) {
  StaticJsonDocument<256> doc;
  doc["temp"]    = serialized(String(temperature, 1));
  doc["hum"]     = serialized(String(humidity, 1));
  doc["light"]   = lightLevel;
  doc["motion"]  = motionDetected;
  doc["mode"]    = getModeString();
  doc["target"]  = serialized(String(targetTemp, 1));
  doc["heating"] = heatingOn;
  doc["alarm"]   = alarmArmed;
  doc["used"]    = serialized(String(dailyEnergyUsed, 2));
  doc["saved"]   = serialized(String(dailyEnergySaved, 2));
  doc["cost"]    = serialized(String(dailyCost, 2));

  String json;
  serializeJson(doc, json);

  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: application/json"));
  client.println(F("Connection: close"));
  client.print(F("Content-Length: ")); client.println(json.length());
  client.println();
  client.print(json);
}

// ============================================================
// HELPER
// ============================================================
const char* getModeString() {
  switch (currentMode) {
    case MODE_ECO:     return "eco";
    case MODE_COMFORT: return "comfort";
    case MODE_SLEEP:   return "sleep";
  }
  return "comfort";
}

// ============================================================
// TFT DASHBOARD
// ============================================================
void drawDashboard() {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 35, C_PRIMARY);
  tft.setTextColor(C_BG); tft.setTextSize(2);
  tft.setCursor(10, 12); tft.println(F("Energy Dashboard"));

  if (wifiConnected) {
    tft.setTextColor(C_TEAL); tft.setTextSize(1);
    tft.setCursor(10, 228);
    tft.print(F("http://"));
    tft.print(WiFi.localIP());
  }
  drawSensorPanel();
  drawEnergyPanel();
  drawModeButtons();
  drawAlarmButton();
  drawTempButtons();
}

void drawSensorPanel() {
  tft.drawRect(10, 50, 145, 80, C_TEAL);
  tft.drawRect(11, 51, 143, 78, C_TEAL);
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(15, 55); tft.println(F("SENSORS"));
}

void drawEnergyPanel() {
  tft.drawRect(165, 50, 145, 80, C_ORANGE);
  tft.drawRect(166, 51, 143, 78, C_ORANGE);
  tft.setTextColor(C_TEXT); tft.setTextSize(2);
  tft.setCursor(170, 55); tft.println(F("ENERGY"));
}

void drawModeButtons() {
  struct { const Rect* r; const char* lbl; uint16_t col; bool active; } btns[] = {
    { &btnEco,     "ECO ", C_PRIMARY, currentMode == MODE_ECO     },
    { &btnComfort, "COMF", C_AMBER,   currentMode == MODE_COMFORT },
    { &btnSleep,   "SLEP", C_TEAL,    currentMode == MODE_SLEEP   },
  };
  for (auto& b : btns) {
    tft.fillRoundRect(b.r->x, b.r->y, b.r->w, b.r->h, 5,
                      b.active ? b.col : (uint16_t)C_LGRAY);
    tft.setTextColor(C_BG); tft.setTextSize(2);
    tft.setCursor(b.r->x + 5, b.r->y + 15);
    tft.println(b.lbl);
  }
}

void drawAlarmButton() {
  tft.fillRoundRect(btnAlarm.x, btnAlarm.y, btnAlarm.w, btnAlarm.h, 3,
                    alarmArmed ? C_RED : (uint16_t)C_LGRAY);
  tft.setTextColor(C_BG); tft.setTextSize(2);
  tft.setCursor(btnAlarm.x + 8, btnAlarm.y + 8);
  tft.println(F("A"));
}

void drawTempButtons() {
  tft.fillRoundRect(btnTempUp.x,   btnTempUp.y,   btnTempUp.w,   btnTempUp.h,   3, C_ORANGE);
  tft.fillRoundRect(btnTempDown.x, btnTempDown.y, btnTempDown.w, btnTempDown.h, 3, C_TEAL);
  tft.setTextColor(C_BG); tft.setTextSize(2);
  tft.setCursor(btnTempUp.x   + 15, btnTempUp.y   + 5); tft.println(F("+"));
  tft.setCursor(btnTempDown.x + 18, btnTempDown.y + 5); tft.println(F("-"));
}

void updateDashboard() {
  tft.fillRect(230, 12, 85, 16, C_PRIMARY);
  tft.setTextColor(C_BG); tft.setTextSize(2);
  tft.setCursor(230, 12);
  tft.print(F("\xA3")); tft.print(dailyCost, 2);

  updateSensorDisplay();
  updateEnergyDisplay();

  static SystemMode lastMode  = MODE_COMFORT;
  static bool       lastAlarm = false;
  if (currentMode != lastMode)  { drawModeButtons(); lastMode  = currentMode; }
  if (alarmArmed  != lastAlarm) { drawAlarmButton(); lastAlarm = alarmArmed;  }
}

void updateSensorDisplay() {
  tft.fillRect(15, 70, 130, 50, C_BG);
  tft.setTextSize(2);

  tft.setTextColor(C_PRIMARY); tft.setCursor(20, 75);
  tft.print(temperature, 1); tft.setTextColor(C_TEXT); tft.print(F("C"));

  tft.setTextColor(C_TEAL); tft.setCursor(20, 95);
  tft.print((int)humidity); tft.setTextColor(C_TEXT); tft.print(F("%"));

  tft.setTextColor(C_ORANGE); tft.setCursor(20, 115);
  tft.print(lightLevel); tft.setTextColor(C_TEXT); tft.print(F("L"));
}

void updateEnergyDisplay() {
  tft.fillRect(170, 70, 130, 50, C_BG);
  tft.setTextSize(2);

  tft.setCursor(170, 75);
  tft.setTextColor(C_TEXT);   tft.print(F("Use:"));
  tft.setTextColor(C_ORANGE); tft.println(dailyEnergyUsed, 1);

  tft.setCursor(170, 95);
  tft.setTextColor(C_TEXT);    tft.print(F("Sav:"));
  tft.setTextColor(C_PRIMARY); tft.println(dailyEnergySaved, 1);

  tft.setCursor(170, 115);
  tft.setTextColor(C_ORANGE); tft.print(F("\xA3")); tft.print(dailyCost, 2);

  tft.fillRect(65, 210, 200, 15, C_BG);
  tft.setTextSize(2); tft.setTextColor(C_TEXT);
  tft.setCursor(70, 210); tft.print(targetTemp, 1); tft.print(F("C"));
  tft.setCursor(160, 210);
  if (heatingOn) { tft.setTextColor(C_ORANGE); tft.print(F("HEAT")); }
  else           { tft.setTextColor(C_TEXT);   tft.print(F("OFF ")); }
}

// ============================================================
// SENSORS
// ============================================================
void readSensors() {
  static unsigned long lastSHT40 = 0;
  if (millis() - lastSHT40 > 2000) {
    Wire.beginTransmission(0x44);
    if (Wire.endTransmission() == 0) {
      sht40.read();
      float t = sht40.getTemperature();
      float h = sht40.getHumidity();
      if (!isnan(t) && t >= -40.0f && t <= 125.0f) temperature = t;
      if (!isnan(h) && h >=   0.0f && h <= 100.0f) humidity    = h;
    }
    lastSHT40 = millis();
  }
  lightLevel = map(analogRead(LDR_PIN), 0, 1023, 0, 1000);
  bool prev  = motionDetected;
  motionDetected = digitalRead(PIR_PIN);
  if (motionDetected && !prev) {
    lastMotionTime = millis();
    Serial.println(F("Motion!"));
  }
}

// ============================================================
// TOUCH – DASHBOARD (only when tftStage == STAGE_OK)
// Rotation=1: raw p.y → screen X (reversed), raw p.x → screen Y
// ============================================================
void checkTouch() {
  if (!touch.touched()) return;
  unsigned long now = millis();
  if (now - lastTouchTime < TOUCH_DEBOUNCE) return;

  TS_Point p = touch.getPoint();
  int tx = map(p.y, 0, 320, 320, 0);
  int ty = map(p.x, 0, 240,   0, 240);

  if      (rectHit(tx, ty, btnEco))      handleTouchMode(MODE_ECO);
  else if (rectHit(tx, ty, btnComfort))  handleTouchMode(MODE_COMFORT);
  else if (rectHit(tx, ty, btnSleep))    handleTouchMode(MODE_SLEEP);
  else if (rectHit(tx, ty, btnAlarm))    handleTouchAlarm();
  else if (rectHit(tx, ty, btnTempUp))   handleTouchTempAdjust( 0.5f);
  else if (rectHit(tx, ty, btnTempDown)) handleTouchTempAdjust(-0.5f);

  lastTouchTime = now;
}

bool rectHit(int x, int y, const Rect& r) {
  return (x >= r.x && x <= r.x + r.w &&
          y >= r.y && y <= r.y + r.h);
}

void handleTouchMode(SystemMode m) {
  currentMode = m;
  if      (m == MODE_ECO)     targetTemp = 18.0f;
  else if (m == MODE_COMFORT) targetTemp = 21.0f;
  else                        targetTemp = 19.0f;
  drawModeButtons();
}

void handleTouchAlarm() {
  alarmArmed     = !alarmArmed;
  alarmTriggered = false;
  drawAlarmButton();
}

void handleTouchTempAdjust(float delta) {
  targetTemp = constrain(targetTemp + delta, 15.0f, 30.0f);
}

// ============================================================
// PHYSICAL BUTTONS
// ============================================================
void checkButtons() {
  static bool          lastMode  = LOW;
  static bool          lastAlarm = LOW;
  static unsigned long lastDB    = 0;

  bool modeBtn  = digitalRead(BUTTON_MODE_PIN);
  bool alarmBtn = digitalRead(BUTTON_ALARM_PIN);

  if (millis() - lastDB > 50UL) {
    if (modeBtn  == HIGH && lastMode  == LOW) { handleModeButton();  lastDB = millis(); }
    if (alarmBtn == HIGH && lastAlarm == LOW) { handleAlarmButton(); lastDB = millis(); }
  }
  lastMode  = modeBtn;
  lastAlarm = alarmBtn;
}

void handleModeButton() {
  if      (currentMode == MODE_ECO)     { currentMode = MODE_COMFORT; targetTemp = 21.0f; }
  else if (currentMode == MODE_COMFORT) { currentMode = MODE_SLEEP;   targetTemp = 19.0f; }
  else                                  { currentMode = MODE_ECO;     targetTemp = 18.0f; }
}

void handleAlarmButton() {
  alarmArmed = !alarmArmed; alarmTriggered = false;
}

// ============================================================
// POTENTIOMETERS
// ============================================================
void readPotentiometers() {
  float potTemp = map(analogRead(POT_TEMP_PIN), 0, 1023, 150, 300) / 10.0f;
  if (fabsf(potTemp - targetTemp) > 0.5f) targetTemp = potTemp;
}

// ============================================================
// PID
// ============================================================
void updatePIDControl() {
  static constexpr float Kp = 2.0f, Ki = 0.1f, Kd = 1.0f;
  float error    = targetTemp - temperature;
  pidIntegral   += error;
  pidIntegral    = constrain(pidIntegral, -50.0f, 50.0f);
  float rawD     = error - pidLastError;
  pidDerivative  = PID_ALPHA * rawD + (1.0f - PID_ALPHA) * pidDerivative;
  heatingOn      = (Kp * error + Ki * pidIntegral + Kd * pidDerivative) > 0.1f;
  pidLastError   = error;
}

// ============================================================
// STATUS LEDs
// ============================================================
void updateStatusLEDs() {
  digitalWrite(LED_RED_PIN, heatingOn ? HIGH : LOW);
  if (alarmArmed) {
    unsigned long now = millis();
    if (now - alarmBlinkLast >= 500UL) {
      alarmBlinkState = !alarmBlinkState;
      digitalWrite(LED_GREEN_PIN, alarmBlinkState ? HIGH : LOW);
      alarmBlinkLast = now;
    }
  } else {
    digitalWrite(LED_GREEN_PIN, HIGH);
    alarmBlinkState = false;
  }
}

// ============================================================
// ECO AUTO-SWITCH
// ============================================================
void checkEcoMode(unsigned long now) {
  if (currentMode == MODE_ECO) return;
  if (now - lastMotionTime > ECO_TIMEOUT) {
    currentMode = MODE_ECO; targetTemp = 18.0f;
    Serial.println(F("Auto ECO"));
  }
}

// ============================================================
// ALARM TRIGGER (non-blocking)
// ============================================================
void checkAlarmTrigger() {
  if (!alarmArmed || !motionDetected) return;
  static uint8_t       blinkCount = 0;
  static unsigned long lastBlink  = 0;
  if (!alarmTriggered) {
    alarmTriggered = true; blinkCount = 0;
    Serial.println(F("*** ALARM ***"));
  }
  if (blinkCount < 20) {
    unsigned long now = millis();
    if (now - lastBlink >= 100UL) {
      blinkCount++;
      digitalWrite(LED_RED_PIN, (blinkCount % 2 == 0) ? HIGH : LOW);
      lastBlink = now;
    }
  }
}

// ============================================================
// ENERGY TRACKING (daily reset)
// ============================================================
void updateEnergyTracking(unsigned long now) {
  if (now - dayStartMillis >= DAY_MS) {
    dailyEnergyUsed = dailyEnergySaved = dailyCost = 0.0f;
    dayStartMillis = now;
  }
  if (now - lastEnergyUpd >= ENERGY_INTERVAL) {
    if (heatingOn) dailyEnergyUsed  += 0.1f;
    else           dailyEnergySaved += 0.05f;
    dailyCost     = dailyEnergyUsed * 0.30f;
    lastEnergyUpd = now;
  }
}


