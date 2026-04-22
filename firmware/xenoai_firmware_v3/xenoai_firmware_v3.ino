/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║       XenoAI Desk Companion — ESP32-S3 Firmware v3.1         ║
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
 * LIBRARIES:
 *   Adafruit SH110X · Adafruit GFX · ArduinoJson · FluxGarage_RoboEyes
 *
 * ARDUINO IDE SETTINGS:
 *   Board: ESP32S3 Dev Module | Flash: 8MB | PSRAM: OPI PSRAM
 *   Upload Speed: 921600 | USB CDC: Enabled
 *
 * TOUCH CONTROLS:
 *   Tap       → Next mode (Face → Clock → Date → Weather → Stopwatch → …)
 *   Long hold → Face: AI touch | Weather: force refresh | Stopwatch: start/pause/reset
 *
 * HC-SR04 LIFE BEHAVIORS:
 *   < 5  cm  → Zone 2: ANGRY + head shake (personal space)
 *   5–15 cm  → Zone 1: discomfort, look away (TIRED)
 *   < 35 cm  → Presence detected (resets sleep timer)
 *   > 35 cm  → Absence; after 2 min → Sleep animation → display off
 *   Wake     → DEFAULT + anim + POST /api/arrived
 *
 * EXPRESSION UPGRADES (v3.1):
 *   - Mood transition overlay: pixel face shown 500ms on every mood change
 *   - Idle ambient animations: random pool fires every 15–40s
 *   - 3-zone personal space (discomfort → angry, with hysteresis)
 *   - New moods: nervous, love/content
 *   - Sleep transition: 1.5s TIRED eyes before display blanks
 *   - applyMoodFromString() resets idle/blink to defaults on every call
 *
 * BUGS FIXED (v3.1):
 *   - tapFlag/longPressFlag cleared before debounce → taps silently dropped
 *   - excited/surprised/curious had no base mood → eyes snapped wrong after anim
 *   - TIRED set on sleep then immediately blanked → never shown
 *   - display.clearDisplay() firing every 500ms while sleeping (I2C spam)
 *   - drawModeFace() and all drawFace*() were dead code — now wired as overlays
 *   - Missing drawFaceAngry() — angry fell through to neutral
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

// ─── USER CONFIG ─────────────────────────────────────────────────────────────
#define WIFI_SSID        "Redmi 9i"
#define WIFI_PASS        "strawberry"
#define XENOAI_URL       "https://xenoai-companion.onrender.com"
#define OTA_PASSWORD     "xenoai123"
#define WEATHER_API_KEY  "ca6c8ee702644b2cbe534732262004"
#define WEATHER_CITY     "Nagpur"
#define GMT_OFFSET_S     19800   // IST = UTC+5:30
#define DST_OFFSET_S     0
#define NTP_SERVER       "pool.ntp.org"

// ─── PINS ────────────────────────────────────────────────────────────────────
#define OLED_SDA   8
#define OLED_SCL   9
#define OLED_ADDR  0x3C
#define TOUCH_PIN  4
#define LED_PIN    2
#define TRIG_PIN   5
#define ECHO_PIN   6

// ─── OLED + ROBOEYES ─────────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H  64
Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, -1);
#include <FluxGarage_RoboEyes.h>
RoboEyes roboEyes(display);

// ─── TIMING ──────────────────────────────────────────────────────────────────
#define STATE_INTERVAL      6000UL
#define MESSAGE_DURATION    4000UL
#define DISPLAY_INTERVAL     200UL
#define WEATHER_INTERVAL   600000UL
#define LONG_PRESS_MS        600UL
#define US_FIRE_INTERVAL     500UL    // HC-SR04 cadence
#define US_TIMEOUT_US      20000UL    // 20 ms ceiling

// ─── LIFE / DISTANCE THRESHOLDS ──────────────────────────────────────────────
#define LIFE_PRESENCE_DIST   35.0f    // cm — within this = someone is here
#define LIFE_WAKE_DIST       35.0f    // cm — wakes device from sleep (same as presence)
#define ZONE2_DIST            5.0f    // cm — ANGRY zone
#define ZONE2_CLEAR_DIST      7.0f    // cm — exit angry zone (hysteresis)
#define ZONE1_DIST           15.0f    // cm — discomfort zone (look away)
#define ZONE1_CLEAR_DIST     17.0f    // cm — exit discomfort zone (hysteresis)
#define SLEEP_TIMEOUT_MS   120000UL   // 2 min absent → sleep
#define SLEEP_ANIM_MS       1500UL    // show TIRED eyes for 1.5s before blanking

