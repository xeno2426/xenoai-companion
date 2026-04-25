// ╔══════════════════════════════════════════════════════════════════╗
// ║              xenoai_firmware_v3.ino                             ║
// ║  ESP32  ·  OLED SSD1306  ·  Touch Sensor  ·  HC-SR04           ║
// ║  Sends a Twilio SMS when object < 15 cm is detected             ║
// ╚══════════════════════════════════════════════════════════════════╝

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ──────────────────────────────────────────────────────────────────
//  SECRETS  ← loaded from secrets.h  (never commit that file!)
// ──────────────────────────────────────────────────────────────────
#include "secrets.h"

// ──────────────────────────────────────────────────────────────────
//  HARDWARE PINS
// ──────────────────────────────────────────────────────────────────
#define I2C_SDA     8
#define I2C_SCL     9
#define TOUCH_PIN   4
#define TRIG_PIN    5
#define ECHO_PIN    6

// ──────────────────────────────────────────────────────────────────
//  OLED
// ──────────────────────────────────────────────────────────────────
#define SCREEN_W    128
#define SCREEN_H    64
#define OLED_RESET  -1
#define OLED_ADDR   0x3C

Adafruit_SH1106G display(SCREEN_W, SCREEN_H, &Wire, OLED_RESET);

// ──────────────────────────────────────────────────────────────────
//  ALERT LOGIC
// ──────────────────────────────────────────────────────────────────
#define DISTANCE_THRESHOLD_CM   15.0f   // Trigger if closer than this
#define ALERT_COOLDOWN_MS       30000UL // 30 s between SMS messages

unsigned long lastAlertTime = 0;
bool          alertSent     = false;

// ──────────────────────────────────────────────────────────────────
//  FUNCTION PROTOTYPES
// ──────────────────────────────────────────────────────────────────
void  connectWiFi();
void  initOLED();
void  initSensors();
float measureDistance();
bool  isTouched();
void  updateDisplay(float dist, bool touched, bool alert);
bool  sendSMSAlert(float dist);

// ══════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[XenoAI v3] Booting...");

  initSensors();
  initOLED();
  connectWiFi();

  Serial.println("[XenoAI v3] Ready.");
}

// ══════════════════════════════════════════════════════════════════
//  MAIN LOOP
// ══════════════════════════════════════════════════════════════════
void loop() {
  float dist    = measureDistance();
  bool  touched = isTouched();

  // ── Alert gate ──────────────────────────────────────────────────
  if (dist > 0.0f && dist <= DISTANCE_THRESHOLD_CM) {
    unsigned long now = millis();
    if (now - lastAlertTime >= ALERT_COOLDOWN_MS) {
      Serial.printf("[ALERT] Object at %.1f cm — sending SMS...\n", dist);
      alertSent     = sendSMSAlert(dist);
      lastAlertTime = now;
    }
  } else {
    // Reset visual flag once object moves away
    if (millis() - lastAlertTime > ALERT_COOLDOWN_MS) {
      alertSent = false;
    }
  }

  updateDisplay(dist, touched, alertSent);
  delay(200);
}

// ══════════════════════════════════════════════════════════════════
//  connectWiFi()
//  Attempts connection for up to 15 s; continues offline if it fails
// ══════════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.print("[WiFi] Connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected → " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Failed — running in offline mode.");
  }
}

// ══════════════════════════════════════════════════════════════════
//  initOLED()
//  Starts I²C on custom pins and boots SSD1306 with a splash screen
// ══════════════════════════════════════════════════════════════════
void initOLED() {
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("[OLED] Init FAILED — check wiring/address");
    return;
  }

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);

  // Splash screen
  display.setTextSize(2);
  display.setCursor(14, 10);
  display.println("XenoAI");
  display.setTextSize(1);
  display.setCursor(28, 32);
  display.println("Firmware v3");
  display.setCursor(20, 48);
  display.println("Initialising...");
  display.display();

  delay(1800);
  Serial.println("[OLED] Ready.");
}

