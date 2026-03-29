# AGENTS.md — XenoAI Desk Companion
> Single source of truth. Read fully before touching any file.
> Built from actual source code — not assumptions.

---

## 🧠 Project Identity

**Project:** XenoAI Desk Companion — a physical AI-powered desktop robot with mood, memory, and presence detection
**Developer:** Xeno (solo, Maharashtra, India)
**Status:** Firmware v2 written. Backend v1 deployed on Render. ESP32-S3 board not yet received.

---

## ⚙️ Hardware Stack (EXACT — confirmed from firmware)

| Component | Part | Notes |
|---|---|---|
| MCU | ESP32-S3-WROOM-1 N8R8 | 8MB Flash, 8MB OPI PSRAM. **Board not received yet.** |
| Display | OLED 1.3" SSD1306 | I2C. SDA=8, SCL=9, Addr=0x3C |
| Touch | TTP223 capacitive module | Digital OUT → GPIO4. Interrupt-driven (RISING). |
| Proximity | HC-SR04 ultrasonic | TRIG=5, ECHO=6. Presence threshold = 80cm |
| LED | Onboard | GPIO2 |
| Audio | ❌ NONE | No speaker, no mic. Do not suggest audio features. |
| Camera | ❌ NONE | Ultrasonic is the only "vision". Do not suggest camera. |

**Arduino IDE Settings:**
- Board: ESP32S3 Dev Module
- Flash Size: 8MB (64Mb)
- PSRAM: OPI PSRAM (enable!)
- Upload Speed: 921600
- USB CDC: Enabled

---

## 📌 Pin Map (confirmed from firmware)

```cpp
#define OLED_SDA   8
#define OLED_SCL   9
#define OLED_ADDR  0x3C
#define TOUCH_PIN  4     // TTP223 OUT → interrupt RISING
#define TRIG_PIN   5     // HC-SR04
#define ECHO_PIN   6
#define LED_PIN    2
#define PRESENCE_CM 80   // < 80cm = person present
```

---

## 🖥️ Backend Stack (confirmed from xenoai_companion.py)

| Detail | Value |
|---|---|
| Framework | Flask (Python) |
| Host | Render free tier — `https://xenoai-companion.onrender.com` |
| Cold start | ~30s — firmware handles with 8s HTTP timeout + silent fail |
| AI model | Groq `llama-3.3-70b-versatile` (only model, no Gemini) |
| Database | PostgreSQL via psycopg2-binary |
| Dependencies | `flask`, `psycopg2-binary`, `requests` |
| Entrypoint | `python xenoai_companion.py` (Procfile: `web: python xenoai_companion.py`) |
| OpenCV | Optional import — likely absent on Render free tier |

### API Routes

| Route | Method | Firmware use | Body | Returns |
|---|---|---|---|---|
| `/api/vision` | POST | Every 2s (on presence change or every 10 cycles) | `{"face_hint": true/false}` | `{mood, energy, expression, message, face_streak}` |
| `/api/touch` | POST | On TTP223 tap | `{}` | `{mood, energy, expression, message, was_sleeping}` |
| `/api/state` | GET | Every 6s idle poll | — | `{mood, energy, expression, message, time, date, face_streak, total_interactions}` |
| `/api/chat` | POST | Web dashboard only | `{message, chat_id?}` | `{reply, mood, expression, chat_id}` |
| `/` | GET | — | — | HTML dashboard with live mood + chat UI |
| `/health` | GET | — | — | `{status, cv2, db, groq, time}` |

> `/api/vision` is presence detection, NOT camera/face detection.
> `face_hint` = `(distance_cm < 80)`. Backend treats it as "person present".

---

## 🧩 Mood Engine (confirmed from compute_mood())

**Moods:** `neutral`, `happy`, `curious`, `sleepy`, `surprised`, `sad`, `excited`

**Transitions:**
- `face` event + present → `happy` (streak>2) → `excited` (streak>20)
- `face` event + absent → `neutral` or `sleepy` (energy<20)
- `touch` event → `excited` (or `surprised` if was sleeping)
- `idle` event → `sleepy` (energy<15) → `sad` (energy<40) → `curious` (idle>30min)

