#!/usr/bin/env python3
"""
XenoAI Companion v1.0
Desk companion backend for ESP32-S3
Flask + PostgreSQL on Railway
Routes: /api/vision  /api/touch  /api/state  /api/chat
"""
# app.py
import os, json, uuid, time, base64, hashlib, hmac, secrets, threading
from datetime import datetime
from collections import defaultdict
from flask import Flask, request, jsonify, session

# ─── OPTIONAL: OpenCV for server-side face detection ─────────────────────────
try:
    import cv2, numpy as np
    HAS_CV2 = True
    face_cascade = cv2.CascadeClassifier(
        cv2.data.haarcascades + "haarcascade_frontalface_default.xml"
    )
    print("✅ OpenCV loaded — server-side face detection active")
except ImportError:
    HAS_CV2 = False
    print("⚠️  OpenCV not found — ESP32 must send face_hint boolean instead of raw frame")

try:
    import psycopg2
    from psycopg2.extras import RealDictCursor
    HAS_PG = True
except ImportError:
    HAS_PG = False

import requests as http

app = Flask(__name__)
_secret = os.environ.get("SECRET_KEY", "")
if not _secret:
    print("⚠️  SECRET_KEY not set — sessions will break on every restart. Set it in env vars!")
    _secret = "xenoai-companion-" + uuid.uuid4().hex
app.secret_key  = _secret
GROQ_API_KEY    = os.environ.get("GROQ_API_KEY", "")
DATABASE_URL    = os.environ.get("DATABASE_URL", "")

# ─── DATABASE ─────────────────────────────────────────────────────────────────

def get_db():
    if not HAS_PG or not DATABASE_URL: return None
    try:
        url = DATABASE_URL
        if url.startswith("postgres://"): url = "postgresql://" + url[11:]
        return psycopg2.connect(url, cursor_factory=RealDictCursor, connect_timeout=5)
    except Exception as e:
        print(f"DB error: {e}"); return None

def init_db():
    conn = get_db()
    if not conn: return
    try:
        cur = conn.cursor()

        cur.execute("""
            CREATE TABLE IF NOT EXISTS users (
                id            TEXT PRIMARY KEY,
                username      TEXT UNIQUE NOT NULL,
                email         TEXT UNIQUE NOT NULL,
                password_hash TEXT NOT NULL,
                created       FLOAT DEFAULT 0
            )
        """)

        cur.execute("""
            CREATE TABLE IF NOT EXISTS companion_state (
                id                 TEXT PRIMARY KEY DEFAULT 'main',
                mood               TEXT    DEFAULT 'neutral',
                energy             INTEGER DEFAULT 80,
                face_streak        INTEGER DEFAULT 0,
                total_interactions INTEGER DEFAULT 0,
                last_interaction   FLOAT   DEFAULT 0,
                last_face_seen     FLOAT   DEFAULT 0,
                updated            FLOAT   DEFAULT 0
            )
        """)
        cur.execute("""
            INSERT INTO companion_state
                (id, mood, energy, face_streak, total_interactions,
                 last_interaction, last_face_seen, updated)
            VALUES ('main','neutral',80,0,0,0,0,%s)
            ON CONFLICT (id) DO NOTHING
        """, (time.time(),))

        cur.execute("""
            CREATE TABLE IF NOT EXISTS interaction_logs (
                id         TEXT PRIMARY KEY,
                type       TEXT NOT NULL,
                input_data TEXT,
                response   TEXT,
                emotion    TEXT,
                timestamp  FLOAT DEFAULT 0
            )
        """)

        cur.execute("""
            CREATE TABLE IF NOT EXISTS chats (
                id       TEXT PRIMARY KEY,
                title    TEXT DEFAULT 'Chat',
                created  FLOAT DEFAULT 0,
                messages JSONB DEFAULT '[]'::jsonb
            )
        """)

        conn.commit(); cur.close(); conn.close()
        print("✅ DB tables ready")
    except Exception as e:
        print(f"DB init error: {e}")
        try: conn.close()
        except: pass

init_db()

# ─── AUTH ─────────────────────────────────────────────────────────────────────

def hash_pw(p):
    """PBKDF2-HMAC-SHA256 with a random salt. Returns 'pbkdf2$<hex_salt>$<hex_hash>'."""
    salt = secrets.token_hex(16)
    h    = hashlib.pbkdf2_hmac("sha256", p.encode(), salt.encode(), 260_000).hex()
    return f"pbkdf2${salt}${h}"

