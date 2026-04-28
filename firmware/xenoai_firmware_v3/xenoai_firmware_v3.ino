/*
 * XenoAI Desk Companion
 * ESP32-S3-WROOM-1 (N8R8) | SH1106G OLED | TTP223 | HC-SR04
 *
 * IMPORTANT: Patch FluxGarage_RoboEyes.h to use Adafruit_SH1106G
 * (see project README).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>      // v7 syntax
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FluxGarage_RoboEyes.h>
#include <time.h>

// ---------- Display ----------
#define WHITE SH110X_WHITE
#define BLACK SH110X_BLACK
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
roboEyes roboEyes;  // patched library uses SH1106G

// ---------- Pins ----------
#define PIN_SDA       8
#define PIN_SCL       9
#define PIN_TOUCH     4
#define PIN_TRIG      5
#define PIN_ECHO      6
#define PIN_LED       2

// ---------- Credentials ----------
#define WIFI_SSID     "Agam"
#define WIFI_PASS     "agam2426"
#define OTA_PASSWORD  "xeno123"
#define OTA_HOSTNAME  "xenoai-desk"

#define WEATHER_KEY   "e12cba0f90fc4fedb7d71530262804"
#define WEATHER_CITY  "nagpur,maharashtra"

#define BACKEND_BASE  "https://xenoai-companion.onrender.com"

// ---------- Timing constants ----------
const uint32_t SONAR_INTERVAL_MS      = 500;
const uint32_t SONAR_TIMEOUT_US       = 10000;   // ~170 cm cap (safe vs 35cm logic)
const uint32_t STATE_POLL_MS          = 6000;
const uint32_t WEATHER_INTERVAL_MS    = 10UL * 60UL * 1000UL;
const uint32_t TOUCH_DEBOUNCE_MS      = 300;
const uint32_t LONG_PRESS_MS          = 600;
const uint32_t MSG_OVERLAY_MS         = 4000;
const uint32_t MOOD_CUT_MS            = 500;
const uint32_t ABSENCE_TO_SLEEP_MS    = 2UL * 60UL * 1000UL;
const uint32_t SLEEP_PHASE1_MS        = 1500;
const uint32_t SLEEP_FRAME_MS         = 30;       // ~33 fps
const uint32_t ZZZ_STAGE_MS           = 700;
const uint32_t ZZZ_SPAWN_MS           = 3000;

// ---------- Zones ----------
const float ZONE2_ENTER = 5.0f;
const float ZONE2_EXIT  = 5.0f + 2.0f;
const float ZONE1_ENTER = 15.0f;
const float ZONE1_EXIT  = 15.0f + 2.0f;
const float PRESENCE    = 35.0f;
const float PRESENCE_EXIT = 35.0f + 2.0f;

// ---------- App state ----------
enum AppMode { MODE_FACE, MODE_CLOCK, MODE_DATE, MODE_WEATHER, MODE_STOPWATCH, MODE_COUNT };
AppMode appMode = MODE_FACE;

enum SpatialZone { ZONE_NONE, ZONE_PRESENT, ZONE_DISCOMFORT, ZONE_PERSONAL };
SpatialZone currentZone = ZONE_NONE;

enum LifeState { LIFE_AWAKE, LIFE_GOING_TO_SLEEP, LIFE_SLEEPING };
LifeState lifeState = LIFE_AWAKE;

enum MoodId {
  M_NEUTRAL, M_HAPPY, M_ANGRY, M_TIRED, M_SAD, M_IRRITATED,
  M_EXCITED, M_SURPRISED, M_NERVOUS, M_LOVE
};
MoodId currentMood = M_NEUTRAL;

// Stopwatch
enum SwState { SW_RESET, SW_RUNNING, SW_PAUSED };
SwState swState = SW_RESET;
uint32_t swStartMs = 0;
uint32_t swAccumMs = 0;

// ---------- Runtime variables ----------
float currentDistanceCm = 999.0f;
uint32_t lastSonarMs = 0;
uint32_t lastStatePollMs = 0;
uint32_t lastWeatherMs = 0;
uint32_t lastPresenceMs = 0;
uint32_t lifeStateEnterMs = 0;
uint32_t lastIdleAnimMs = 0;
uint32_t nextIdleDelay = 20000;

// Touch
volatile uint32_t touchEdgeMs = 0;
volatile bool touchEdgePending = false;
volatile int  touchEdgeLevel = 0;
uint32_t touchPressStartMs = 0;
bool touchHeld = false;
bool longPressFired = false;

// Mood transition overlay
bool moodOverlayActive = false;
uint32_t moodOverlayStartMs = 0;
MoodId moodOverlayId = M_NEUTRAL;

// Message overlay
bool msgOverlayActive = false;
uint32_t msgOverlayStartMs = 0;
String msgOverlayText = "";

// Weather cache
String weatherCondition = "--";
float  weatherTempC = 0.0f;
bool   weatherValid = false;

// Sleep animation Z's
struct ZBubble { uint32_t bornMs; uint8_t stage; bool alive; };
ZBubble zbubs[3];
uint32_t lastZSpawnMs = 0;
uint32_t lastSleepFrameMs = 0;

// WiFi / NTP
bool wifiUp = false;
bool timeSynced = false;

// ============================================================
//                         ISR
// ============================================================
void IRAM_ATTR onTouchChange() {
  touchEdgeMs = millis();
  touchEdgeLevel = digitalRead(PIN_TOUCH);
  touchEdgePending = true;
}

// ============================================================
//                       SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[XenoAI] Booting...");

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_TOUCH, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  // I2C + OLED
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!display.begin(0x3C, true)) {
    Serial.println("[OLED] Init failed");
    while (1) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(100); }
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("XenoAI booting...");
  display.display();

  // RoboEyes
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 50);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);
  roboEyes.setMood(DEFAULT);

  // Touch interrupt
  attachInterrupt(digitalPinToInterrupt(PIN_TOUCH), onTouchChange, CHANGE);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiUp = true;
    Serial.print("\n[WiFi] OK ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] FAILED (continuing offline)");
  }

  // NTP (UTC+5:30 = 19800 seconds)
  if (wifiUp) {
    configTime(19800, 0, "pool.ntp.org", "time.google.com");
    struct tm ti;
    if (getLocalTime(&ti, 5000)) {
      timeSynced = true;
      Serial.println("[NTP] Synced");
    } else {
      Serial.println("[NTP] Sync failed");
    }
  }

  // OTA
  if (wifiUp) {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("[OTA] End");   });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Err %u\n", e); });
    ArduinoOTA.begin();
  }

  // Initial weather fetch
  fetchWeather();
  lastWeatherMs = millis();

  // Initial idle anim scheduler
  lastIdleAnimMs = millis();
  nextIdleDelay = random(15000, 40001);

  // Mark presence so we don't immediately sleep on boot
  lastPresenceMs = millis();
  lifeStateEnterMs = millis();

  Serial.println("[XenoAI] Ready");
}

// ============================================================
//                       MAIN LOOP
// ============================================================
void loop() {
  uint32_t now = millis();

  if (wifiUp) ArduinoOTA.handle();

  handleTouch(now);
  handleSonar(now);
  handleSpatialAndLife(now);
  handleStatePoll(now);
  handleWeatherTimer(now);
  handleIdleAnim(now);

  render(now);
}

// ============================================================
//                       TOUCH HANDLING
// ============================================================
void handleTouch(uint32_t now) {
  // Snapshot ISR data
  bool pending;
  int level;
  uint32_t tEdge;
  noInterrupts();
  pending = touchEdgePending;
  level = touchEdgeLevel;
  tEdge = touchEdgeMs;
  touchEdgePending = false;
  interrupts();

  if (pending) {
    // Debounce
    static uint32_t lastEdgeMs = 0;
    if (now - lastEdgeMs >= TOUCH_DEBOUNCE_MS) {
      lastEdgeMs = now;
      if (level == HIGH && !touchHeld) {
        touchHeld = true;
        touchPressStartMs = now;
        longPressFired = false;
      } else if (level == LOW && touchHeld) {
        touchHeld = false;
        if (!longPressFired) {
          // Short tap -> cycle mode
          onShortTap();
        }
      }
    }
  }

  // Long press detection
  if (touchHeld && !longPressFired && (now - touchPressStartMs >= LONG_PRESS_MS)) {
    longPressFired = true;
    onLongPress();
  }
}

void onShortTap() {
  if (lifeState == LIFE_SLEEPING) return;  // ignore taps while asleep (motion wakes)
  appMode = (AppMode)((appMode + 1) % MODE_COUNT);
  Serial.printf("[Mode] -> %d\n", appMode);
}

void onLongPress() {
  Serial.println("[Touch] LONG PRESS");
  switch (appMode) {
    case MODE_FACE:
      postEndpoint("/api/touch");
      break;
    case MODE_WEATHER:
      fetchWeather();
      lastWeatherMs = millis();
      break;
    case MODE_STOPWATCH:
      stopwatchAdvance();
      break;
    default: break;
  }
}

void stopwatchAdvance() {
  uint32_t now = millis();
  switch (swState) {
    case SW_RESET:
      swStartMs = now;
      swAccumMs = 0;
      swState = SW_RUNNING;
      break;
    case SW_RUNNING:
      swAccumMs += (now - swStartMs);
      swState = SW_PAUSED;
      break;
    case SW_PAUSED:
      swStartMs = now;
      swState = SW_RESET;
      swAccumMs = 0;
      break;
  }
}

uint32_t stopwatchElapsed() {
  if (swState == SW_RUNNING) return swAccumMs + (millis() - swStartMs);
  return swAccumMs;
}

// ============================================================
//                       SONAR (non-blocking-ish)
// ============================================================
void handleSonar(uint32_t now) {
  if (now - lastSonarMs < SONAR_INTERVAL_MS) return;
  lastSonarMs = now;

  // Trigger
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Capped pulseIn (10ms ceiling -> ~170cm max, well above our 35cm logic)
  uint32_t dur = pulseIn(PIN_ECHO, HIGH, SONAR_TIMEOUT_US);
  if (dur == 0) {
    currentDistanceCm = 999.0f;
  } else {
    currentDistanceCm = (dur * 0.0343f) / 2.0f;
  }
}

// ============================================================
//             SPATIAL ZONES + LIFE STATE MACHINE
// ============================================================
void handleSpatialAndLife(uint32_t now) {
  float d = currentDistanceCm;

  // Update presence timestamp
  if (d < PRESENCE) lastPresenceMs = now;

  // ---------- Zone transitions with hysteresis ----------
  SpatialZone newZone = currentZone;

  switch (currentZone) {
    case ZONE_PERSONAL:
      if (d > ZONE2_EXIT) newZone = (d <= ZONE1_ENTER) ? ZONE_DISCOMFORT
                                  : (d <= PRESENCE)    ? ZONE_PRESENT : ZONE_NONE;
      break;
    case ZONE_DISCOMFORT:
      if (d <= ZONE2_ENTER) newZone = ZONE_PERSONAL;
      else if (d > ZONE1_EXIT) newZone = (d <= PRESENCE) ? ZONE_PRESENT : ZONE_NONE;
      break;
    case ZONE_PRESENT:
      if (d <= ZONE2_ENTER) newZone = ZONE_PERSONAL;
      else if (d <= ZONE1_ENTER) newZone = ZONE_DISCOMFORT;
      else if (d > PRESENCE_EXIT) newZone = ZONE_NONE;
      break;
    case ZONE_NONE:
      if (d <= ZONE2_ENTER) newZone = ZONE_PERSONAL;
      else if (d <= ZONE1_ENTER) newZone = ZONE_DISCOMFORT;
      else if (d <= PRESENCE) newZone = ZONE_PRESENT;
      break;
  }

  if (newZone != currentZone) {
    currentZone = newZone;
    Serial.printf("[Zone] -> %d (d=%.1fcm)\n", currentZone, d);

    if (lifeState == LIFE_AWAKE) {
      if (currentZone == ZONE_PERSONAL) {
        applyMoodFromString("angry");
        // Head shake via confused anim
        roboEyes.anim_confused();
      } else if (currentZone == ZONE_DISCOMFORT) {
        applyMoodFromString("tired");
        roboEyes.setPosition(W);
      } else if (currentZone == ZONE_PRESENT || currentZone == ZONE_NONE) {
        roboEyes.setPosition(DEFAULT);
        if (currentMood == M_ANGRY || currentMood == M_TIRED) {
          applyMoodFromString("neutral");
        }
      }
    }
  }

  // ---------- Life state ----------
  switch (lifeState) {
    case LIFE_AWAKE:
      if (now - lastPresenceMs >= ABSENCE_TO_SLEEP_MS) {
        Serial.println("[Life] -> GOING TO SLEEP");
        lifeState = LIFE_GOING_TO_SLEEP;
        lifeStateEnterMs = now;
        applyMoodFromString("tired");
      }
      break;

    case LIFE_GOING_TO_SLEEP:
      if (d < PRESENCE) {
        // user came back during transition
        lifeState = LIFE_AWAKE;
        lastPresenceMs = now;
        applyMoodFromString("neutral");
        break;
      }
      if (now - lifeStateEnterMs >= SLEEP_PHASE1_MS) {
        Serial.println("[Life] -> SLEEPING");
        lifeState = LIFE_SLEEPING;
        lifeStateEnterMs = now;
        // reset Z bubbles
        for (int i = 0; i < 3; i++) zbubs[i].alive = false;
        lastZSpawnMs = now - ZZZ_SPAWN_MS; // immediate first Z
      }
      break;

    case LIFE_SLEEPING:
      if (d < PRESENCE) {
        Serial.println("[Life] WAKE UP");
        lifeState = LIFE_AWAKE;
        lastPresenceMs = now;
        appMode = MODE_FACE;
        applyMoodFromString("neutral");
        roboEyes.anim_confused(); // surprised
        postEndpoint("/api/arrived");
      }
      break;
  }
}

// ============================================================
//                    BACKEND COMMS
// ============================================================
void handleStatePoll(uint32_t now) {
  if (!wifiUp || WiFi.status() != WL_CONNECTED) return;
  if (now - lastStatePollMs < STATE_POLL_MS) return;
  lastStatePollMs = now;

  HTTPClient http;
  String url = String(BACKEND_BASE) + "/api/state";
  if (!http.begin(url)) return;
  http.setTimeout(4000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    parseAndApplyResponse(body);
  }
  http.end();
}

void postEndpoint(const char* path) {
  if (!wifiUp || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(BACKEND_BASE) + path;
  if (!http.begin(url)) return;
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST("{}");
  if (code == 200) {
    String body = http.getString();
    parseAndApplyResponse(body);
  }
  http.end();
}

void parseAndApplyResponse(const String& body) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[JSON] parse err: %s\n", err.c_str());
    return;
  }
  if (doc["mood"].is<const char*>()) {
    String mood = doc["mood"].as<String>();
    if (mood.length()) applyMoodFromString(mood);
  }
  if (doc["message"].is<const char*>()) {
    String msg = doc["message"].as<String>();
    if (msg.length()) showMessageOverlay(msg);
  }
}

// ============================================================
//                       WEATHER
// ============================================================
void handleWeatherTimer(uint32_t now) {
  if (now - lastWeatherMs >= WEATHER_INTERVAL_MS) {
    fetchWeather();
    lastWeatherMs = now;
  }
}

void fetchWeather() {
  if (!wifiUp || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://api.weatherapi.com/v1/current.json?key=" WEATHER_KEY
               "&q=" WEATHER_CITY "&aqi=no";
  if (!http.begin(url)) return;
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      weatherTempC = doc["current"]["temp_c"] | 0.0f;
      const char* cond = doc["current"]["condition"]["text"] | "--";
      weatherCondition = String(cond);
      weatherValid = true;
      Serial.printf("[Weather] %.1fC %s\n", weatherTempC, weatherCondition.c_str());
    }
  }
  http.end();
}

// ============================================================
//                       OVERLAYS
// ============================================================
void showMessageOverlay(const String& msg) {
  msgOverlayText = msg;
  msgOverlayActive = true;
  msgOverlayStartMs = millis();
}

// ============================================================
//                       MOOD MAPPING
// ============================================================
void applyMoodFromString(String mood) {
  mood.toLowerCase();
  MoodId target = M_NEUTRAL;

  if      (mood == "happy")     target = M_HAPPY;
  else if (mood == "angry")     target = M_ANGRY;
  else if (mood == "tired")     target = M_TIRED;
  else if (mood == "sad")       target = M_SAD;
  else if (mood == "irritated") target = M_IRRITATED;
  else if (mood == "excited")   target = M_EXCITED;
  else if (mood == "surprised" || mood == "curious") target = M_SURPRISED;
  else if (mood == "nervous")   target = M_NERVOUS;
  else if (mood == "love")      target = M_LOVE;
  else                          target = M_NEUTRAL;

  if (target == currentMood) return;
  currentMood = target;

  // Trigger 500ms cut overlay
  moodOverlayId = target;
  moodOverlayActive = true;
  moodOverlayStartMs = millis();

  // Apply RoboEyes parameters (will become visible after overlay)
  applyMoodToRoboEyes(target);
}

void applyMoodToRoboEyes(MoodId m) {
  // Reset to a known baseline each call
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);
  roboEyes.setPosition(DEFAULT);

  switch (m) {
    case M_HAPPY:
      roboEyes.setMood(HAPPY);
      break;
    case M_ANGRY:
      roboEyes.setMood(ANGRY);
      break;
    case M_TIRED:
      roboEyes.setMood(TIRED);
      break;
    case M_SAD:
      roboEyes.setMood(TIRED);
      roboEyes.setPosition(SW);
      break;
    case M_IRRITATED:
      roboEyes.setMood(ANGRY);
      roboEyes.anim_confused();
      break;
    case M_EXCITED:
      roboEyes.setMood(HAPPY);
      roboEyes.anim_laugh();
      break;
    case M_SURPRISED:
      roboEyes.setMood(DEFAULT);
      roboEyes.anim_confused();
      break;
    case M_NERVOUS:
      roboEyes.setMood(TIRED);
      roboEyes.setAutoblinker(ON, 1, 1);
      roboEyes.setIdleMode(ON, 1, 1);
      break;
    case M_LOVE:
      roboEyes.setMood(HAPPY);
      roboEyes.setAutoblinker(ON, 5, 2);
      roboEyes.setIdleMode(OFF, 0, 0);
      break;
    case M_NEUTRAL:
    default:
      roboEyes.setMood(DEFAULT);
      break;
  }
}

// ============================================================
//                       IDLE ANIMATIONS
// ============================================================
void handleIdleAnim(uint32_t now) {
  if (lifeState != LIFE_AWAKE) return;
  if (appMode != MODE_FACE) return;
  if (currentZone == ZONE_PERSONAL || currentZone == ZONE_DISCOMFORT) return;
  if (!(currentMood == M_NEUTRAL || currentMood == M_HAPPY)) return;
  if (moodOverlayActive || msgOverlayActive) return;

  if (now - lastIdleAnimMs < nextIdleDelay) return;
  lastIdleAnimMs = now;
  nextIdleDelay = random(15000, 40001);

  int pick = random(0, 5);
  switch (pick) {
    case 0: roboEyes.anim_laugh(); break;
    case 1: roboEyes.anim_confused(); break;
    case 2: roboEyes.setPosition(E);  break;
    case 3: roboEyes.setPosition(W);  break;
    case 4: roboEyes.setPosition(N);  break;
  }
}

// ============================================================
//                       RENDER PIPELINE
// ============================================================
void render(uint32_t now) {
  // PRIORITY 1: Message overlay (pauses everything)
  if (msgOverlayActive) {
    if (now - msgOverlayStartMs >= MSG_OVERLAY_MS) {
      msgOverlayActive = false;
    } else {
      drawMessageOverlay();
      return;
    }
  }

  // PRIORITY 2: Sleep mode rendering
  if (lifeState == LIFE_SLEEPING) {
    if (now - lastSleepFrameMs >= SLEEP_FRAME_MS) {
      lastSleepFrameMs = now;
      drawSleepAnimation();
    }
    return;
  }

  // PRIORITY 3: Mood transition cut (500ms)
  if (moodOverlayActive) {
    if (now - moodOverlayStartMs >= MOOD_CUT_MS) {
      moodOverlayActive = false;
      display.clearDisplay();
      display.display();
    } else {
      drawMoodCut(moodOverlayId);
      return;
    }
  }

  PRIORITY 4: Mode rendering
  switch (appMode) {
    case MODE_FACE:
      // RoboEyes drives the screen each loop iteration
      roboEyes.update();
      break;
    case MODE_CLOCK:
      drawClock();
      break;
    case MODE_DATE:
      drawDate();
      break;
    case MODE_WEATHER:
      drawWeather();
      break;
    case MODE_STOPWATCH:
      drawStopwatch();
      break;
    default: break;
  }
}

// ---------- Message overlay ----------
void drawMessageOverlay() {
  display.clearDisplay();
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, WHITE);
  display.drawRect(2, 2, SCREEN_WIDTH - 4, SCREEN_HEIGHT - 4, WHITE);

  display.setTextSize(1);
  display.setTextColor(WHITE);

  // word-wrap at ~20 chars
  const int maxChars = 20;
  String txt = msgOverlayText;
  int y = 10;
  while (txt.length() > 0 && y < SCREEN_HEIGHT - 8) {
    int cut = txt.length();
    if (cut > maxChars) {
      cut = maxChars;
      int sp = txt.lastIndexOf(' ', cut);
      if (sp > 4) cut = sp;
    }
    String line = txt.substring(0, cut);
    line.trim();
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(line, 0, y, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - (int)w) / 2;
    if (x < 4) x = 4;
    display.setCursor(x, y);
    display.print(line);
    y += 10;
    txt = txt.substring(cut);
    txt.trim();
  }
  display.display();
}

// ---------- Mood cut: hand-drawn pixel face per mood ----------
void drawMoodCut(MoodId m) {
  display.clearDisplay();
  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int eyeY = cy - 8;
  int lx = cx - 22;
  int rx = cx + 22;

  switch (m) {
    case M_HAPPY:
    case M_EXCITED: {
      // Arc-shaped happy eyes: ^ ^
      for (int i = -8; i <= 8; i++) {
        int yy = eyeY - (8 - abs(i)) / 2;
        display.drawPixel(lx + i, yy, WHITE);
        display.drawPixel(rx + i, yy, WHITE);
      }
      // Smile
      for (int i = -14; i <= 14; i++) {
        int yy = cy + 14 + (14 - abs(i)) / 3;
        display.drawPixel(cx + i, yy, WHITE);
      }
    } break;

    case M_ANGRY:
    case M_IRRITATED: {
      // Slanted brows
      for (int i = 0; i < 14; i++) {
        display.drawPixel(lx - 7 + i, eyeY - 8 + i / 3, WHITE);
        display.drawPixel(rx - 7 + i, eyeY - 8 + (13 - i) / 3, WHITE);
      }
      // Filled eyes
      display.fillCircle(lx, eyeY, 4, WHITE);
      display.fillCircle(rx, eyeY, 4, WHITE);
      // Frown
      for (int i = -12; i <= 12; i++) {
        int yy = cy + 18 - (12 - abs(i)) / 3;
        display.drawPixel(cx + i, yy, WHITE);
      }
    } break;

    case M_TIRED:
    case M_SAD:
    case M_NERVOUS: {
      // Half-closed eyes (top half)
      display.drawCircle(lx, eyeY, 6, WHITE);
      display.drawCircle(rx, eyeY, 6, WHITE);
      display.fillRect(lx - 8, eyeY, 17, 8, BLACK);
      display.fillRect(rx - 8, eyeY, 17, 8, BLACK);
      display.drawLine(lx - 6, eyeY, lx + 6, eyeY, WHITE);
      display.drawLine(rx - 6, eyeY, rx + 6, eyeY, WHITE);
      // Sad mouth
      for (int i = -10; i <= 10; i++) {
        int yy = cy + 18 - (10 - abs(i)) / 3;
        display.drawPixel(cx + i, yy, WHITE);
      }
    } break;

    case M_SURPRISED: {
      // Wide circles
      display.drawCircle(lx, eyeY, 7, WHITE);
      display.drawCircle(rx, eyeY, 7, WHITE);
      display.fillCircle(lx, eyeY, 2, WHITE);
      display.fillCircle(rx, eyeY, 2, WHITE);
      // O mouth
      display.drawCircle(cx, cy + 16, 4, WHITE);
    } break;

    case M_LOVE: {
      // Heart-shaped eyes (approx)
      drawTinyHeart(lx, eyeY);
      drawTinyHeart(rx, eyeY);
      // Blush
      display.fillCircle(lx - 10, cy + 8, 2, WHITE);
      display.fillCircle(rx + 10, cy + 8, 2, WHITE);
      // Smile
      for (int i = -10; i <= 10; i++) {
        int yy = cy + 14 + (10 - abs(i)) / 3;
        display.drawPixel(cx + i, yy, WHITE);
      }
    } break;

    case M_NEUTRAL:
    default: {
      display.fillCircle(lx, eyeY, 4, WHITE);
      display.fillCircle(rx, eyeY, 4, WHITE);
      display.drawLine(cx - 10, cy + 16, cx + 10, cy + 16, WHITE);
    } break;
  }
  display.display();
}

void drawTinyHeart(int cx, int cy) {
  display.fillCircle(cx - 3, cy - 1, 3, WHITE);
  display.fillCircle(cx + 3, cy - 1, 3, WHITE);
  display.fillTriangle(cx - 6, cy, cx + 6, cy, cx, cy + 6, WHITE);
}

// ---------- Clock ----------
void drawClock() {
  display.clearDisplay();
  display.setTextColor(WHITE);

  struct tm ti;
  if (timeSynced && getLocalTime(&ti, 50)) {
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &ti);
    display.setTextSize(3);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 12);
    display.print(buf);

    char sec[8];
    strftime(sec, sizeof(sec), ":%S", &ti);
    display.setTextSize(1);
    display.getTextBounds(sec, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 48);
    display.print(sec);
  } else {
    display.setTextSize(1);
    display.setCursor(8, 28);
    display.print("Syncing time...");
  }
  display.display();
}

// ---------- Date ----------
void drawDate() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  struct tm ti;
  if (timeSynced && getLocalTime(&ti, 50)) {
    char d1[16], d2[24];
    strftime(d1, sizeof(d1), "%a", &ti);
    strftime(d2, sizeof(d2), "%d %b %Y", &ti);

    int16_t x1, y1; uint16_t w, h;
    display.setTextSize(2);
    display.getTextBounds(d1, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 8);
    display.print(d1);

    display.setTextSize(1);
    display.getTextBounds(d2, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 40);
    display.print(d2);
  } else {
    display.setTextSize(1);
    display.setCursor(8, 28);
    display.print("Syncing date...");
  }
  display.display();
}

// ---------- Weather ----------
void drawWeather() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Nagpur");

  if (weatherValid) {
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%.1f C", weatherTempC);
    display.setTextSize(2);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(tbuf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 18);
    display.print(tbuf);

    display.setTextSize(1);
    display.getTextBounds(weatherCondition, 0, 0, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - (int)w) / 2;
    if (x < 0) x = 0;
    display.setCursor(x, 48);
    display.print(weatherCondition);
  } else {
    display.setCursor(8, 28);
    display.print("Loading weather...");
  }
  display.display();
}

// ---------- Stopwatch ----------
void drawStopwatch() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Stopwatch");
  display.setCursor(80, 0);
  switch (swState) {
    case SW_RESET:   display.print("READY"); break;
    case SW_RUNNING: display.print("RUN");   break;
    case SW_PAUSED:  display.print("PAUSE"); break;
  }

  uint32_t e = stopwatchElapsed();
  uint32_t mm = (e / 60000UL);
  uint32_t ss = (e / 1000UL) % 60UL;
  uint32_t cs = (e / 10UL) % 100UL;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu",
           (unsigned long)mm, (unsigned long)ss, (unsigned long)cs);

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 22);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("Long-press: cycle");
  display.display();
  }

// ============================================================
//                  CUSTOM SLEEP ANIMATION
// ============================================================
void drawSleepAnimation() {
  display.clearDisplay();

  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int eyeY = cy - 4;
  int lx = cx - 24;
  int rx = cx + 24;

  // Half-lidded droopy eyes (mask bottom half of circle)
  int eyeR = 10;
  display.drawCircle(lx, eyeY, eyeR, WHITE);
  display.drawCircle(rx, eyeY, eyeR, WHITE);
  display.fillRect(lx - eyeR - 1, eyeY - eyeR - 1,
                   2 * eyeR + 3, eyeR + 1, BLACK);
  display.fillRect(rx - eyeR - 1, eyeY - eyeR - 1,
                   2 * eyeR + 3, eyeR + 1, BLACK);
  // Lid line
  display.drawLine(lx - eyeR, eyeY, lx + eyeR, eyeY, WHITE);
  display.drawLine(rx - eyeR, eyeY, rx + eyeR, eyeY, WHITE);
  // Subtle mouth
  display.drawLine(cx - 6, cy + 14, cx + 6, cy + 14, WHITE);

  // ZZZ bubble logic: every 3s spawn a new bubble, each advances stage every 700ms
  uint32_t now = millis();
  if (now - lastZSpawnMs >= ZZZ_SPAWN_MS) {
    lastZSpawnMs = now;
    // find a free slot
    for (int i = 0; i < 3; i++) {
      if (!zbubs[i].alive) {
        zbubs[i].alive = true;
        zbubs[i].bornMs = now;
        zbubs[i].stage = 0;
        break;
      }
    }
  }

  for (int i = 0; i < 3; i++) {
    if (!zbubs[i].alive) continue;
    uint32_t age = now - zbubs[i].bornMs;
    uint8_t st = age / ZZZ_STAGE_MS;
    if (st > 2) {
      zbubs[i].alive = false;
      continue;
    }
    zbubs[i].stage = st;
    drawZ(st);
  }

  display.display();
}

void drawZ(uint8_t stage) {
  // Stage 0: small lower-right; Stage 1: medium higher; Stage 2: large highest
  display.setTextColor(WHITE);
  int x, y;
  uint8_t size;
  switch (stage) {
    case 0: x = 100; y = 40; size = 1; break;
    case 1: x = 104; y = 22; size = 2; break;
    case 2: x = 108; y = 4;  size = 3; break;
    default: return;
  }
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print('z');
}


