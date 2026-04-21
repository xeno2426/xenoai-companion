/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║       XenoAI Desk Companion — ESP32-S3 Firmware v3           ║
 * ║       Board: ESP32-S3-WROOM-1 N8R8                           ║
 * ║       Modes: AI Face · Clock · Date · Weather · Stopwatch     ║
 * ║       Built by Xeno                                          ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * HARDWARE:
 *   ESP32-S3-WROOM-1 N8R8 | OLED 1.3" SSD1306 (I2C, SDA=8, SCL=9)
 *   TTP223 capacitive touch sensor (GPIO4)
 *
 * LIBRARIES (Arduino Library Manager):
 *   Adafruit SSD1306 · Adafruit GFX Library · ArduinoJson
 *
 * ARDUINO IDE SETTINGS:
 *   Board: ESP32S3 Dev Module | Flash: 8MB | PSRAM: OPI PSRAM
 *   Upload Speed: 921600 | USB CDC: Enabled
 *
 * TOUCH CONTROLS:
 *   Tap        → Next mode  (Face → Clock → Date → Weather → Stopwatch → …)
 *   Long hold  → Mode action:
 *                  Face mode      : Trigger AI touch event
 *                  Weather mode   : Force weather refresh
 *                  Stopwatch mode : Start → Pause → Reset  (cycles each hold)
 *                  Clock / Date   : No action
 *
 * CHANGES FROM v2:
 *   - HC-SR04 / ultrasonic removed entirely (hardware absent)
 *   - Touch ISR changed RISING→CHANGE for tap vs long-press detection
 *   - Mode system added (5 modes)
 *   - NTP clock via configTime() — no extra library
 *   - Weather via weatherapi.com with ArduinoJson filtered parse
 *   - Stopwatch with accurate millis() accumulator
 *   - Tap in face mode now cycles to next mode;
 *     long-press in face mode triggers AI (preserves AI interaction)
 */

// ─── LIBRARIES ───────────────────────────────────────────────────────────────
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <time.h>
#define WHITE SH110X_WHITE
#define BLACK SH110X_BLACK

// ─── USER CONFIG — EDIT THESE ────────────────────────────────────────────────
#define WIFI_SSID        "Redmi 9i"
#define WIFI_PASS        "strawberry"
#define XENOAI_URL       "https://xenoai-companion.onrender.com"
#define OTA_PASSWORD     "xenoai123"

// weatherapi.com — free account at https://www.weatherapi.com/
#define WEATHER_API_KEY  "ca6c8ee702644b2cbe534732262004"
#define WEATHER_CITY     "Nagpur"   // city name OR "lat,lon" e.g. "28.6,77.2"

// Timezone: seconds east of UTC
//   IST  (India)    = 19800   (UTC+5:30)
//   EST  (US East)  = -18000  (UTC-5)
//   CET  (Europe)   = 3600    (UTC+1)
//   UTC              = 0
#define GMT_OFFSET_S     19800
#define DST_OFFSET_S     0
#define NTP_SERVER       "pool.ntp.org"

// ─── HARDWARE PINS ───────────────────────────────────────────────────────────
#define OLED_SDA   8
#define OLED_SCL   9
#define OLED_ADDR  0x3C
#define TOUCH_PIN  4
#define LED_PIN    2

// ─── OLED ────────────────────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H  64
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
// Initialize RoboEyes AFTER the display object is created
#include <FluxGarage_RoboEyes.h>
RoboEyes roboEyes(display);

// ─── TIMING CONSTANTS ────────────────────────────────────────────────────────
#define STATE_INTERVAL    6000UL    // Backend poll every 6 s
#define MESSAGE_DURATION  4000UL   // Show AI message for 4 s
#define DISPLAY_INTERVAL   200UL   // Non-face display refresh rate (ms)
#define WEATHER_INTERVAL 600000UL  // Re-fetch weather every 10 min
#define LONG_PRESS_MS      600UL   // Hold ≥ 600 ms → long press

// ─── APP MODES ───────────────────────────────────────────────────────────────
enum AppMode {
  MODE_FACE = 0,
  MODE_CLOCK,
  MODE_DATE,
  MODE_WEATHER,
  MODE_STOPWATCH,
  MODE_COUNT
};
AppMode currentMode = MODE_FACE;