// ─── EXPRESSION UPGRADE CONSTANTS ────────────────────────────────────────────
#define MOOD_TRANSITION_MS   500UL    // pixel overlay shown this long on mood change
#define IDLE_ANIM_MIN      15000UL    // minimum idle animation interval
#define IDLE_ANIM_MAX      40000UL    // maximum idle animation interval

// ─── HC-SR04 STATE ───────────────────────────────────────────────────────────
float         currentDistanceCm  = 999.0f;
unsigned long lastUltrasonicAt   = 0;

// ─── LIFE STATE ──────────────────────────────────────────────────────────────
enum LifeState { LIFE_AWAKE, LIFE_GOING_SLEEP, LIFE_SLEEPING };
LifeState     lifeState           = LIFE_AWAKE;
unsigned long lastPresenceAt      = 0;
unsigned long sleepTransitionAt   = 0;
bool          sleepDisplayBlanked = false;
uint8_t       personalSpaceZone   = 0;   // 0 = clear, 1 = discomfort, 2 = angry

// ─── MOOD + EXPRESSION STATE ─────────────────────────────────────────────────
String        currentMood         = "neutral";
String        previousMood        = "";
bool          moodTransitioning   = false;
unsigned long moodChangedAt       = 0;

// ─── IDLE AMBIENT ANIMATIONS ─────────────────────────────────────────────────
unsigned long lastIdleAnimAt      = 0;
unsigned long idleAnimInterval    = 20000UL;  // first fires at 20s
uint8_t       idleAnimIndex       = 0;

// ─── APP MODES ───────────────────────────────────────────────────────────────
enum AppMode { MODE_FACE=0, MODE_CLOCK, MODE_DATE, MODE_WEATHER, MODE_STOPWATCH, MODE_COUNT };
AppMode currentMode = MODE_FACE;

// ─── STOPWATCH ───────────────────────────────────────────────────────────────
enum SwState { SW_STOPPED, SW_RUNNING, SW_PAUSED };
SwState       swState  = SW_STOPPED;
unsigned long swStart  = 0, swAccum = 0;

// ─── WEATHER ─────────────────────────────────────────────────────────────────
String        weatherTemp  = "--", weatherCond = "No data";
bool          weatherValid = false;
unsigned long lastWeatherAt = 0;

// ─── AI / BACKEND STATE ──────────────────────────────────────────────────────
String        currentMessage = "";
bool          msgShowing     = false;
unsigned long msgShownAt     = 0, lastStateAt = 0;

// ─── DISPLAY ─────────────────────────────────────────────────────────────────
unsigned long lastDisplayAt = 0;
bool          needsRedraw   = true;

// ─── TOUCH ISR ───────────────────────────────────────────────────────────────
volatile bool          tapFlag       = false;
volatile bool          longPressFlag = false;
volatile unsigned long touchDownAt   = 0;
volatile bool          touchActive   = false;
unsigned long          lastTouchAt   = 0;

void IRAM_ATTR onTouchChange() {
  if (digitalRead(TOUCH_PIN) == HIGH) {
    touchDownAt = millis();
    touchActive = true;
  } else if (touchActive) {
    unsigned long held = millis() - touchDownAt;
    touchActive = false;
    if      (held >= LONG_PRESS_MS) longPressFlag = true;
    else if (held >= 30)            tapFlag       = true;
  }
}