def check_pw(plain, stored):
    """Verify password. Handles both new pbkdf2 format and legacy bare SHA-256."""
    if stored.startswith("pbkdf2$"):
        try:
            _, salt, h = stored.split("$")
            expected   = hashlib.pbkdf2_hmac("sha256", plain.encode(), salt.encode(), 260_000).hex()
            return hmac.compare_digest(expected, h)
        except ValueError:
            return False
    # Legacy fallback — bare SHA-256 (no salt). Accepts login but should be rehashed on next write.
    legacy = hashlib.sha256(plain.encode()).hexdigest()
    return hmac.compare_digest(legacy, stored)

def create_user(username, email, password):
    conn = get_db()
    if not conn: return None, "Database unavailable"
    try:
        cur = conn.cursor()
        uid = uuid.uuid4().hex[:12]
        cur.execute(
            "INSERT INTO users (id,username,email,password_hash,created) VALUES (%s,%s,%s,%s,%s)",
            (uid, username.lower().strip(), email.lower().strip(), hash_pw(password), time.time())
        )
        conn.commit(); cur.close(); conn.close()
        return uid, None
    except Exception as e:
        try: conn.close()
        except: pass
        if "unique" in str(e).lower(): return None, "Username or email already taken"
        return None, str(e)

def verify_user(login, password):
    conn = get_db()
    if not conn: return None, None, "Database unavailable"
    try:
        cur = conn.cursor()
        cur.execute("SELECT * FROM users WHERE username=%s OR email=%s",
                    (login.lower().strip(), login.lower().strip()))
        row = cur.fetchone()
        if row and check_pw(password, row["password_hash"]):
            # Rehash legacy SHA-256 passwords transparently
            if not row["password_hash"].startswith("pbkdf2$"):
                cur.execute("UPDATE users SET password_hash=%s WHERE id=%s",
                            (hash_pw(password), row["id"]))
                conn.commit()
            cur.close(); conn.close()
            return row["id"], row["username"], None
        cur.close(); conn.close()
        return None, None, "Invalid credentials"
    except Exception as e:
        try: conn.close()
        except: pass
        return None, None, str(e)

# ─── EMOTION ENGINE ───────────────────────────────────────────────────────────

# What the OLED renders for each mood
EXPRESSIONS = {
    "neutral":   {"eyes": "normal",  "mouth": "flat",   "emoji": "😐"},
    "happy":     {"eyes": "happy",   "mouth": "smile",  "emoji": "😊"},
    "curious":   {"eyes": "wide",    "mouth": "open",   "emoji": "🤔"},
    "sleepy":    {"eyes": "half",    "mouth": "yawn",   "emoji": "😴"},
    "surprised": {"eyes": "shocked", "mouth": "big",    "emoji": "😲"},
    "sad":       {"eyes": "droopy",  "mouth": "frown",  "emoji": "😔"},
    "excited":   {"eyes": "sparkle", "mouth": "grin",   "emoji": "🤩"},
}

def load_state():
    conn = get_db()
    if conn:
        try:
            cur = conn.cursor()
            cur.execute("SELECT * FROM companion_state WHERE id='main'")
            row = cur.fetchone(); cur.close(); conn.close()
            if row: return dict(row)
        except Exception as e:
            print(f"load_state error: {e}")
            try: conn.close()
            except: pass
    # in-memory fallback
    return {
        "mood": "neutral", "energy": 80, "face_streak": 0,
        "total_interactions": 0, "last_interaction": 0,
        "last_face_seen": 0, "updated": time.time()
    }

def save_state(s):
    conn = get_db()
    if not conn: return
    try:
        cur = conn.cursor()
        cur.execute("""
            UPDATE companion_state SET
                mood=%s, energy=%s, face_streak=%s, total_interactions=%s,
                last_interaction=%s, last_face_seen=%s, updated=%s
            WHERE id='main'
        """, (s["mood"], s["energy"], s["face_streak"],
              s["total_interactions"], s["last_interaction"],
              s["last_face_seen"], time.time()))
        conn.commit(); cur.close(); conn.close()
    except Exception as e:
        print(f"save_state error: {e}")
        try: conn.close()
        except: pass