**Energy system:**
- Starts at 80, decays -1 per 5 min idle
- Touch: +15 | Face present: +5 | Capped 0–100

**State persisted in PostgreSQL** `companion_state` table. Falls back to in-memory dict if DB unavailable.

---

## 🎭 OLED Faces (confirmed from firmware)

Display: 128×64. Text helpers: `drawText()`, `drawTextCentered()`, `drawTextWrapped()` (21 chars/line max).

| Mood | Eyes | Mouth | Extra |
|---|---|---|---|
| neutral | filled circles + pupils | flat line | — |
| happy | V-shaped squint | parabolic smile | — |
| excited | ✱ star eyes | thick double-parabola grin | — |
| surprised | large open circles | open circle | — |
| sleepy | half-closed (rect mask) | flat line | ZZZ text |
| sad | droopy pupils | inverted parabola | tear lines |
| curious | raised left brow | slight smile | "?" text |

`drawCurrentFace()` renders face + mood label at y=56.
`drawMessage()` shows 4s message panel (blocks face updates while active).

---

## 📁 Repository Structure

```
/
├── xenoai_firmware_v2.ino       ← Arduino firmware (written, not yet flashed)
├── xenoai_companion.py          ← Flask backend (deployed on Render)
├── requirements.txt             ← flask, psycopg2-binary, requests
├── Procfile                     ← web: python xenoai_companion.py
└── AGENTS.md                    ← you are here
```

---

## ⚙️ Firmware Loop Architecture

Non-blocking `loop()` using `millis()` timers — no `delay()` in normal operation:

```
loop():
  ArduinoOTA.handle()
  if touchFlag → sendTouch()          ← ISR flag, 500ms debounce
  if msgShowing → drawMessage() → return  ← blocks other updates for 4s
  if now - lastSonarAt > 2000  → readDistanceCM() → sendVision() on change or every 10 cycles
  if now - lastStateAt > 6000  → pollState() → drawCurrentFace()
  delay(50)
```

Touch uses ISR (`IRAM_ATTR onTouch()`) + volatile flag pattern. Safe.

---

## 🌐 Environment Variables (Render dashboard)

```
GROQ_API_KEY=      ← required
DATABASE_URL=      ← Render sets if DB attached
SECRET_KEY=        ← Flask sessions (falls back to random UUID per boot = breaks sessions)
PORT=              ← Render sets automatically
```

---

## 🚫 Hard Constraints

1. **No audio** — zero components. Never suggest buzzer/speaker/I2S/PWM tones.
2. **No camera** — HC-SR04 only. Never suggest OV2640 or any camera module.
3. **No SD card** — state = PostgreSQL only.
4. **Render cold starts** — firmware already handles silently (timeout=8s, empty string = skip).
5. **Groq only** — `llama-3.3-70b-versatile`. No Gemini, no OpenAI.
6. **OLED limit** — 21 chars/line, max 5 lines before y>54 cutoff.
7. **AI reply cap** — `max_tokens: 100`. Messages must be ≤2 sentences.

---

## ✅ Task Intake Format

```
TASK: [one-line description]
FILE: [xenoai_firmware_v2.ino | xenoai_companion.py | other]
CONTEXT: [paste relevant existing function/block if editing]
CONSTRAINT: [e.g. "no new libs", "non-blocking only", "keep under 40 lines"]
```

---

## 🔋 Token Efficiency Rules

- **Diff mode (default for edits)** — show only changed functions ± 3 lines context.
- **Full file mode** — only if explicitly asked OR file is <80 lines.
- **Plan mode** — if task is ambiguous, output 3–5 bullet plan, wait for approval.
- **No preamble** — start with code or bullets directly.
- **No explanations** unless asked. Inline comments for non-obvious logic only.
- **No placeholders** — complete working code. No `// TODO`, no `pass`.
- **No alternatives** — unless current approach has a hard technical blocker.

---

## 🔄 Future TODOs (not current sprint)

- [ ] WiFi credentials → NVS (currently hardcoded `#define`)
- [ ] Cold-start wake-ping: GET /health on boot before first real call
- [ ] `/api/chat` accessible from firmware via serial → HTTP → OLED
- [ ] Energy bar visualization on OLED
- [ ] OTA trigger from web dashboard
