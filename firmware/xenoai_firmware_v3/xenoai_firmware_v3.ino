/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║       XenoAI Desk Companion — ESP32-S3 Firmware v3           ║
 * ║       Board: ESP32-S3-WROOM-1 N8R8                           ║
 * ║       Modes: AI Face · Clock · Date · Weather · Stopwatch     ║
 * ║       Built by Xeno                                          ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * HARDWARE:
 *   ESP32-S3-WROOM-1 N8R8 | OLED 1.3" SH1106 (I2C, SDA=8, SCL=9)
 *   TTP223 capacitive touch sensor (GPIO4)
 *   HC-SR04 ultrasonic sensor (TRIG=GPIO5, ECHO=GPIO6)
 *
 * LIBRARIES (Arduino Library Manager):
 *   Adafruit SSD1306 · Adafruit GFX Library · ArduinoJson
 *   FluxGarage_RoboEyes
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
 * HC-SR04 AUTONOMOUS BEHAVIORS:
 *   < 10 cm    → Personal space violated: ANGRY mood + head shake
 *   < 60 cm    → Wake from sleep: DEFAULT mood + surprised animation + /api/arrived
 *   > 100 cm   → Absence timer starts; after 5 min → Sleep (display off)
 *
 * CHANGES FROM v2:
 *   - HC-SR04 integrated (non-blocking: pulseIn 20 ms timeout, fired every 500 ms)
 *   - Autonomous life state machine: AWAKE / SLEEPING
 *   - Presence webhook: POST /api/arrived on wake transition
 *   - applyMoodFromString() helper extracted for reuse by life logic
 *   - Touch ISR changed RISING→CHANGE for tap vs long-press detection
 *   - Mode system added (5 modes)
 *   - NTP clock via configTime() — no extra library
 *   - Weather via weatherapi.com with ArduinoJson filtered parse
 *   - Stopwatch with accurate millis() accumulator
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
#define TRIG_PIN   5   // HC-SR04 Trigger
#define ECHO_PIN   6   // HC-SR04 Echo

// ─── OLED ────────────────────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H  64
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
// Initialize RoboEyes AFTER the display object is created
#include <FluxGarage_RoboEyes.h>
RoboEyes roboEyes(display);

// ─── TIMING CONSTANTS ────────────────────────────────────────────────────────
#define STATE_INTERVAL    6000UL     // Backend poll every 6 s
#define MESSAGE_DURATION  4000UL    // Show AI message for 4 s
#define DISPLAY_INTERVAL   200UL    // Non-face display refresh rate (ms)
#define WEATHER_INTERVAL 600000UL   // Re-fetch weather every 10 min
#define LONG_PRESS_MS      600UL    // Hold ≥ 600 ms → long press

// ─── HC-SR04 CONSTANTS ───────────────────────────────────────────────────────
#define US_FIRE_INTERVAL  500UL     // Fire ultrasonic trigger every 500 ms
#define US_TIMEOUT_US     20000UL   // Max 20 ms echo wait (~343 cm range)
#define LIFE_SLEEP_DIST   100.0f    // cm — beyond this = "absent"
#define LIFE_WAKE_DIST     35.0f    // cm — closer than this = "present" (wakes device)
#define LIFE_ANGRY_DIST     5.0f    // cm — closer than this = personal space violation
#define LIFE_CALM_DIST      7.0f    // cm — beyond this = restore normal expression
#define SLEEP_TIMEOUT_MS  120000UL  // 2 minutes absent → sleep

// ─── HC-SR04 STATE ───────────────────────────────────────────────────────────
float         currentDistanceCm  = 999.0f; // Start as "nobody present"
unsigned long lastUltrasonicAt   = 0;

// ─── LIFE STATE MACHINE ───────────────────────────────────────────────────────
enum LifeState { LIFE_AWAKE, LIFE_SLEEPING };
LifeState     lifeState          = LIFE_AWAKE;
unsigned long lastPresenceAt     = 0;   // Last millis() someone was within LIFE_SLEEP_DIST
bool          personalSpaceActive = false; // True while < LIFE_ANGRY_DIST

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

// ─── OLED HELPERS ────────────────────────────────────────────────────────────

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