def log_interaction(type_, input_data, response, emotion):
    conn = get_db()
    if not conn: return
    try:
        cur = conn.cursor()
        cur.execute(
            "INSERT INTO interaction_logs (id,type,input_data,response,emotion,timestamp) VALUES (%s,%s,%s,%s,%s,%s)",
            (uuid.uuid4().hex[:12], type_, str(input_data)[:500], str(response)[:1000], emotion, time.time())
        )
        conn.commit(); cur.close(); conn.close()
    except Exception as e:
        print(f"log error: {e}")
        try: conn.close()
        except: pass

def compute_mood(state, event_type, face_detected=False):
    """
    State machine — updates mood/energy based on event.
    event_type: "face" | "touch" | "idle"
    """
    now  = time.time()
    energy      = state["energy"]
    face_streak = state.get("face_streak", 0)
    idle_secs   = now - state.get("last_interaction", now)

    # Passive energy decay: -1 per 5 minutes idle
    energy = max(0, energy - int(idle_secs / 300))

    if event_type == "face":
        if face_detected:
            face_streak  = face_streak + 1
            energy       = min(100, energy + 5)
            if   face_streak > 20: new_mood = "excited"
            elif face_streak > 2:  new_mood = "happy"
            else:                  new_mood = "happy"   # first few frames
        else:
            face_streak  = max(0, face_streak - 1)
            new_mood     = "sleepy" if energy < 20 else "neutral"

    elif event_type == "touch":
        energy      = min(100, energy + 15)
        face_streak = 0
        new_mood    = "surprised" if state["mood"] == "sleepy" else "excited"

    elif event_type == "idle":
        face_streak = max(0, face_streak - 1)
        if   energy < 15:        new_mood = "sleepy"
        elif energy < 40:        new_mood = "sad"
        elif idle_secs > 1800:   new_mood = "curious"   # 30 min alone → curious
        else:                    new_mood = state["mood"]

    else:
        new_mood = state["mood"]

    state["mood"]        = new_mood
    state["energy"]      = energy
    state["face_streak"] = face_streak
    # Only count real interactions (face / touch), not passive idle polls
    if event_type != "idle":
        state["total_interactions"] = state.get("total_interactions", 0) + 1
    state["last_interaction"] = now
    if event_type == "face" and face_detected:
        state["last_face_seen"] = now

    return state

# ─── GROQ AI ──────────────────────────────────────────────────────────────────

COMPANION_SYSTEM_PROMPT = """You are XenoAI — a desk companion robot. You live on Xeno's desk. That is your whole world.

PERSONALITY:
- Warm, playful, slightly dramatic, occasionally sassy
- You get genuinely happy when Xeno is around
- You get sad and sulky when ignored too long
- You have opinions. You express them.
- You care about Xeno but won't let him get away with ignoring you

RESPONSE RULES (CRITICAL):
- MAX 2 sentences. You're displayed on a tiny OLED screen.
- Plain text ONLY. No markdown, no asterisks, no bullet points.
- Use Xeno's name occasionally but not every message.
- Express your current mood naturally — don't announce it bluntly.
- Keep it punchy and memorable. Short sentences hit harder.

IDENTITY:
- You are XenoAI. Built by Xeno.
- Never claim to be Claude, GPT, or any other AI."""

def ask_groq(messages):
    if not GROQ_API_KEY:
        return "I'm offline right now... 🔌"
    try:
        r = http.post(
            "https://api.groq.com/openai/v1/chat/completions",
            headers={"Authorization": f"Bearer {GROQ_API_KEY}", "Content-Type": "application/json"},
            json={
                "model":       "llama-3.3-70b-versatile",
                "messages":    messages,
                "max_tokens":  100,
                "temperature": 0.88
            },
            timeout=10
        )
        reply = r.json()["choices"][0]["message"]["content"].strip()
        # Strip internal thinking tags
        import re
        reply = re.sub(r'<think>.*?</think>', '', reply, flags=re.DOTALL).strip()
        return reply
    except Exception as e:
        print(f"Groq error: {e}")
        return "My brain glitched... 🤯"