// ─── STOPWATCH ───────────────────────────────────────────────────────────────
enum SwState { SW_STOPPED, SW_RUNNING, SW_PAUSED };
SwState       swState  = SW_STOPPED;
unsigned long swStart  = 0;   // millis() at last start / resume
unsigned long swAccum  = 0;   // ms accumulated before last pause

// ─── WEATHER ─────────────────────────────────────────────────────────────────
String        weatherTemp   = "--";
String        weatherCond   = "No data";
bool          weatherValid  = false;
unsigned long lastWeatherAt = 0;

// ─── AI FACE STATE ───────────────────────────────────────────────────────────
String        currentMood    = "neutral";
String        currentMessage = "";
bool          msgShowing     = false;
unsigned long msgShownAt     = 0;
unsigned long lastStateAt    = 0;

// ─── DISPLAY ─────────────────────────────────────────────────────────────────
unsigned long lastDisplayAt  = 0;
bool          needsRedraw    = true;

// ─── TOUCH ISR ───────────────────────────────────────────────────────────────
// CHANGE interrupt: RISING records press start, FALLING classifies tap / long.
volatile bool          tapFlag       = false;
volatile bool          longPressFlag = false;
volatile unsigned long touchDownAt   = 0;
volatile bool          touchActive   = false;
unsigned long          lastTouchAt   = 0;   // debounce reference (in loop)

void IRAM_ATTR onTouchChange() {
  if (digitalRead(TOUCH_PIN) == HIGH) {
    touchDownAt = millis();
    touchActive = true;
  } else if (touchActive) {
    unsigned long held = millis() - touchDownAt;
    touchActive = false;
    if      (held >= LONG_PRESS_MS) longPressFlag = true;
    else if (held >= 30)            tapFlag       = true;  // ≥30 ms = real touch
  }
}

// ─── OLED HELPERS (defined early — called from WiFi/OTA setup) ───────────────

void drawText(int x, int y, const String& text) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(x, y);
  display.print(text);
}

void drawTextCentered(int y, const String& text) {
  int x = max(0, (SCREEN_W - (int)text.length() * 6) / 2);
  drawText(x, y, text);
}

void drawTextWrapped(const String& text, int y) {
  const int CPL = 21;   // chars per line at textSize 1
  for (int i = 0; i < (int)text.length() && y <= 54; i += CPL) {
    drawTextCentered(y, text.substring(i, min((int)text.length(), i + CPL)));
    y += 10;
  }
}

// ─── WIFI ────────────────────────────────────────────────────────────────────
void connectWiFi() {
  display.clearDisplay();
  drawTextCentered(20, "Connecting WiFi");
  drawTextCentered(35, WIFI_SSID);
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    tries++;
  }

  display.clearDisplay();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: " + WiFi.localIP().toString());
    drawTextCentered(20, "WiFi Connected!");
    drawTextCentered(35, WiFi.localIP().toString());
  } else {
    Serial.println("WiFi: FAILED");
    drawTextCentered(25, "WiFi Failed :(");
  }
  display.display();
  delay(1200);
}