// ─── MOOD HELPER ─────────────────────────────────────────────────────────────
// Centralised so both parseResponse() and the life state machine can call it
// without duplicating the if/else chain.
void applyMoodFromString(const String& mood) {
  roboEyes.setPosition(DEFAULT);

  if      (mood == "happy")                      roboEyes.setMood(HAPPY);
  else if (mood == "angry")                      roboEyes.setMood(ANGRY);
  else if (mood == "tired")                      roboEyes.setMood(TIRED);
  else if (mood == "sad")  { roboEyes.setMood(TIRED);  roboEyes.setPosition(S); }
  else if (mood == "irritated") { roboEyes.setMood(ANGRY); roboEyes.anim_confused(); }
  else if (mood == "excited")                    roboEyes.anim_laugh();
  else if (mood == "surprised" || mood == "curious") roboEyes.anim_confused();
  else                                           roboEyes.setMood(DEFAULT);
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
      // Only apply roboEyes mood when life is not overriding
      if (lifeState == LIFE_AWAKE && !personalSpaceActive) {
        applyMoodFromString(currentMood);
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

void sendArrived() {
  Serial.println("[life] -> /api/arrived");
  String resp = httpPost("/api/arrived", "{}");
  if (!resp.isEmpty()) parseResponse(resp);
  Serial.println("[life] arrived: " + resp.substring(0, 80));
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
bool getTime(struct tm& t) {
  return getLocalTime(&t, 200);
}

// ─── HC-SR04: NON-BLOCKING TRIGGER ───────────────────────────────────────────
// Called from loop() every US_FIRE_INTERVAL ms.
// Uses pulseIn() with a hard 20 ms ceiling so the main loop never stalls long.
// At the speed of sound (~343 m/s), 20 ms covers a round-trip of ~343 cm —
// well beyond the HC-SR04's rated 400 cm max, so any genuine object is caught.
// A return value of 0 means timeout (no object detected); we treat that as
// "nobody present" (distance stored as 0.0f, handled as > LIFE_SLEEP_DIST).
void fireUltrasonic() {
  // 2 µs LOW to settle, then 10 µs HIGH pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Read echo — blocks AT MOST US_TIMEOUT_US µs (20 ms) if nothing detected
  long duration = pulseIn(ECHO_PIN, HIGH, US_TIMEOUT_US);

  if (duration == 0) {
    // Timeout: no reflection within range
    currentDistanceCm = 0.0f;
  } else {
    // duration (µs) / 58.0 gives cm  (speed-of-sound round-trip formula)
    currentDistanceCm = (float)duration / 58.0f;
  }

  Serial.printf("[sonar] %.1f cm\n", currentDistanceCm);
}

// ─── LIFE STATE MACHINE ───────────────────────────────────────────────────────
// Reads currentDistanceCm and drives autonomous eye/display behaviour.
// Rules (checked in priority order each loop after each sensor reading):
//
//   1. SLEEPING  + distance < WAKE_DIST (35 cm)
//        → Wake: DEFAULT mood, anim_confused, POST /api/arrived, switch to FACE
//
//   2. AWAKE     + distance < ANGRY_DIST (5 cm)  [personal space]
//        → ANGRY mood + anim_confused (one-shot on entry, restored when > 7 cm)
//
//   3. AWAKE     + absent > SLEEP_TIMEOUT (2 min)
//        → Sleep: TIRED mood, blank display
//
// Backend mood changes are buffered in currentMood; they resume on wake
// or when personal-space clears, via applyMoodFromString().
void handleLifeLogic(unsigned long now) {
  // "Present" = a real reading within the sleep threshold
  bool someonePresent = (currentDistanceCm > 0.0f &&
                         currentDistanceCm <= LIFE_SLEEP_DIST);
  if (someonePresent) lastPresenceAt = now;

  // ── Case 1: Device is sleeping — check for wake condition ────────────────
  if (lifeState == LIFE_SLEEPING) {
    bool wakeCondition = (currentDistanceCm > 0.0f &&
                          currentDistanceCm < LIFE_WAKE_DIST);

    if (wakeCondition) {
      Serial.println("[life] WAKE — someone within " + String(LIFE_WAKE_DIST) + " cm");
      lifeState           = LIFE_AWAKE;
      lastPresenceAt      = now;
      personalSpaceActive = false;

      // Restore roboEyes and animate a "surprised" wake
      roboEyes.setMood(DEFAULT);
      roboEyes.anim_confused();

      // Switch to face mode so the user sees the eyes
      currentMode = MODE_FACE;
      needsRedraw = true;
      msgShowing  = false;

      // Notify backend; response may carry a fresh mood/message
      sendArrived();
    } else {
      // Still sleeping: keep display dark to save power
      display.clearDisplay();
      display.display();
    }
    return;  // No further checks while sleeping
  }

  // ── AWAKE from here ───────────────────────────────────────────────────────

  // ── Case 2: Personal space violation (< 10 cm) ────────────────────────────
  bool inPersonalSpace = (currentDistanceCm > 0.0f &&
                          currentDistanceCm < LIFE_ANGRY_DIST);

  if (inPersonalSpace && !personalSpaceActive) {
    // Enter personal-space state (one-shot)
    personalSpaceActive = true;
    Serial.println("[life] Personal space violated — ANGRY");
    roboEyes.setMood(ANGRY);
    roboEyes.anim_confused();  // Head-shake animation
  } else if (personalSpaceActive &&
             (currentDistanceCm == 0.0f || currentDistanceCm >= LIFE_CALM_DIST)) {
    // Exit personal-space state once they back beyond 7 cm — restore backend mood
    personalSpaceActive = false;
    Serial.println("[life] Personal space cleared — restoring mood");
    applyMoodFromString(currentMood);
  }

  // ── Case 3: Check sleep condition (> 100 cm / absent for 5 min) ──────────
  bool absentTooLong = (now - lastPresenceAt > SLEEP_TIMEOUT_MS);

  if (absentTooLong) {
    Serial.println("[life] SLEEP — absent for 5+ minutes");
    lifeState = LIFE_SLEEPING;
    roboEyes.setMood(TIRED);   // Drowsy eyes before display blanks
    display.clearDisplay();
    display.display();         // Blank the screen
  }
}

// ─── FACE EXPRESSIONS (legacy — kept for non-RoboEyes mood display fallback) ─
void drawFaceNeutral() {
  display.fillCircle(44, 24, 5, WHITE);  display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE);  display.fillCircle(84, 24, 2, BLACK);
  display.drawLine(49, 45, 79, 45, WHITE);
}

void drawFaceHappy() {
  display.drawLine(38, 24, 44, 19, WHITE); display.drawLine(44, 19, 50, 24, WHITE);
  display.drawLine(78, 24, 84, 19, WHITE); display.drawLine(84, 19, 90, 24, WHITE);
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 50 - (i * i) / 30, WHITE);
}

void drawFaceExcited() {
  display.drawLine(38, 24, 50, 24, WHITE); display.drawLine(44, 18, 44, 30, WHITE);
  display.drawLine(39, 19, 49, 29, WHITE); display.drawLine(49, 19, 39, 29, WHITE);
  display.drawLine(78, 24, 90, 24, WHITE); display.drawLine(84, 18, 84, 30, WHITE);
  display.drawLine(79, 19, 89, 29, WHITE); display.drawLine(89, 19, 79, 29, WHITE);
  for (int i = -25; i <= 25; i++) {
    display.drawPixel(64 + i, 50 - (i * i) / 25, WHITE);
    display.drawPixel(64 + i, 51 - (i * i) / 25, WHITE);
  }
}

void drawFaceSurprised() {
  display.drawCircle(44, 24, 7, WHITE); display.fillCircle(44, 24, 3, WHITE);
  display.drawCircle(84, 24, 7, WHITE); display.fillCircle(84, 24, 3, WHITE);
  display.drawCircle(64, 46, 7, WHITE);
}

void drawFaceSleepy() {
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
  display.drawLine(48, 30, 50, 38, WHITE);
  display.drawLine(88, 30, 90, 38, WHITE);
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 42 + (i * i) / 30, WHITE);
}