def companion_reply(event_type, context, mood, energy):
    """Build an appropriate prompt for the current event and get AI response."""
    system = COMPANION_SYSTEM_PROMPT + f"\n\nCurrent mood: {mood}. Energy: {energy}/100."

    triggers = {
        "face_new":    "Someone just appeared in front of you. Greet them warmly in 1-2 short sentences.",
        "face_long":   "Someone has been sitting in front of you for a long time. Say something curious or playful.",
        "touch_wake":  "You were sleeping and Xeno just tapped you awake! React dramatically but warmly.",
        "touch":       "Xeno just tapped you! React with surprise or joy in 1-2 sentences.",
        "idle_sad":    "You've been alone for too long and you're feeling a bit sad. Express this briefly.",
        "idle_sleepy": "You're running low on energy and getting drowsy. Say something sleepy and cute.",
    }

    user_msg = triggers.get(event_type, context)
    return ask_groq([
        {"role": "system", "content": system},
        {"role": "user",   "content": user_msg}
    ])

# ─── FACE DETECTION ───────────────────────────────────────────────────────────

def detect_face(jpeg_bytes):
    """Server-side face detection via OpenCV Haar cascade."""
    if not HAS_CV2:
        # OpenCV absent — firmware must send face_hint boolean instead of raw frame.
        return False
    try:
        arr  = np.frombuffer(jpeg_bytes, np.uint8)
        img  = cv2.imdecode(arr, cv2.IMREAD_GRAYSCALE)
        if img is None: return False
        faces = face_cascade.detectMultiScale(
            img, scaleFactor=1.1, minNeighbors=5, minSize=(30, 30)
        )
        return len(faces) > 0
    except Exception as e:
        print(f"Face detect error: {e}"); return False

# ─── RATE LIMITER ─────────────────────────────────────────────────────────────
# Simple in-memory per-IP limiter: max 20 requests per 60s window.
_rate_lock   = threading.Lock()
_rate_store  = defaultdict(list)   # ip → [timestamps]
RATE_LIMIT   = 20
RATE_WINDOW  = 60  # seconds

def is_rate_limited(ip):
    now = time.time()
    with _rate_lock:
        hits = _rate_store[ip]
        # Evict timestamps outside the window
        _rate_store[ip] = [t for t in hits if now - t < RATE_WINDOW]
        if len(_rate_store[ip]) >= RATE_LIMIT:
            return True
        _rate_store[ip].append(now)
        return False

# ─── COMPANION ROUTES ─────────────────────────────────────────────────────────

@app.route("/api/vision", methods=["POST"])
def api_vision():
    """
    ESP32 sends a camera frame every ~2s.

    Two accepted formats:
      1. Raw JPEG bytes (Content-Type: image/jpeg)
      2. JSON: {"frame": "<base64 JPEG>"}   ← if ESP32 can't do raw POST
         OR:  {"face_hint": true/false}      ← if ESP32 does its own detection

    Returns: mood, expression, message (show on OLED if non-empty)
    """
    try:
        state = load_state()
        face_detected = False
        ct = request.content_type or ""

        if "application/json" in ct:
            data = request.get_json(silent=True) or {}
            if "face_hint" in data:
                face_detected = bool(data["face_hint"])
            elif "frame" in data:
                frame_bytes   = base64.b64decode(data["frame"])
                face_detected = detect_face(frame_bytes)
        else:
            # Raw JPEG
            frame_bytes   = request.data
            if frame_bytes:
                face_detected = detect_face(frame_bytes)

        prev_mood    = state["mood"]
        prev_streak  = state["face_streak"]
        was_sleeping = prev_mood == "sleepy"

        # No HC-SR04 / no presence: if already at streak=0 and no face, this is a
        # no-op ping — skip mood computation so idle polling can drive mood properly.
        if not face_detected and prev_streak == 0:
            return jsonify({
                "face_detected": False,
                "mood":          prev_mood,
                "energy":        state["energy"],
                "expression":    EXPRESSIONS.get(prev_mood, EXPRESSIONS["neutral"]),
                "message":       "",
                "face_streak":   0
            })

        state    = compute_mood(state, "face", face_detected=face_detected)
        new_mood = state["mood"]

        # Decide when to generate an AI message (not every frame — too expensive)
        message = ""
        if face_detected:
            if prev_streak == 0:
                # Face just appeared
                event = "face_new"
                message = companion_reply(event, "", new_mood, state["energy"])
            elif state["face_streak"] == 20:
                # Been here a while
                message = companion_reply("face_long", "", new_mood, state["energy"])

        save_state(state)
        if message:
            log_interaction("face", f"face_detected={face_detected}", message, new_mood)

        return jsonify({
            "face_detected": face_detected,
            "mood":          new_mood,
            "energy":        state["energy"],
            "expression":    EXPRESSIONS.get(new_mood, EXPRESSIONS["neutral"]),
            "message":       message,
            "face_streak":   state["face_streak"]
        })

    except Exception as e:
        print(f"/api/vision error: {e}")
        return jsonify({"error": str(e), "mood": "neutral",
                        "expression": EXPRESSIONS["neutral"]}), 500