// ══════════════════════════════════════════════════════════════════
//  initSensors()
//  Configures GPIO directions for HC-SR04 and touch sensor
// ══════════════════════════════════════════════════════════════════
void initSensors() {
  pinMode(TRIG_PIN,  OUTPUT);
  pinMode(ECHO_PIN,  INPUT);
  pinMode(TOUCH_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW); // Ensure TRIG starts LOW
  Serial.println("[Sensors] Pins initialised.");
}

// ══════════════════════════════════════════════════════════════════
//  measureDistance()
//  Returns distance in cm using HC-SR04 echo timing.
//  Returns -1.0 if no echo received (out of range / timeout).
//  ECHO is read through the 1k/2k voltage divider → safe for 3.3 V
// ══════════════════════════════════════════════════════════════════
float measureDistance() {
  // Send 10 µs trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo pulse width (timeout = 30 000 µs ≈ 5 m max range)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return -1.0f;

  // Distance = (duration × speed-of-sound) / 2
  // Speed of sound ≈ 0.0343 cm/µs at 20 °C
  return (duration * 0.0343f) / 2.0f;
}

// ══════════════════════════════════════════════════════════════════
//  isTouched()
//  Returns true when the capacitive touch sensor output is HIGH
// ══════════════════════════════════════════════════════════════════
bool isTouched() {
  return digitalRead(TOUCH_PIN) == HIGH;
}

// ══════════════════════════════════════════════════════════════════
//  updateDisplay()
//  Renders a 4-row status screen on the 128×64 OLED
// ══════════════════════════════════════════════════════════════════
void updateDisplay(float dist, bool touched, bool alert) {
  display.clearDisplay();
  display.setTextSize(1);

  // ── Row 0 : header ──────────────────────────────────────────────
  display.setCursor(16, 0);
  display.println("[ XenoAI  v3 ]");

  // ── Row 1 : distance ────────────────────────────────────────────
  display.setCursor(0, 16);
  display.print("Dist : ");
  if (dist < 0.0f) {
    display.println("---  cm");
  } else {
    display.printf("%.1f cm", dist);
  }

  // ── Row 2 : touch ───────────────────────────────────────────────
  display.setCursor(0, 28);
  display.print("Touch: ");
  display.println(touched ? "ACTIVE" : "---   ");

  // ── Row 3 : alert / status ──────────────────────────────────────
  display.setCursor(0, 42);
  if (alert) {
    display.println("> SMS SENT! <");
  } else if (dist > 0.0f && dist <= DISTANCE_THRESHOLD_CM) {
    display.println("!! OBJECT NEAR !!");
  } else {
    display.println("Status : OK");
  }

  // ── Row 4 : WiFi indicator ──────────────────────────────────────
  display.setCursor(0, 56);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi:[OK]" : "WiFi:[--]");

  display.display();
}

// ══════════════════════════════════════════════════════════════════
//  sendSMSAlert()
//  POSTs a Twilio message via HTTPS.
//  Returns true on HTTP 201 Created, false on any failure.
// ══════════════════════════════════════════════════════════════════
bool sendSMSAlert(float dist) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SMS] Skipped — no WiFi.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Accept any TLS cert (suitable for IoT devices)

  HTTPClient http;
  String url  = "https://api.twilio.com/2010-04-01/Accounts/";
  url += TWILIO_ACCOUNT_SID;
  url += "/Messages.json";

  if (!http.begin(client, url)) {
    Serial.println("[SMS] http.begin() failed.");
    return false;
  }

  http.setAuthorization(TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body  = "To=";
  body += ALERT_TO_NUMBER;
  body += "&From=";
  body += TWILIO_FROM_NUMBER;
  body += "&Body=XenoAI+Alert%3A+Object+detected+";
  body += String(dist, 1);
  body += "+cm+away%21+%5BxenoAI+v3%5D";

  int httpCode = http.POST(body);
  http.end();

  if (httpCode == 201) {
    Serial.printf("[SMS] Sent! Twilio responded: %d\n", httpCode);
    return true;
  } else {
    Serial.printf("[SMS] Failed. HTTP code: %d\n", httpCode);
    return false;
  }
}