// ─── OTA ─────────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    display.clearDisplay();
    drawTextCentered(25, "OTA Updating...");
    display.display();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = progress / (total / 100);
    display.clearDisplay();
    drawTextCentered(15, "Updating");
    drawTextCentered(30, String(pct) + "%");
    display.drawRect(10, 45, 108, 8, WHITE);
    display.fillRect(10, 45, 108 * pct / 100, 8, WHITE);
    display.display();
  });
  ArduinoOTA.onEnd([]() {
    display.clearDisplay();
    drawTextCentered(25, "Done! Rebooting");
    display.display();
    delay(1000);
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

// ─── HTTP HELPERS ────────────────────────────────────────────────────────────
String httpPost(const String& endpoint, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(XENOAI_URL + endpoint);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return resp;
}

String httpGet(const String& endpoint) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(XENOAI_URL + endpoint);
  http.setTimeout(8000);
  int code = http.GET();
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return resp;
}

// ─── BACKEND JSON PARSER ─────────────────────────────────────────────────────
void parseResponse(const String& json) {
  if (json.isEmpty()) return;
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;

  if (doc.containsKey("mood")) {
    String newMood = doc["mood"].as<String>();
    if (newMood != currentMood) {
      currentMood = newMood;
      needsRedraw = true; 

      // Map the backend string to the visual RoboEyes library states
      if (currentMood == "happy") {
        roboEyes.setMood(HAPPY); 
      } else if (currentMood == "excited") {
        roboEyes.anim_laugh(); 
      } else if (currentMood == "surprised" || currentMood == "curious") {
        roboEyes.anim_confused(); 
      } else if (currentMood == "sleepy" || currentMood == "sad") {
        roboEyes.setMood(TIRED); 
      } else {
        roboEyes.setMood(DEFAULT); 
      }
    }
  }
  
  if (doc.containsKey("message")) {
    String msg = doc["message"].as<String>();
    if (msg.length() > 0) {
      currentMessage = msg;
      msgShowing     = true;
      msgShownAt     = millis();
    }
  }
}


// ─── BACKEND API CALLS ───────────────────────────────────────────────────────
void sendTouch() {
  Serial.println("[touch] -> /api/touch");
  String resp = httpPost("/api/touch", "{}");
  parseResponse(resp);
  Serial.println("[touch] " + resp.substring(0, 80));
}

void pollState() {
  String resp = httpGet("/api/state");
  if (!resp.isEmpty()) parseResponse(resp);
}

// ─── WEATHER ─────────────────────────────────────────────────────────────────
// Filtered parse keeps doc small; full weatherapi response is ~3 KB.
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url  = "http://api.weatherapi.com/v1/current.json?key=";
  url += WEATHER_API_KEY;
  url += "&q=";
  url += WEATHER_CITY;
  url += "&aqi=no";

  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();

  if (code == 200) {
    String json = http.getString();

    StaticJsonDocument<64> filter;
    filter["current"]["temp_c"]            = true;
    filter["current"]["condition"]["text"] = true;

    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, json, DeserializationOption::Filter(filter))) {
      float tc             = doc["current"]["temp_c"] | 0.0f;
      const char* condRaw  = doc["current"]["condition"]["text"] | "Unknown";

      weatherTemp  = String((int)roundf(tc)) + "C";
      weatherCond  = String(condRaw);
      if (weatherCond.length() > 21) weatherCond = weatherCond.substring(0, 21);
      weatherValid = true;

      Serial.println("[wx] " + weatherTemp + " | " + weatherCond);
    }
  } else {
    Serial.printf("[wx] HTTP %d\n", code);
  }
  http.end();
}

// ─── NTP HELPER ──────────────────────────────────────────────────────────────
// After setup() sync, this returns in <1 ms (reads cached ESP32 RTC).
// The 200 ms timeout only applies if NTP hasn't synced yet.
bool getTime(struct tm& t) {
  return getLocalTime(&t, 200);
}

// ─── FACE EXPRESSIONS ────────────────────────────────────────────────────────
// Face occupies roughly y=10..50, mouth y=42..50, eyes y=19..29.
// Faces are drawn after clearDisplay(); the caller calls display.display().

void drawFaceNeutral() {
  display.fillCircle(44, 24, 5, WHITE);  display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE);  display.fillCircle(84, 24, 2, BLACK);
  display.drawLine(49, 45, 79, 45, WHITE);
}

void drawFaceHappy() {
  // Squint eyes
  display.drawLine(38, 24, 44, 19, WHITE); display.drawLine(44, 19, 50, 24, WHITE);
  display.drawLine(78, 24, 84, 19, WHITE); display.drawLine(84, 19, 90, 24, WHITE);
  // U-shaped smile: center pixel lower than edges
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 50 - (i * i) / 30, WHITE);
}

void drawFaceExcited() {
  // Star eyes
  display.drawLine(38, 24, 50, 24, WHITE); display.drawLine(44, 18, 44, 30, WHITE);
  display.drawLine(39, 19, 49, 29, WHITE); display.drawLine(49, 19, 39, 29, WHITE);
  display.drawLine(78, 24, 90, 24, WHITE); display.drawLine(84, 18, 84, 30, WHITE);
  display.drawLine(79, 19, 89, 29, WHITE); display.drawLine(89, 19, 79, 29, WHITE);
  // Big double-line grin
  for (int i = -25; i <= 25; i++) {
    display.drawPixel(64 + i, 50 - (i * i) / 25, WHITE);
    display.drawPixel(64 + i, 51 - (i * i) / 25, WHITE);
  }
}

void drawFaceSurprised() {
  display.drawCircle(44, 24, 7, WHITE); display.fillCircle(44, 24, 3, WHITE);
  display.drawCircle(84, 24, 7, WHITE); display.fillCircle(84, 24, 3, WHITE);
  display.drawCircle(64, 46, 7, WHITE);   // O-mouth
}