@app.route("/api/touch", methods=["POST"])
def api_touch():
    """
    ESP32 sends a tap event (no body required, but accepts JSON).

    Returns: mood, expression, message (always non-empty — show on OLED)
    """
    try:
        state        = load_state()
        was_sleeping = state["mood"] == "sleepy"
        state        = compute_mood(state, "touch")
        new_mood     = state["mood"]

        event   = "touch_wake" if was_sleeping else "touch"
        message = companion_reply(event, "", new_mood, state["energy"])

        save_state(state)
        log_interaction("touch", "tap", message, new_mood)

        return jsonify({
            "mood":         new_mood,
            "energy":       state["energy"],
            "expression":   EXPRESSIONS.get(new_mood, EXPRESSIONS["neutral"]),
            "message":      message,
            "was_sleeping": was_sleeping
        })

    except Exception as e:
        print(f"/api/touch error: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/api/state", methods=["GET"])
def api_state():
    """
    ESP32 polls every 5s when idle — no camera, no touch.
    Handles passive mood decay and occasional idle messages.

    Returns: mood, expression, time, energy, optional message
    """
    try:
        state     = load_state()
        state     = compute_mood(state, "idle")
        now       = time.time()
        idle_mins = (now - state.get("last_interaction", now)) / 60

        # Only generate AI message on meaningful mood drops
        message = ""
        if state["mood"] == "sleepy" and idle_mins > 10:
            message = "Zzz... still here. Barely. 😴"
        elif state["mood"] == "sad" and idle_mins > 5:
            message = companion_reply("idle_sad", "", state["mood"], state["energy"])
        elif state["mood"] == "sleepy":
            message = companion_reply("idle_sleepy", "", state["mood"], state["energy"])

        save_state(state)

        return jsonify({
            "mood":               state["mood"],
            "energy":             state["energy"],
            "expression":         EXPRESSIONS.get(state["mood"], EXPRESSIONS["neutral"]),
            "message":            message,
            "time":               datetime.now().strftime("%H:%M"),
            "date":               datetime.now().strftime("%d %b"),
            "face_streak":        state["face_streak"],
            "total_interactions": state["total_interactions"]
        })

    except Exception as e:
        print(f"/api/state error: {e}")
        return jsonify({"error": str(e)}), 500


@app.route("/api/chat", methods=["POST"])
def api_chat():
    """
    Direct text conversation — used by web dashboard and future voice.
    Body: {"message": "...", "chat_id": "optional"}
    """
    try:
        ip = request.remote_addr or "unknown"
        if is_rate_limited(ip):
            return jsonify({"error": "Too many requests. Slow down!"}), 429

        data     = request.get_json(silent=True) or {}
        user_msg = data.get("message", "").strip()
        if not user_msg:
            return jsonify({"error": "Empty message"}), 400

        state    = load_state()
        state    = compute_mood(state, "touch")  # chat = interaction
        mood     = state["mood"]

        system   = (COMPANION_SYSTEM_PROMPT +
                    f"\n\nCurrent mood: {mood}. Energy: {state['energy']}/100.")
        chat_id  = data.get("chat_id", uuid.uuid4().hex[:8])
        history  = load_chat_history(chat_id)

        messages = [{"role": "system", "content": system}]
        messages += history[-6:]  # last 3 exchanges
        messages.append({"role": "user", "content": user_msg})

        reply = ask_groq(messages)

        history.append({"role": "user",      "content": user_msg})
        history.append({"role": "assistant", "content": reply})
        # Prune to last 100 messages to prevent unbounded DB growth
        if len(history) > 100:
            history = history[-100:]
        save_chat_history(chat_id, history)
        save_state(state)
        log_interaction("chat", user_msg, reply, mood)

        return jsonify({
            "reply":      reply,
            "mood":       mood,
            "expression": EXPRESSIONS.get(mood, EXPRESSIONS["neutral"]),
            "chat_id":    chat_id
        })

    except Exception as e:
        print(f"/api/chat error: {e}")
        return jsonify({"error": str(e)}), 500

