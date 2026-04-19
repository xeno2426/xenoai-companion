/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║         XenoAI Desk Companion — ESP32-S3 Firmware v2     ║
 * ║         Board: ESP32-S3-WROOM-1 N8R8                     ║
 * ║         Built by Xeno                                    ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * COMPONENTS:
 *   - ESP32-S3 N8R8 Dev Board
 *   - OLED 1.3" SSD1306 (I2C)
 *   - TTP223 Capacitive Touch Sensor
 *   - HC-SR04 Ultrasonic Distance Sensor
 *
 * LIBRARIES (install via Arduino Library Manager):
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - ArduinoJson
 *
 * ARDUINO IDE SETTINGS:
 *   Board:         ESP32S3 Dev Module
 *   Flash Size:    8MB (64Mb)       ← N8R8 has 8MB
 *   PSRAM:         OPI PSRAM        ← Enable this!
 *   Upload Speed:  921600
 *   USB CDC:       Enabled
 */

// ─── LIBRARIES ───────────────────────────────────────────────────────────────
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── YOUR CONFIG — CHANGE THESE ──────────────────────────────────────────────
#define WIFI_SSID    "Redmi 9i"
#define WIFI_PASS    "strawberry"
#define XENOAI_URL   "https://xenoai-companion.onrender.com"
#define OTA_PASSWORD "xenoai123"

// ─── PIN DEFINITIONS ─────────────────────────────────────────────────────────

// OLED (I2C)
#define OLED_SDA   8
#define OLED_SCL   9
#define OLED_ADDR  0x3C

// TTP223 Touch Sensor
#define TOUCH_PIN  4

// HC-SR04 Ultrasonic
#define TRIG_PIN   5
#define ECHO_PIN   6

// Onboard LED
#define LED_PIN    2

// Presence threshold — person detected if closer than this (cm)
#define PRESENCE_CM 80

// ─── OLED ────────────────────────────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ─── STATE ───────────────────────────────────────────────────────────────────
String currentMood    = "neutral";
String currentMessage = "";
bool   msgShowing     = false;

unsigned long msgShownAt   = 0;
unsigned long lastSonarAt  = 0;
unsigned long lastStateAt  = 0;
unsigned long lastTouchAt  = 0;

bool lastPresence = false;  // was someone detected last cycle?

// Timings (ms)
#define SONAR_INTERVAL   2000   // Check distance every 2s
#define STATE_INTERVAL   6000   // Poll idle state every 6s
#define MESSAGE_DURATION 4000   // Show message for 4s

// ─── TOUCH INTERRUPT ─────────────────────────────────────────────────────────
volatile bool touchFlag = false;
void IRAM_ATTR onTouch() { touchFlag = true; }

// ─── ULTRASONIC DISTANCE ─────────────────────────────────────────────────────
float readDistanceCM() {
  // Send 10µs pulse on TRIG
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Read ECHO pulse duration (timeout 30ms = ~5m max range)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999.0;  // timeout = nothing detected

  // Convert to cm: sound travels 343m/s = 0.0343 cm/µs
  // Divide by 2 because pulse travels to object AND back
  return (duration * 0.0343) / 2.0;
}

// ─── WIFI + OTA ──────────────────────────────────────────────────────────────
void connectWiFi() {
  display.clearDisplay();
  drawTextCentered(20, "Connecting WiFi");
  drawTextCentered(35, WIFI_SSID);
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi: " + WiFi.localIP().toString());
    display.clearDisplay();
    drawTextCentered(20, "WiFi Connected!");
    drawTextCentered(35, WiFi.localIP().toString());
    display.display();
    delay(1500);
  } else {
    display.clearDisplay();
    drawTextCentered(25, "WiFi Failed :(");
    display.display();
    delay(2000);
  }
}

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
    display.fillRect(10, 45, (108 * pct / 100), 8, WHITE);
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
String httpPost(String endpoint, String body) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(XENOAI_URL + endpoint);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return resp;
}

String httpGet(String endpoint) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  http.begin(XENOAI_URL + endpoint);
  http.setTimeout(8000);
  int code = http.GET();
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return resp;
}