void drawFaceSleepy() {
  // Half-closed eyes: fill circle then blank top half
  display.fillCircle(44, 26, 5, WHITE); display.fillRect(38, 21, 13, 5, BLACK);
  display.fillCircle(84, 26, 5, WHITE); display.fillRect(78, 21, 13, 5, BLACK);
  display.drawLine(52, 46, 76, 46, WHITE);
  display.setCursor(90, 10); display.print("z");
  display.setCursor(98,  4); display.print("z");
  display.setCursor(106, 0); display.print("z");
}

void drawFaceSad() {
  display.fillCircle(44, 26, 5, WHITE); display.fillCircle(44, 26, 2, BLACK);
  display.fillCircle(84, 26, 5, WHITE); display.fillCircle(84, 26, 2, BLACK);
  // Tears
  display.drawLine(48, 30, 50, 38, WHITE);
  display.drawLine(88, 30, 90, 38, WHITE);
  // Arch frown: center pixel higher than edges
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 42 + (i * i) / 30, WHITE);
}

void drawFaceCurious() {
  display.drawLine(37, 18, 51, 14, WHITE);  // raised left brow
  display.drawLine(77, 14, 91, 14, WHITE);  // flat right brow
  display.fillCircle(44, 24, 5, WHITE); display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE); display.fillCircle(84, 24, 2, BLACK);
  for (int i = -15; i <= 15; i++)
    display.drawPixel(64 + i, 48 - (i * i) / 35, WHITE);
  display.setCursor(110, 10); display.print("?");
}

// ─── MODE: AI FACE ───────────────────────────────────────────────────────────
// Redraws only when needsRedraw is set (mood change / boot).
void drawModeFace() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if      (currentMood == "happy")     drawFaceHappy();
  else if (currentMood == "excited")   drawFaceExcited();
  else if (currentMood == "surprised") drawFaceSurprised();
  else if (currentMood == "sleepy")    drawFaceSleepy();
  else if (currentMood == "sad")       drawFaceSad();
  else if (currentMood == "curious")   drawFaceCurious();
  else                                 drawFaceNeutral();

  drawTextCentered(56, currentMood);
  display.display();
}

// ─── MODE: CLOCK ─────────────────────────────────────────────────────────────
// Layout:
//   y= 0  "-- CLOCK --"
//   y= 9  horizontal rule
//   y=14  HH:MM:SS  (textSize 2 → 16 px tall, 96 px wide, x=16)
//   y=36  Mon, 20 Apr 2026  (textSize 1)
void drawModeClock() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(0, "-- CLOCK --");
  display.drawLine(0, 9, 127, 9, WHITE);

  struct tm t;
  if (!getTime(t)) {
    drawTextCentered(28, "Syncing NTP...");
    display.display();
    return;
  }

  // Time — textSize 2: each char 12×16 px; "HH:MM:SS" = 8 chars = 96 px; x=16
  char timeBuf[9];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &t);
  display.setTextSize(2);
  display.setCursor(16, 14);
  display.print(timeBuf);
  display.setTextSize(1);

  // Date below
  char dateBuf[20];
  strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y", &t);  // "Mon, 20 Apr 2026"
  drawTextCentered(38, dateBuf);

  display.display();
}

// ─── MODE: DATE ──────────────────────────────────────────────────────────────
// Layout:
//   y= 0  "-- DATE --"
//   y= 9  rule
//   y=13  Monday
//   y=24  20 April 2026
//   y=36  HH:MM  (textSize 2 → 5 chars = 60 px, x=34)
void drawModeDate() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(0, "-- DATE --");
  display.drawLine(0, 9, 127, 9, WHITE);

  struct tm t;
  if (!getTime(t)) {
    drawTextCentered(28, "Syncing NTP...");
    display.display();
    return;
  }

  char dayName[12], dateStr[18], timeStr[6];
  strftime(dayName,  sizeof(dayName),  "%A",         &t);  // "Monday"
  strftime(dateStr,  sizeof(dateStr),  "%d %B %Y",   &t);  // "20 April 2026"
  strftime(timeStr,  sizeof(timeStr),  "%H:%M",      &t);  // "14:30"

  drawTextCentered(13, dayName);
  drawTextCentered(24, dateStr);

  // Large time — "HH:MM" = 5 chars × 12 px = 60 px; center x=(128-60)/2=34
  display.setTextSize(2);
  display.setCursor(34, 36);
  display.print(timeStr);
  display.setTextSize(1);

  display.display();
}