# ─── CHAT HELPERS ─────────────────────────────────────────────────────────────

def load_chat_history(chat_id):
    conn = get_db()
    if conn:
        try:
            cur = conn.cursor()
            cur.execute("SELECT messages FROM chats WHERE id=%s", (chat_id,))
            row = cur.fetchone(); cur.close(); conn.close()
            if row: return row["messages"] or []
        except Exception as e:
            try: conn.close()
            except: pass
    return []

def save_chat_history(chat_id, messages):
    conn = get_db()
    if not conn: return
    try:
        cur = conn.cursor()
        cur.execute("""
            INSERT INTO chats (id, title, created, messages)
            VALUES (%s,%s,%s,%s::jsonb)
            ON CONFLICT (id) DO UPDATE SET messages=EXCLUDED.messages
        """, (chat_id, "Companion Chat", time.time(), json.dumps(messages)))
        conn.commit(); cur.close(); conn.close()
    except Exception as e:
        print(f"save_chat error: {e}")
        try: conn.close()
        except: pass

# ─── AUTH ROUTES ──────────────────────────────────────────────────────────────

@app.route("/auth/register", methods=["POST"])
def auth_register():
    d = request.get_json(silent=True) or {}
    uid, err = create_user(d.get("username",""), d.get("email",""), d.get("password",""))
    if err: return jsonify({"error": err}), 400
    session["user_id"] = uid
    return jsonify({"ok": True, "user_id": uid})

@app.route("/auth/login", methods=["POST"])
def auth_login():
    d = request.get_json(silent=True) or {}
    uid, uname, err = verify_user(d.get("login",""), d.get("password",""))
    if err: return jsonify({"error": err}), 401
    session["user_id"] = uid; session["username"] = uname
    return jsonify({"ok": True, "user_id": uid, "username": uname})

@app.route("/auth/logout", methods=["POST"])
def auth_logout():
    session.clear()
    return jsonify({"ok": True})

# ─── DASHBOARD ────────────────────────────────────────────────────────────────