// ─── JSON PARSER ─────────────────────────────────────────────────────────────
void parseResponse(String json) {
  if (json.isEmpty()) return;
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;

  if (doc.containsKey("mood"))
    currentMood = doc["mood"].as<String>();

  if (doc.containsKey("message")) {
    String msg = doc["message"].as<String>();
    if (msg.length() > 0) {
      currentMessage = msg;
      msgShowing     = true;
      msgShownAt     = millis();
    }
  }
}

// ─── OLED HELPERS ────────────────────────────────────────────────────────────
void drawText(int x, int y, String text) {
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(x, y);
  display.println(text);
}

void drawTextCentered(int y, String text) {
  int x = max(0, (SCREEN_W - (int)(text.length() * 6)) / 2);
  drawText(x, y, text);
}

void drawTextWrapped(String text, int y) {
  int charsPerLine = 21;
  for (int i = 0; i < (int)text.length(); i += charsPerLine) {
    String line = text.substring(i, min((int)text.length(), i + charsPerLine));
    drawTextCentered(y, line);
    y += 10;
    if (y > 54) break;
  }
}

// ─── FACE EXPRESSIONS ────────────────────────────────────────────────────────

void drawFaceNeutral() {
  display.fillCircle(44, 24, 5, WHITE);
  display.fillCircle(84, 24, 5, WHITE);
  display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 2, BLACK);
  display.drawLine(49, 45, 79, 45, WHITE);
}

void drawFaceHappy() {
  // Happy squint eyes
  display.drawLine(38, 24, 44, 19, WHITE);
  display.drawLine(44, 19, 50, 24, WHITE);
  display.drawLine(78, 24, 84, 19, WHITE);
  display.drawLine(84, 19, 90, 24, WHITE);
  // Smile — center lower than edges = U-shape
  for (int i = -20; i <= 20; i++) {
    display.drawPixel(64 + i, 50 - (i * i) / 30, WHITE);
  }
}

void drawFaceExcited() {
  // Star eyes
  display.drawLine(38, 24, 50, 24, WHITE);
  display.drawLine(44, 18, 44, 30, WHITE);
  display.drawLine(39, 19, 49, 29, WHITE);
  display.drawLine(49, 19, 39, 29, WHITE);
  display.drawLine(78, 24, 90, 24, WHITE);
  display.drawLine(84, 18, 84, 30, WHITE);
  display.drawLine(79, 19, 89, 29, WHITE);
  display.drawLine(89, 19, 79, 29, WHITE);
  // Big grin — two lines, center lower than edges
  for (int i = -25; i <= 25; i++) {
    display.drawPixel(64 + i, 50 - (i * i) / 25, WHITE);
    display.drawPixel(64 + i, 51 - (i * i) / 25, WHITE);
  }
}

void drawFaceSurprised() {
  display.drawCircle(44, 24, 7, WHITE);
  display.drawCircle(84, 24, 7, WHITE);
  display.fillCircle(44, 24, 3, WHITE);
  display.fillCircle(84, 24, 3, WHITE);
  display.drawCircle(64, 46, 7, WHITE);
}

void drawFaceSleepy() {
  // Half closed eyes
  display.fillCircle(44, 26, 5, WHITE);
  display.fillCircle(84, 26, 5, WHITE);
  display.fillRect(38, 21, 13, 5, BLACK);
  display.fillRect(78, 21, 13, 5, BLACK);
  display.drawLine(52, 46, 76, 46, WHITE);
  // ZZZs
  display.setCursor(90, 10); display.print("z");
  display.setCursor(98, 4);  display.print("z");
  display.setCursor(106, 0); display.print("z");
}

void drawFaceSad() {
  display.fillCircle(44, 26, 5, WHITE);
  display.fillCircle(84, 26, 5, WHITE);
  display.fillCircle(44, 26, 2, BLACK);
  display.fillCircle(84, 26, 2, BLACK);
  // Tears
  display.drawLine(48, 30, 50, 38, WHITE);
  display.drawLine(88, 30, 90, 38, WHITE);
  // Frown — center higher than edges = arch-shape
  for (int i = -20; i <= 20; i++) {
    display.drawPixel(64 + i, 42 + (i * i) / 30, WHITE);
  }
}