// ─── MODE: WEATHER ───────────────────────────────────────────────────────────
// Layout:
//   y= 0  "-- WEATHER --"
//   y= 9  rule
//   y=12  city name
//   y=24  "23C"  (textSize 2, centered)
//   y=45  condition text
//   y=56  "LP: Refresh"
void drawModeWeather() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(0, "-- WEATHER --");
  display.drawLine(0, 9, 127, 9, WHITE);
  drawTextCentered(12, WEATHER_CITY);

  if (!weatherValid) {
    drawTextCentered(30, "Loading...");
    drawTextCentered(50, "LP: force fetch");
    display.display();
    return;
  }

  // Temperature (big) — dynamic width centering
  display.setTextSize(2);
  int tW = weatherTemp.length() * 12;
  display.setCursor((SCREEN_W - tW) / 2, 24);
  display.print(weatherTemp);
  display.setTextSize(1);

  drawTextCentered(45, weatherCond);
  drawTextCentered(56, "LP: Refresh");
  display.display();
}

// ─── MODE: STOPWATCH ─────────────────────────────────────────────────────────
// Layout:
//   y= 0  "STOPWATCH"
//   y= 9  rule
//   y=14  MM:SS.cc  (textSize 2, x=16)
//   y=36  [  RUNNING  ] / [  PAUSED  ] / [  STOPPED  ]
//   y=54  LP action hint
unsigned long swGetElapsed() {
  return (swState == SW_RUNNING) ? swAccum + (millis() - swStart) : swAccum;
}

void drawModeStopwatch() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(0, "STOPWATCH");
  display.drawLine(0, 9, 127, 9, WHITE);

  unsigned long ms    = swGetElapsed();
  unsigned long mins  = ms / 60000UL;
  unsigned long secs  = (ms % 60000UL) / 1000UL;
  unsigned long cents = (ms % 1000UL)  / 10UL;

  // "MM:SS.cc" = 8 chars × 12 px = 96 px; x=16
  char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", mins, secs, cents);
  display.setTextSize(2);
  display.setCursor(16, 14);
  display.print(buf);
  display.setTextSize(1);

  if (swState == SW_RUNNING) {
    drawTextCentered(36, "[ RUNNING ]");
    drawTextCentered(54, "LP: Pause");
  } else if (swState == SW_PAUSED) {
    drawTextCentered(36, "[ PAUSED  ]");
    drawTextCentered(54, "LP: Reset");
  } else {
    drawTextCentered(36, "[ STOPPED ]");
    drawTextCentered(54, "LP: Start");
  }

  display.display();
}

// ─── AI MESSAGE PANEL ────────────────────────────────────────────────────────
void drawMessage() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(1, "XenoAI");
  display.drawLine(0, 10, 127, 10, WHITE);
  display.drawLine(0, 11, 127, 11, WHITE);
  drawTextWrapped(currentMessage, 15);
  display.display();
}

// ─── MODE MANAGEMENT ─────────────────────────────────────────────────────────
void cycleMode() {
  currentMode = (AppMode)(((int)currentMode + 1) % (int)MODE_COUNT);
  needsRedraw = true;
  msgShowing  = false;
  Serial.printf("[mode] -> %d\n", (int)currentMode);
}