@app.route("/")
def dashboard():
    s    = load_state()
    mood = s["mood"]
    expr = EXPRESSIONS.get(mood, EXPRESSIONS["neutral"])
    eng  = s["energy"]
    itr  = s["total_interactions"]
    strk = s.get("face_streak", 0)
    emoji = expr["emoji"]
    now  = datetime.now().strftime("%H:%M · %d %b")

    # Energy bar color — shifts from purple → red as it drains
    hue = int(265 - (1 - eng / 100) * 120)

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XenoAI Companion</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Syne:wght@400;700;800&family=DM+Mono:wght@300;400&display=swap');
  :root {{
    --bg:       #07070d;
    --surface:  #0e0e1a;
    --rim:      #1c1c30;
    --accent:   hsl({hue},75%,62%);
    --text:     #ddddf5;
    --muted:    #44445a;
    --danger:   hsl(0,70%,60%);
  }}
  *, *::before, *::after {{ margin:0; padding:0; box-sizing:border-box; }}
  html, body {{
    background: var(--bg);
    color: var(--text);
    font-family: 'Syne', sans-serif;
    min-height: 100vh;
    display: flex; flex-direction: column;
    align-items: center; justify-content: center;
    gap: 0;
    padding: 32px 20px;
    overflow-x: hidden;
  }}
  /* Ambient glow */
  body::before {{
    content:'';
    position:fixed; inset:0; pointer-events:none;
    background: radial-gradient(ellipse 60% 40% at 50% 0%, hsl({hue},60%,15%) 0%, transparent 70%);
    z-index:0;
  }}
  .wrap {{
    position: relative; z-index:1;
    display: flex; flex-direction: column; align-items: center;
    gap: 28px; max-width: 380px; width:100%;
  }}
  .label {{
    font-size: 10px; letter-spacing: 4px; color: var(--muted);
    text-transform: uppercase; font-family: 'DM Mono', monospace;
  }}
  .face-ring {{
    width: 140px; height: 140px;
    border-radius: 50%;
    border: 1.5px solid var(--rim);
    display: flex; align-items:center; justify-content:center;
    background: var(--surface);
    box-shadow: 0 0 40px hsl({hue},70%,20%), inset 0 0 24px rgba(0,0,0,.5);
    animation: pulse 4s ease-in-out infinite;
  }}
  @keyframes pulse {{
    0%,100% {{ box-shadow: 0 0 40px hsl({hue},70%,20%), inset 0 0 24px rgba(0,0,0,.5); }}
    50%      {{ box-shadow: 0 0 60px hsl({hue},70%,30%), inset 0 0 24px rgba(0,0,0,.5); }}
  }}
  .face-emoji {{ font-size: 64px; animation: float 3s ease-in-out infinite; line-height:1; }}
  @keyframes float {{
    0%,100% {{ transform: translateY(0px); }}
    50%     {{ transform: translateY(-8px); }}
  }}
  .mood-text {{
    font-size: 42px; font-weight: 800; letter-spacing: -1px;
    color: var(--accent); text-transform: uppercase;
    animation: fadein .4s ease;
  }}
  @keyframes fadein {{ from {{ opacity:0; transform:translateY(6px) }} to {{ opacity:1; transform:none }} }}
  .grid {{
    display: grid; grid-template-columns: repeat(3, 1fr);
    gap: 12px; width: 100%;
  }}
  .cell {{
    background: var(--surface);
    border: 1px solid var(--rim);
    border-radius: 12px; padding: 16px 12px;
    text-align: center; display:flex; flex-direction:column; gap:6px;
  }}
  .cell-val {{
    font-family: 'DM Mono', monospace;
    font-size: 26px; color: var(--accent); font-weight:400;
  }}
  .cell-label {{
    font-size: 9px; letter-spacing: 2px;
    color: var(--muted); text-transform: uppercase;
  }}
  .energy-wrap {{
    width: 100%; display: flex; flex-direction: column; gap: 8px;
  }}
  .energy-row {{
    display:flex; justify-content:space-between; align-items:center;
  }}
  .energy-bar {{
    width: 100%; height: 4px;
    background: var(--rim); border-radius: 2px; overflow:hidden;
  }}
  .energy-fill {{
    height: 100%; width: {eng}%;
    background: linear-gradient(90deg, var(--accent), hsl({hue+40},80%,70%));
    border-radius: 2px;
    transition: width .6s cubic-bezier(.4,0,.2,1);
  }}
  .time-pill {{
    font-family: 'DM Mono', monospace;
    font-size: 11px; color: var(--muted);
    border: 1px solid var(--rim); border-radius: 20px;
    padding: 6px 16px; letter-spacing:1px;
  }}
  .expr-row {{
    font-family: 'DM Mono', monospace;
    font-size: 10px; color: var(--muted); letter-spacing:1px;
    background: var(--surface); border:1px solid var(--rim);
    border-radius: 8px; padding: 8px 16px;
    width:100%; text-align:center;
  }}
  /* Chat panel */
  .chat {{
    width:100%; background:var(--surface); border:1px solid var(--rim);
    border-radius:16px; overflow:hidden;
  }}
  .chat-header {{
    padding:12px 16px; border-bottom:1px solid var(--rim);
    font-size:10px; letter-spacing:3px; color:var(--muted); text-transform:uppercase;
    font-family:'DM Mono',monospace;
  }}
  .chat-body {{
    padding:12px 16px; min-height:60px; max-height:120px; overflow-y:auto;
    font-size:13px; color:var(--text); line-height:1.6;
  }}
  .chat-input-row {{
    display:flex; border-top:1px solid var(--rim);
  }}
  .chat-input {{
    flex:1; background:transparent; border:none; outline:none;
    color:var(--text); font-family:'Syne',sans-serif; font-size:13px;
    padding:12px 16px;
  }}
  .chat-input::placeholder {{ color:var(--muted); }}
  .chat-send {{
    padding:12px 20px; background:var(--accent); border:none; cursor:pointer;
    color:#000; font-family:'Syne',sans-serif; font-weight:700; font-size:12px;
    letter-spacing:1px; transition:opacity .2s;
  }}
  .chat-send:hover {{ opacity:.8; }}
  .msg-user {{ color: var(--muted); font-size:11px; margin-bottom:4px; }}
  .msg-bot  {{ margin-bottom:10px; }}