// ─── OLED HELPERS ────────────────────────────────────────────────────────────
void drawText(int x, int y, const String& s) {
  display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(x, y); display.print(s);
}
void drawTextCentered(int y, const String& s) {
  drawText(max(0, (SCREEN_W - (int)s.length() * 6) / 2), y, s);
}
void drawTextWrapped(const String& s, int y) {
  for (int i = 0; i < (int)s.length() && y <= 54; i += 21) {
    drawTextCentered(y, s.substring(i, min((int)s.length(), i + 21)));
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
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  display.clearDisplay();
  if (WiFi.status() == WL_CONNECTED) {
    drawTextCentered(20, "WiFi Connected!");
    drawTextCentered(35, WiFi.localIP().toString());
  } else {
    drawTextCentered(25, "WiFi Failed :(");
  }
  display.display(); delay(1200);
}

// ─── OTA ─────────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    display.clearDisplay(); drawTextCentered(25, "OTA Updating..."); display.display();
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    int pct = p / (t / 100);
    display.clearDisplay(); drawTextCentered(15, "Updating");
    drawTextCentered(30, String(pct) + "%");
    display.drawRect(10, 45, 108, 8, WHITE);
    display.fillRect(10, 45, 108 * pct / 100, 8, WHITE);
    display.display();
  });
  ArduinoOTA.onEnd([]() {
    display.clearDisplay(); drawTextCentered(25, "Done! Rebooting");
    display.display(); delay(1000);
  });
  ArduinoOTA.begin();
}

// ─── HTTP ────────────────────────────────────────────────────────────────────
String httpPost(const String& ep, const String& body) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient h; h.begin(XENOAI_URL + ep);
  h.addHeader("Content-Type", "application/json"); h.setTimeout(8000);
  int c = h.POST((uint8_t*)body.c_str(), body.length());
  String r = (c > 0) ? h.getString() : ""; h.end(); return r;
}
String httpGet(const String& ep) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient h; h.begin(XENOAI_URL + ep); h.setTimeout(8000);
  int c = h.GET(); String r = (c > 0) ? h.getString() : ""; h.end(); return r;
}

// ─── MOOD HELPER ─────────────────────────────────────────────────────────────
// Always resets idle/blink to defaults first, then applies mood-specific overrides.
// Starts the 500ms pixel overlay transition on every call.
// FIX: excited/surprised/curious now set a base mood before triggering animation,
//      so eyes return to the correct expression after the animation completes.
void applyMoodFromString(const String& mood) {
  // Reset RoboEyes behavioural parameters to clean defaults
  roboEyes.setPosition(DEFAULT);
  roboEyes.setAutoblinker(true, 3, 2);
  roboEyes.setIdleMode(true, 4, 2);

  // Kick off pixel overlay transition (shown in loop until MOOD_TRANSITION_MS elapses)
  moodTransitioning = true;
  moodChangedAt     = millis();
  needsRedraw       = true;

  // Apply RoboEyes mood — set base FIRST, then animation on top
  if      (mood == "happy")    { roboEyes.setMood(HAPPY); }
  else if (mood == "angry")    { roboEyes.setMood(ANGRY); }
  else if (mood == "tired")    { roboEyes.setMood(TIRED); }
  else if (mood == "sad")      { roboEyes.setMood(TIRED);  roboEyes.setPosition(SW); }
  else if (mood == "irritated"){ roboEyes.setMood(ANGRY);  roboEyes.anim_confused(); }
  else if (mood == "excited")  { roboEyes.setMood(HAPPY);  roboEyes.anim_laugh(); }     // FIX: base mood first
  else if (mood == "surprised"){ roboEyes.setMood(DEFAULT); roboEyes.anim_confused(); } // FIX: base mood first
  else if (mood == "curious")  { roboEyes.setMood(HAPPY);  roboEyes.anim_confused(); } // FIX: base mood first
  else if (mood == "nervous")  {
    roboEyes.setMood(TIRED);
    roboEyes.setIdleMode(true, 1, 0);       // fast anxious eye movement
    roboEyes.setAutoblinker(true, 1, 1);    // rapid nervous blinking
  }
  else if (mood == "love" || mood == "content") {
    roboEyes.setMood(HAPPY);
    roboEyes.setAutoblinker(true, 6, 2);    // slow dreamy blink
    roboEyes.setIdleMode(false, 0, 0);      // still, gazing
  }
  else { roboEyes.setMood(DEFAULT); }
}