void drawFaceCurious() {
  display.drawLine(37, 18, 51, 14, WHITE);  // raised brow
  display.drawLine(77, 14, 91, 14, WHITE);  // flat brow
  display.fillCircle(44, 24, 5, WHITE);
  display.fillCircle(84, 24, 5, WHITE);
  display.fillCircle(44, 24, 2, BLACK);
  display.fillCircle(84, 24, 2, BLACK);
  for (int i = -15; i <= 15; i++) {
    display.drawPixel(64 + i, 48 - (i * i) / 35, WHITE);
  }
  display.setCursor(110, 10); display.print("?");
}

void drawCurrentFace() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  if      (currentMood == "happy")     drawFaceHappy();
  else if (currentMood == "excited")   drawFaceExcited();
  else if (currentMood == "surprised") drawFaceSurprised();
  else if (currentMood == "sleepy")    drawFaceSleepy();
  else if (currentMood == "sad")       drawFaceSad();
  else if (currentMood == "curious")   drawFaceCurious();
  else                                  drawFaceNeutral();

  drawTextCentered(56, currentMood);
  display.display();
}

void drawMessage() {
  display.clearDisplay();
  display.drawLine(0, 10, 128, 10, WHITE);
  drawTextCentered(1, "XenoAI");
  display.drawLine(0, 11, 128, 11, WHITE);
  drawTextWrapped(currentMessage, 16);
  display.display();
}

// ─── API CALLS ───────────────────────────────────────────────────────────────
void sendVision(bool faceDetected) {
  String body = "{\"face_hint\":" + String(faceDetected ? "true" : "false") + "}";
  String resp = httpPost("/api/vision", body);
  parseResponse(resp);
  Serial.println("Vision [" + String(faceDetected) + "]: " + resp.substring(0, 80));
}

void sendTouch() {
  Serial.println("Touch → /api/touch");
  String resp = httpPost("/api/touch", "{}");
  parseResponse(resp);
  Serial.println("Touch response: " + resp.substring(0, 80));
}

void pollState() {
  String resp = httpGet("/api/state");
  if (resp.length() > 0) parseResponse(resp);
}

// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n🤖 XenoAI Companion v2 booting...");

  // Pins
  pinMode(LED_PIN,  OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(TOUCH_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED failed!"); while (true);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

  // Boot screen
  drawTextCentered(20, "XenoAI v2");
  drawTextCentered(35, "Booting...");
  display.display();
  delay(1000);

  // WiFi + OTA
  connectWiFi();
  setupOTA();

  // Touch interrupt
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), onTouch, RISING);

  // Test ultrasonic
  float dist = readDistanceCM();
  Serial.printf("Ultrasonic test: %.1f cm\n", dist);

  // Ready!
  display.clearDisplay();
  drawTextCentered(20, "XenoAI Ready!");
  drawTextCentered(35, ":)");
  display.display();
  delay(1000);

  currentMood = "happy";
  drawCurrentFace();
  Serial.println("Boot complete!");
}

// ─── MAIN LOOP ───────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();

  unsigned long now = millis();

  // ── Touch event ──
  if (touchFlag) {
    touchFlag = false;
    if (now - lastTouchAt > 500) {  // 500ms debounce
      lastTouchAt = now;
      sendTouch();
    }
  }

  // ── Message display timer ──
  if (msgShowing) {
    drawMessage();
    if (now - msgShownAt > MESSAGE_DURATION) {
      msgShowing = false;
      drawCurrentFace();
    }
    return;  // Don't do other updates while showing message
  }

  // ── Ultrasonic presence check every 2s ──
  if (now - lastSonarAt > SONAR_INTERVAL) {
    lastSonarAt = now;

    float dist        = readDistanceCM();
    bool  presenceNow = (dist < PRESENCE_CM);

    Serial.printf("Distance: %.1f cm | Present: %s\n",
                  dist, presenceNow ? "YES" : "no");

    // Only send to backend if presence changed OR every 10th cycle
    static int sonarCycle = 0;
    sonarCycle++;
    if (presenceNow != lastPresence || sonarCycle >= 10) {
      sonarCycle    = 0;
      lastPresence  = presenceNow;
      sendVision(presenceNow);
      drawCurrentFace();
    }
  }

  // ── Idle state poll every 6s ──
  if (now - lastStateAt > STATE_INTERVAL) {
    lastStateAt = now;
    pollState();
    if (!msgShowing) drawCurrentFace();
  }

  delay(50);
}