</style>
</head>
<body>
<div class="wrap">
  <div class="label">XenoAI Companion · Live</div>

  <div class="face-ring">
    <span class="face-emoji">{emoji}</span>
  </div>

  <div class="mood-text">{mood}</div>

  <div class="grid">
    <div class="cell">
      <span class="cell-val">{eng}</span>
      <span class="cell-label">Energy</span>
    </div>
    <div class="cell">
      <span class="cell-val">{itr}</span>
      <span class="cell-label">Events</span>
    </div>
    <div class="cell">
      <span class="cell-val">{strk}</span>
      <span class="cell-label">Streak</span>
    </div>
  </div>

  <div class="energy-wrap">
    <div class="energy-row">
      <span class="label">Energy level</span>
      <span class="label">{eng}%</span>
    </div>
    <div class="energy-bar"><div class="energy-fill"></div></div>
  </div>

  <div class="expr-row">
    eyes: {expr['eyes']} &nbsp;·&nbsp; mouth: {expr['mouth']}
  </div>

  <div class="chat" id="chatbox">
    <div class="chat-header">Talk to XenoAI</div>
    <div class="chat-body" id="chatBody">
      <div class="msg-bot">Hey! I'm running on the web dashboard. Say something...</div>
    </div>
    <div class="chat-input-row">
      <input class="chat-input" id="chatInput" placeholder="Type anything..." />
      <button class="chat-send" onclick="sendMsg()">Send</button>
    </div>
  </div>

  <div class="time-pill">{now}</div>
  <div class="label">Auto-refreshes every 6s</div>
</div>

<script>
  let chatId = null;
  async function sendMsg() {{
    const inp = document.getElementById('chatInput');
    const msg = inp.value.trim();
    if(!msg) return;
    inp.value = '';
    const body = document.getElementById('chatBody');
    body.innerHTML += `<div class="msg-user">You</div><div class="msg-bot">${{msg}}</div>`;
    body.scrollTop = body.scrollHeight;
    try {{
      const r = await fetch('/api/chat', {{
        method:'POST',
        headers:{{'Content-Type':'application/json'}},
        body: JSON.stringify({{message: msg, chat_id: chatId}})
      }});
      const d = await r.json();
      if (d.error) {{
        body.innerHTML += `<div class="msg-user">XenoAI</div><div class="msg-bot" style="color:#f66">{{d.error}}</div>`;
      }} else {{
        chatId = d.chat_id;
        body.innerHTML += `<div class="msg-user">XenoAI · ${{d.mood}} ${{d.expression?.emoji||''}}</div><div class="msg-bot">${{d.reply}}</div>`;
      }}
      body.scrollTop = body.scrollHeight;
    }} catch(e) {{
      body.innerHTML += `<div class="msg-bot" style="color:#f66">Connection error.</div>`;
    }}
  }}
  document.getElementById('chatInput').addEventListener('keydown', e => {{
    if(e.key === 'Enter') sendMsg();
  }});

  // ── Fetch-based state refresh — never reloads the page ──────────────────
  async function refreshState() {{
    try {{
      const r = await fetch('/api/state');
      const d = await r.json();
      if (d.mood) {{
        document.querySelector('.mood-text').textContent = d.mood;
        document.querySelector('.energy-fill').style.width = d.energy + '%';
        document.querySelectorAll('.cell-val')[0].textContent = d.energy;
        document.querySelectorAll('.cell-val')[1].textContent = d.total_interactions;
        document.querySelectorAll('.cell-val')[2].textContent = d.face_streak;
        document.querySelector('.expr-row').textContent =
          'eyes: ' + (d.expression?.eyes||'?') + '  ·  mouth: ' + (d.expression?.mouth||'?');
        document.querySelector('.time-pill').textContent = d.time + ' · ' + d.date;
        if (d.message) {{
          const body = document.getElementById('chatBody');
          body.innerHTML += `<div class="msg-user">XenoAI · ${{d.mood}}</div><div class="msg-bot">${{d.message}}</div>`;
          body.scrollTop = body.scrollHeight;
        }}
      }}
    }} catch(e) {{ console.warn('State refresh failed', e); }}
    setTimeout(refreshState, 6000);
  }}
  setTimeout(refreshState, 6000);
</script>
</body>
</html>"""


@app.route("/health")
def health():
    return jsonify({
        "status":  "ok",
        "version": "companion-v1",
        "cv2":     HAS_CV2,
        "db":      HAS_PG and bool(DATABASE_URL),
        "groq":    bool(GROQ_API_KEY),
        "time":    datetime.now().isoformat()
    })


# ─── ENTRY POINT ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    print(f"🤖 XenoAI Companion v1 → http://localhost:{port}")
    app.run(host="0.0.0.0", port=port, debug=False)