// ─── BACKEND JSON PARSER ─────────────────────────────────────────────────────
void parseResponse(const String& json) {
  if (json.isEmpty()) return;
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;

  if (doc.containsKey("mood")) {
    String newMood = doc["mood"].as<String>();
    if (newMood != currentMood) {
      previousMood = currentMood;
      currentMood  = newMood;
      needsRedraw  = true;
      if (lifeState == LIFE_AWAKE && personalSpaceZone == 0)
        applyMoodFromString(currentMood);
    }
  }
  if (doc.containsKey("message")) {
    String msg = doc["message"].as<String>();
    if (msg.length() > 0) {
      currentMessage = msg; msgShowing = true; msgShownAt = millis();
    }
  }
}

void sendTouch()   { parseResponse(httpPost("/api/touch",   "{}")); }
void pollState()   { parseResponse(httpGet("/api/state")); }
void sendArrived() { parseResponse(httpPost("/api/arrived", "{}")); }

// ─── WEATHER ─────────────────────────────────────────────────────────────────
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient h;
  h.begin("http://api.weatherapi.com/v1/current.json?key=" + String(WEATHER_API_KEY) +
          "&q=" + WEATHER_CITY + "&aqi=no");
  h.setTimeout(5000);
  if (h.GET() == 200) {
    StaticJsonDocument<64>  f;
    f["current"]["temp_c"] = true;
    f["current"]["condition"]["text"] = true;
    StaticJsonDocument<256> d;
    if (!deserializeJson(d, h.getString(), DeserializationOption::Filter(f))) {
      weatherTemp  = String((int)roundf(d["current"]["temp_c"] | 0.0f)) + "C";
      weatherCond  = String(d["current"]["condition"]["text"] | "Unknown").substring(0, 21);
      weatherValid = true;
    }
  }
  h.end();
}

bool getTime(struct tm& t) { return getLocalTime(&t, 200); }

// ─── HC-SR04 ─────────────────────────────────────────────────────────────────
void fireUltrasonic() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, US_TIMEOUT_US);
  currentDistanceCm = (dur == 0) ? 0.0f : dur / 58.0f;
  Serial.printf("[sonar] %.1f cm\n", currentDistanceCm);
}

// ─── IDLE AMBIENT ANIMATIONS ─────────────────────────────────────────────────
// Cycles through a pool of ambient behaviours every 15–40s.
// Only fires when: AWAKE + MODE_FACE + no personal space + not transitioning.
void fireIdleAnimation() {
  idleAnimIndex = (idleAnimIndex + 1) % 5;
  switch (idleAnimIndex) {
    case 0:
      roboEyes.anim_laugh();                           // happy bounce
      break;
    case 1:
      roboEyes.anim_confused();                        // curious head-shake
      break;
    case 2:
      roboEyes.setPosition(W);                         // glance left
      break;
    case 3:
      roboEyes.setPosition(NE);                        // look up-right (thinking)
      break;
    case 4:
      roboEyes.setPosition(E);                         // glance right
      break;
  }
  // Randomise next interval and reset timer
  idleAnimInterval = (unsigned long)random(IDLE_ANIM_MIN, IDLE_ANIM_MAX);
  lastIdleAnimAt   = millis();
  Serial.printf("[idle] anim %d, next in %lus\n", idleAnimIndex, idleAnimInterval / 1000);
}