void drawFaceCurious() {
  display.drawLine(37, 18, 51, 14, WHITE);
  display.drawLine(77, 14, 91, 14, WHITE);
  display.fillCircle(44, 24, 5, WHITE); display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE); display.fillCircle(84, 24, 2, BLACK);
  for (int i = -15; i <= 15; i++)
    display.drawPixel(64 + i, 48 - (i * i) / 35, WHITE);
  display.setCursor(110, 10); display.print("?");
}

// ─── MODE: AI FACE ───────────────────────────────────────────────────────────
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

  char timeBuf[9];
  strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &t);
  display.setTextSize(2);
  display.setCursor(16, 14);
  display.print(timeBuf);
  display.setTextSize(1);

  char dateBuf[20];
  strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y", &t);
  drawTextCentered(38, dateBuf);

  display.display();
}

// ─── MODE: DATE ──────────────────────────────────────────────────────────────
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
  strftime(dayName,  sizeof(dayName),  "%A",         &t);
  strftime(dateStr,  sizeof(dateStr),  "%d %B %Y",   &t);
  strftime(timeStr,  sizeof(timeStr),  "%H:%M",      &t);

  drawTextCentered(13, dayName);
  drawTextCentered(24, dateStr);

  display.setTextSize(2);
  display.setCursor(34, 36);
  display.print(timeStr);
  display.setTextSize(1);

  display.display();
}