void handleLongPress() {
  switch (currentMode) {

    case MODE_FACE:
      // AI touch event
      sendTouch();
      break;

    case MODE_WEATHER:
      // Force weather data refresh
      fetchWeather();
      needsRedraw = true;
      break;

    case MODE_STOPWATCH:
      // Cycle: STOPPED→RUNNING | RUNNING→PAUSED | PAUSED→STOPPED(reset)
      if (swState == SW_STOPPED) {
        swStart = millis(); swAccum = 0; swState = SW_RUNNING;
        Serial.println("[sw] START");
      } else if (swState == SW_RUNNING) {
        swAccum += millis() - swStart; swState = SW_PAUSED;
        Serial.println("[sw] PAUSE");
      } else {                          // PAUSED → reset
        swAccum = 0; swState = SW_STOPPED;
        Serial.println("[sw] RESET");
      }
      needsRedraw = true;
      break;

    default:
      break;  // CLOCK, DATE: no action
  }
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nXenoAI v3 booting...");

  // Pins
  pinMode(LED_PIN,   OUTPUT);
  pinMode(TOUCH_PIN, INPUT);

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED FAIL — halting");
    while (true) {}
  }
 // Double-clear flushes any garbage left in SSD1306 GDDRAM after boot
  display.clearDisplay();
    // Initialize RoboEyes with your screen dimensions and a target framerate (e.g., 100 FPS)
  roboEyes.begin(SCREEN_W, SCREEN_H, 100);

  // Start the autoblinker. Parameters: (ON/OFF, interval in seconds, variation in seconds)
  // This blinks both eyes randomly around every 3 seconds (+/- 2 seconds).
  roboEyes.setAutoblinker(true, 3, 2);

  // Start idle mode. Parameters: (ON/OFF, interval in seconds, variation in seconds)
  // This randomly repositions both eyes so XenoAI "looks around" the room.
  roboEyes.setIdleMode(true, 4, 2);
  display.display();
  delay(50);
  display.clearDisplay();
  display.setTextColor(WHITE);
  drawTextCentered(20, "XenoAI v3");
  drawTextCentered(35, "Booting...");
  display.display();
  delay(800);

  // WiFi
  connectWiFi();

  // NTP — must come after WiFi
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  display.clearDisplay();
  drawTextCentered(22, "Syncing time...");
  display.display();
  delay(2000);  // Allow NTP server response; subsequent getTime() is instant

  // OTA
  setupOTA();

  // Initial weather fetch (blocking, acceptable in setup)
  display.clearDisplay();
  drawTextCentered(22, "Fetching weather");
  display.display();
  fetchWeather();
  lastWeatherAt = millis();

  // Touch interrupt — CHANGE to detect both press and release
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), onTouchChange, CHANGE);

  // Ready splash
  display.clearDisplay();
  drawTextCentered(12, "XenoAI v3 Ready!");
  drawTextCentered(28, "Tap: next mode");
  drawTextCentered(44, "Hold: action");
  display.display();
  delay(2000);

  // Initial AI state
  currentMood = "happy";
  lastStateAt = millis();
  needsRedraw = true;
  Serial.println("Boot complete.");
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();
  unsigned long now = millis();

  // ── Touch: tap → cycle mode (or dismiss message) ──────────────────────────
  if (tapFlag) {
    tapFlag = false;
    if (now - lastTouchAt > 300) {   // 300 ms inter-tap debounce
      lastTouchAt = now;
      if (msgShowing) { msgShowing = false; needsRedraw = true; }
      else            { cycleMode(); }
    }
  }

  // ── Touch: long press → mode action ──────────────────────────────────────
  if (longPressFlag) {
    longPressFlag = false;
    if (now - lastTouchAt > 300) {
      lastTouchAt = now;
      handleLongPress();
    }
  }

  // ── AI message timeout ────────────────────────────────────────────────────
  if (msgShowing && now - msgShownAt > MESSAGE_DURATION) {
    msgShowing = false;
    needsRedraw = true;
  }

  // ── Backend state poll every 6 s ──────────────────────────────────────────
  if (now - lastStateAt > STATE_INTERVAL) {
    lastStateAt = now;
    pollState();
  }

  // ── Weather refresh every 10 min ──────────────────────────────────────────
  if (now - lastWeatherAt > WEATHER_INTERVAL) {
    lastWeatherAt = now;
    fetchWeather();
    if (currentMode == MODE_WEATHER) needsRedraw = true;
  }

  // ── Display update ────────────────────────────────────────────────────────
  if (msgShowing) {
    // Message panel overrides all mode displays
    if (needsRedraw) { drawMessage(); needsRedraw = false; }
    return;
  }

  // ── ROBOEYES UPDATE ───────────────────────────────────────────────────────
  // Continuously calculate and draw the animation state if Face mode is active
  if (currentMode == MODE_FACE) {
      roboEyes.update();
  }

  // All other modes: redraw on state change or 200 ms timer (live clock / stopwatch).
  bool shouldRedraw = needsRedraw;
  if (!shouldRedraw && currentMode != MODE_FACE) {
    shouldRedraw = (now - lastDisplayAt > DISPLAY_INTERVAL);
  }

  if (shouldRedraw) {
    lastDisplayAt = now;
    needsRedraw   = false;
    switch (currentMode) {
      case MODE_FACE:      /* Handled continuously by roboEyes.update() above */ break;
      case MODE_CLOCK:     drawModeClock();     break;
      case MODE_DATE:      drawModeDate();      break;
      case MODE_WEATHER:   drawModeWeather();   break;
      case MODE_STOPWATCH: drawModeStopwatch(); break;
    }
  }
}