// ─── LIFE STATE MACHINE ───────────────────────────────────────────────────────
void handleLifeLogic(unsigned long now) {
  bool present = (currentDistanceCm > 0.0f && currentDistanceCm <= LIFE_PRESENCE_DIST);
  if (present) lastPresenceAt = now;

  // ── SLEEPING / GOING_SLEEP ────────────────────────────────────────────────
  if (lifeState == LIFE_GOING_SLEEP) {
    if (now - sleepTransitionAt >= SLEEP_ANIM_MS) {
      // Transition complete — blank display
      lifeState = LIFE_SLEEPING;
      display.clearDisplay(); display.display();
      sleepDisplayBlanked = true;
    }
    // During transition, roboEyes.update() in loop() shows TIRED eyes
    return;
  }

  if (lifeState == LIFE_SLEEPING) {
    if (currentDistanceCm > 0.0f && currentDistanceCm < LIFE_WAKE_DIST) {
      Serial.println("[life] WAKE");
      lifeState           = LIFE_AWAKE;
      sleepDisplayBlanked = false;
      personalSpaceZone   = 0;
      lastPresenceAt      = now;
      lastIdleAnimAt      = now;   // prevent instant idle anim on wake

      roboEyes.setAutoblinker(true, 3, 2);
      roboEyes.setIdleMode(true, 4, 2);
      applyMoodFromString("neutral");          // DEFAULT eyes + transition overlay
      roboEyes.anim_confused();               // wake surprise animation

      currentMode = MODE_FACE; needsRedraw = true; msgShowing = false;
      sendArrived();
    }
    // No display calls here — display is already blank (sleepDisplayBlanked)
    return;
  }

  // ── AWAKE: 3-zone personal space ─────────────────────────────────────────
  uint8_t newZone = 0;
  if (currentDistanceCm > 0.0f) {
    if      (currentDistanceCm < ZONE2_DIST)  newZone = 2;
    else if (currentDistanceCm < ZONE1_DIST)  newZone = 1;
  }

  // Apply hysteresis on exit
  if (personalSpaceZone == 2 && newZone < 2 && currentDistanceCm < ZONE2_CLEAR_DIST)
    newZone = 2;   // stay in zone 2 until past clear threshold
  if (personalSpaceZone == 1 && newZone < 1 && currentDistanceCm < ZONE1_CLEAR_DIST)
    newZone = 1;   // stay in zone 1 until past clear threshold

  if (newZone != personalSpaceZone) {
    personalSpaceZone = newZone;
    switch (newZone) {
      case 2:
        Serial.println("[life] Zone 2 — ANGRY");
        roboEyes.setMood(ANGRY);
        roboEyes.anim_confused();   // head-shake
        break;
      case 1:
        Serial.println("[life] Zone 1 — discomfort");
        roboEyes.setMood(TIRED);
        roboEyes.setPosition(W);    // look away
        break;
      case 0:
        Serial.println("[life] Zone clear — restoring mood");
        applyMoodFromString(currentMood);
        break;
    }
  }

  // ── AWAKE: sleep trigger ──────────────────────────────────────────────────
  if (now - lastPresenceAt > SLEEP_TIMEOUT_MS && lifeState == LIFE_AWAKE) {
    Serial.println("[life] Going to sleep…");
    lifeState           = LIFE_GOING_SLEEP;
    sleepTransitionAt   = now;
    sleepDisplayBlanked = false;
    personalSpaceZone   = 0;
    roboEyes.setIdleMode(false, 0, 0);
    roboEyes.setAutoblinker(true, 5, 3);  // slow sleepy blink during transition
    roboEyes.setMood(TIRED);
    roboEyes.setPosition(S);              // eyes drooping downward
  }
}

// ─── PIXEL FACE DRAW FUNCTIONS ───────────────────────────────────────────────
// Used as 500ms transition overlays on mood change (then roboEyes takes over).
// Also provides a visual "cut" between moods so changes feel intentional.

void drawFaceNeutral() {
  display.fillCircle(44, 24, 5, WHITE); display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE); display.fillCircle(84, 24, 2, BLACK);
  display.drawLine(49, 45, 79, 45, WHITE);
}

void drawFaceHappy() {
  display.drawLine(38, 24, 44, 19, WHITE); display.drawLine(44, 19, 50, 24, WHITE);
  display.drawLine(78, 24, 84, 19, WHITE); display.drawLine(84, 19, 90, 24, WHITE);
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 50 - (i * i) / 30, WHITE);
}

void drawFaceExcited() {
  // Star eyes
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
  display.drawLine(48, 30, 50, 38, WHITE); display.drawLine(88, 30, 90, 38, WHITE);
  for (int i = -20; i <= 20; i++)
    display.drawPixel(64 + i, 42 + (i * i) / 30, WHITE);
}

void drawFaceCurious() {
  display.drawLine(37, 18, 51, 14, WHITE); display.drawLine(77, 14, 91, 14, WHITE);
  display.fillCircle(44, 24, 5, WHITE); display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 5, WHITE); display.fillCircle(84, 24, 2, BLACK);
  for (int i = -15; i <= 15; i++)
    display.drawPixel(64 + i, 48 - (i * i) / 35, WHITE);
  display.setCursor(110, 10); display.print("?");
}