// ─── MODE: WEATHER ───────────────────────────────────────────────────────────
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
      sendTouch();
      break;

    case MODE_WEATHER:
      fetchWeather();
      needsRedraw = true;
      break;

    case MODE_STOPWATCH:
      if (swState == SW_STOPPED) {
        swStart = millis(); swAccum = 0; swState = SW_RUNNING;
        Serial.println("[sw] START");
      } else if (swState == SW_RUNNING) {
        swAccum += millis() - swStart; swState = SW_PAUSED;
        Serial.println("[sw] PAUSE");
      } else {
        swAccum = 0; swState = SW_STOPPED;
        Serial.println("[sw] RESET");
      }
      needsRedraw = true;
      break;

    default:
      break;
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
  pinMode(TRIG_PIN,  OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  digitalWrite(TRIG_PIN, LOW);  // Ensure trigger is idle

  // OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED FAIL — halting");
    while (true) {}
  }
  // Double-clear flushes any garbage left in SH1106 GDDRAM after boot
  display.clearDisplay();
  // Initialize RoboEyes
  roboEyes.begin(SCREEN_W, SCREEN_H, 100);
  roboEyes.setAutoblinker(true, 3, 2);
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
  delay(2000);

  // OTA
  setupOTA();

  // Initial weather fetch (blocking — acceptable in setup)
  display.clearDisplay();
  drawTextCentered(22, "Fetching weather");
  display.display();
  fetchWeather();
  lastWeatherAt = millis();

  // Touch interrupt — CHANGE detects both press and release
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), onTouchChange, CHANGE);

  // Life state init — mark "present" so device doesn't sleep immediately on boot
  lastPresenceAt   = millis();
  lastUltrasonicAt = millis();

  // Ready splash
  display.clearDisplay();
  drawTextCentered(12, "XenoAI v3 Ready!");
  drawTextCentered(28, "Tap: next mode");
  drawTextCentered(44, "Hold: action");
  display.display();
  delay(2000);

  // Initial AI state
  currentMood = "happy";
  applyMoodFromString(currentMood);
  lastStateAt = millis();
  needsRedraw = true;
  Serial.println("Boot complete.");
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();
  unsigned long now = millis();

  // ── HC-SR04: fire trigger every 500 ms ────────────────────────────────────
  // pulseIn() has a hard 20 ms ceiling — worst-case adds ~20 ms latency
  // once per 500 ms, which is imperceptible to roboEyes at 100 fps.
  if (now - lastUltrasonicAt >= US_FIRE_INTERVAL) {
    lastUltrasonicAt = now;
    fireUltrasonic();
    // Evaluate life behaviours immediately after each fresh reading
    handleLifeLogic(now);
  }

  // ── If sleeping, skip all UI/backend logic (display already blanked) ───────
  if (lifeState == LIFE_SLEEPING) {
    return;
  }

  // ── Touch: tap → cycle mode (or dismiss message) ──────────────────────────
  if (tapFlag) {
    tapFlag = false;
    if (now - lastTouchAt > 300) {
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
    if (needsRedraw) { drawMessage(); needsRedraw = false; }
    return;
  }

  // ── RoboEyes update — must run every loop() iteration in FACE mode ────────
  // This drives blinking, idle gaze, and mood animations at the target FPS.
  if (currentMode == MODE_FACE) {
    roboEyes.update();
  }

  // All other modes: redraw on state change or every 200 ms (live clock/stopwatch).
  bool shouldRedraw = needsRedraw;
  if (!shouldRedraw && currentMode != MODE_FACE) {
    shouldRedraw = (now - lastDisplayAt > DISPLAY_INTERVAL);
  }

  if (shouldRedraw) {
    lastDisplayAt = now;
    needsRedraw   = false;
    switch (currentMode) {
      case MODE_FACE:      /* roboEyes.update() above handles rendering */  break;
      case MODE_CLOCK:     drawModeClock();     break;
      case MODE_DATE:      drawModeDate();      break;
      case MODE_WEATHER:   drawModeWeather();   break;
      case MODE_STOPWATCH: drawModeStopwatch(); break;
    }
  }
}