// FIX: was missing — "angry" previously fell through to neutral
void drawFaceAngry() {
  // Angled inward brows
  display.drawLine(38, 18, 50, 24, WHITE);
  display.drawLine(78, 24, 90, 18, WHITE);
  // Squinting eyes (filled rect over top of circles)
  display.fillCircle(44, 27, 5, WHITE); display.fillRect(39, 21, 11, 5, BLACK);
  display.fillCircle(84, 27, 5, WHITE); display.fillRect(79, 21, 11, 5, BLACK);
  // Frown
  for (int i = -15; i <= 15; i++)
    display.drawPixel(64 + i, 48 + (i * i) / 22, WHITE);
}

// NEW: nervous mood
void drawFaceNervous() {
  // Wide open eyes with shaking pupils
  display.drawCircle(44, 24, 7, WHITE); display.fillCircle(46, 26, 2, WHITE);
  display.drawCircle(84, 24, 7, WHITE); display.fillCircle(86, 26, 2, WHITE);
  // Wavy nervous mouth
  for (int i = -14; i <= 14; i++)
    display.drawPixel(64 + i, 46 + ((i / 4) % 2 == 0 ? 1 : -1), WHITE);
  // Sweat drop
  display.drawLine(52, 13, 52, 17, WHITE);
  display.drawPixel(51, 18, WHITE); display.drawPixel(53, 18, WHITE);
}

// NEW: love / content mood
void drawFaceLove() {
  // Heart eyes (two filled circles + triangle = heart shape)
  display.fillCircle(41, 22, 3, WHITE); display.fillCircle(47, 22, 3, WHITE);
  display.fillTriangle(38, 23, 50, 23, 44, 30, WHITE);
  display.fillCircle(81, 22, 3, WHITE); display.fillCircle(87, 22, 3, WHITE);
  display.fillTriangle(78, 23, 90, 23, 84, 30, WHITE);
  // Big warm smile
  for (int i = -22; i <= 22; i++)
    display.drawPixel(64 + i, 52 - (i * i) / 24, WHITE);
}

// ─── MOOD TRANSITION OVERLAY ─────────────────────────────────────────────────
// Renders the correct pixel face for the current mood. Called during the 500ms
// transition window so the user sees a clear "cut" before RoboEyes takes over.
void drawMoodOverlay() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  if      (currentMood == "happy")                   drawFaceHappy();
  else if (currentMood == "excited")                  drawFaceExcited();
  else if (currentMood == "surprised")                drawFaceSurprised();
  else if (currentMood == "tired" || currentMood == "sleepy") drawFaceSleepy();
  else if (currentMood == "sad")                      drawFaceSad();
  else if (currentMood == "curious")                  drawFaceCurious();
  else if (currentMood == "angry" || currentMood == "irritated") drawFaceAngry();
  else if (currentMood == "nervous")                  drawFaceNervous();
  else if (currentMood == "love" || currentMood == "content")   drawFaceLove();
  else                                                drawFaceNeutral();

  // Small mood label at bottom
  drawTextCentered(56, currentMood);
  display.display();
}

// ─── MODE RENDERERS ──────────────────────────────────────────────────────────
void drawModeClock() {
  display.clearDisplay(); display.setTextColor(WHITE);
  drawTextCentered(0, "-- CLOCK --"); display.drawLine(0, 9, 127, 9, WHITE);
  struct tm t;
  if (!getTime(t)) { drawTextCentered(28, "Syncing NTP..."); display.display(); return; }
  char tb[9]; strftime(tb, sizeof(tb), "%H:%M:%S", &t);
  display.setTextSize(2); display.setCursor(16, 14); display.print(tb); display.setTextSize(1);
  char db[20]; strftime(db, sizeof(db), "%a, %d %b %Y", &t);
  drawTextCentered(38, db); display.display();
}

void drawModeDate() {
  display.clearDisplay(); display.setTextColor(WHITE);
  drawTextCentered(0, "-- DATE --"); display.drawLine(0, 9, 127, 9, WHITE);
  struct tm t;
  if (!getTime(t)) { drawTextCentered(28, "Syncing NTP..."); display.display(); return; }
  char dn[12], ds[18], ts[6];
  strftime(dn, sizeof(dn), "%A", &t);
  strftime(ds, sizeof(ds), "%d %B %Y", &t);
  strftime(ts, sizeof(ts), "%H:%M", &t);
  drawTextCentered(13, dn); drawTextCentered(24, ds);
  display.setTextSize(2); display.setCursor(34, 36); display.print(ts); display.setTextSize(1);
  display.display();
}

void drawModeWeather() {
  display.clearDisplay(); display.setTextColor(WHITE);
  drawTextCentered(0, "-- WEATHER --"); display.drawLine(0, 9, 127, 9, WHITE);
  drawTextCentered(12, WEATHER_CITY);
  if (!weatherValid) {
    drawTextCentered(30, "Loading..."); drawTextCentered(50, "LP: force fetch");
    display.display(); return;
  }
  display.setTextSize(2);
  display.setCursor((SCREEN_W - weatherTemp.length() * 12) / 2, 24);
  display.print(weatherTemp); display.setTextSize(1);
  drawTextCentered(45, weatherCond); drawTextCentered(56, "LP: Refresh");
  display.display();
}

unsigned long swGetElapsed() {
  return (swState == SW_RUNNING) ? swAccum + (millis() - swStart) : swAccum;
}
void drawModeStopwatch() {
  display.clearDisplay(); display.setTextColor(WHITE);
  drawTextCentered(0, "STOPWATCH"); display.drawLine(0, 9, 127, 9, WHITE);
  unsigned long ms = swGetElapsed(); char buf[9];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", ms/60000, (ms%60000)/1000, (ms%1000)/10);
  display.setTextSize(2); display.setCursor(16, 14); display.print(buf); display.setTextSize(1);
  if      (swState == SW_RUNNING) { drawTextCentered(36, "[ RUNNING ]"); drawTextCentered(54, "LP: Pause"); }
  else if (swState == SW_PAUSED)  { drawTextCentered(36, "[ PAUSED  ]"); drawTextCentered(54, "LP: Reset"); }
  else                            { drawTextCentered(36, "[ STOPPED ]"); drawTextCentered(54, "LP: Start"); }
  display.display();
}

void drawMessage() {
  display.clearDisplay(); display.setTextColor(WHITE);
  drawTextCentered(1, "XenoAI");
  display.drawLine(0, 10, 127, 10, WHITE); display.drawLine(0, 11, 127, 11, WHITE);
  drawTextWrapped(currentMessage, 15); display.display();
}

// ─── MODE MANAGEMENT ─────────────────────────────────────────────────────────
void cycleMode() {
  currentMode = (AppMode)(((int)currentMode + 1) % (int)MODE_COUNT);
  needsRedraw = true; msgShowing = false;
  Serial.printf("[mode] -> %d\n", (int)currentMode);
}

void handleLongPress() {
  switch (currentMode) {
    case MODE_FACE:      sendTouch(); break;
    case MODE_WEATHER:   fetchWeather(); needsRedraw = true; break;
    case MODE_STOPWATCH:
      if      (swState == SW_STOPPED) { swStart = millis(); swAccum = 0;               swState = SW_RUNNING; Serial.println("[sw] START"); }
      else if (swState == SW_RUNNING) { swAccum += millis() - swStart;                 swState = SW_PAUSED;  Serial.println("[sw] PAUSE"); }
      else                            { swAccum = 0;                                   swState = SW_STOPPED; Serial.println("[sw] RESET"); }
      needsRedraw = true; break;
    default: break;
  }
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\nXenoAI v3.1 booting…");

  pinMode(LED_PIN, OUTPUT); pinMode(TOUCH_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(OLED_ADDR, true)) { Serial.println("OLED FAIL"); while (true) {} }
  display.clearDisplay();
  roboEyes.begin(SCREEN_W, SCREEN_H, 100);
  roboEyes.setAutoblinker(true, 3, 2);
  roboEyes.setIdleMode(true, 4, 2);
  display.display(); delay(50);

  display.clearDisplay(); drawTextCentered(20, "XenoAI v3.1");
  drawTextCentered(35, "Booting…"); display.display(); delay(800);

  connectWiFi();
  configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  display.clearDisplay(); drawTextCentered(22, "Syncing time…"); display.display(); delay(2000);
  setupOTA();
  display.clearDisplay(); drawTextCentered(22, "Fetching weather"); display.display();
  fetchWeather(); lastWeatherAt = millis();

  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), onTouchChange, CHANGE);

  lastPresenceAt   = millis();
  lastUltrasonicAt = millis();
  lastIdleAnimAt   = millis();
  idleAnimInterval = 20000UL;

  display.clearDisplay(); drawTextCentered(12, "XenoAI v3.1 Ready!");
  drawTextCentered(28, "Tap: next mode"); drawTextCentered(44, "Hold: action");
  display.display(); delay(2000);

  currentMood = "happy";
  applyMoodFromString(currentMood);
  lastStateAt = millis(); needsRedraw = true;
  Serial.println("Boot complete.");
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();
  unsigned long now = millis();

  // ── HC-SR04 (500 ms cadence, 20 ms max block) ─────────────────────────────
  if (now - lastUltrasonicAt >= US_FIRE_INTERVAL) {
    lastUltrasonicAt = now;
    fireUltrasonic();
    handleLifeLogic(now);
  }

  // ── Sleep / going-to-sleep: drive RoboEyes during transition, then stop ───
  if (lifeState == LIFE_GOING_SLEEP) {
    roboEyes.update();   // show TIRED eyes during 1.5s transition
    return;
  }
  if (lifeState == LIFE_SLEEPING) return;  // display already blank, do nothing

  // ── Touch: FIX — flag cleared only when debounce passes ──────────────────
  if (tapFlag && now - lastTouchAt > 300) {
    tapFlag = false; lastTouchAt = now;
    if (msgShowing) { msgShowing = false; needsRedraw = true; }
    else            { cycleMode(); }
  }
  if (longPressFlag && now - lastTouchAt > 300) {
    longPressFlag = false; lastTouchAt = now;
    handleLongPress();
  }

  // ── Message expiry ────────────────────────────────────────────────────────
  if (msgShowing && now - msgShownAt > MESSAGE_DURATION) { msgShowing = false; needsRedraw = true; }

  // ── Backend poll ──────────────────────────────────────────────────────────
  if (now - lastStateAt > STATE_INTERVAL) { lastStateAt = now; pollState(); }

  // ── Weather refresh ───────────────────────────────────────────────────────
  if (now - lastWeatherAt > WEATHER_INTERVAL) {
    lastWeatherAt = now; fetchWeather();
    if (currentMode == MODE_WEATHER) needsRedraw = true;
  }

  // ── Message panel overrides all face rendering ────────────────────────────
  if (msgShowing) { if (needsRedraw) { drawMessage(); needsRedraw = false; } return; }

  // ── FACE MODE ─────────────────────────────────────────────────────────────
  if (currentMode == MODE_FACE) {
    if (moodTransitioning) {
      // Show pixel overlay for MOOD_TRANSITION_MS then hand off to RoboEyes
      if (now - moodChangedAt < MOOD_TRANSITION_MS) {
        if (needsRedraw) { drawMoodOverlay(); needsRedraw = false; }
      } else {
        moodTransitioning = false;
        needsRedraw = false;
        // RoboEyes now takes over — reset idle timer so it doesn't fire instantly
        lastIdleAnimAt = now;
      }
    } else {
      // RoboEyes drives continuous animation
      roboEyes.update();

      // Idle ambient animation (only when no personal space, not transitioning)
      if (personalSpaceZone == 0 && now - lastIdleAnimAt > idleAnimInterval) {
        fireIdleAnimation();
      }
    }
    return;
  }

  // ── Other modes: redraw on state change or every 200 ms ──────────────────
  bool doRedraw = needsRedraw || (now - lastDisplayAt > DISPLAY_INTERVAL);
  if (doRedraw) {
    lastDisplayAt = now; needsRedraw = false;
    switch (currentMode) {
      case MODE_CLOCK:     drawModeClock();     break;
      case MODE_DATE:      drawModeDate();      break;
      case MODE_WEATHER:   drawModeWeather();   break;
      case MODE_STOPWATCH: drawModeStopwatch(); break;
      default: break;
    }
  }
}
