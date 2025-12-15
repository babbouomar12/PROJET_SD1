import os
import time
import json
import threading
import queue
import numpy as np
import cv2
import requests
import smtplib
from email.message import EmailMessage
from flask import Flask, request, jsonify, send_from_directory
from deepface import DeepFace

# =========================
# Receiver ESP32 (LED driver)
# =========================
RECEIVER_IP = "192.168.1.38"
RECEIVER_PORT = 8081
RECEIVER_URL = f"http://{RECEIVER_IP}:{RECEIVER_PORT}/result"
SEND_TO_RECEIVER = True

# =========================
# Telegram (OWNER ALERT)
# =========================
TELEGRAM_ENABLED = True
TELEGRAM_BOT_TOKEN = "8416307441:AAGhA4CIUZIAxJ1YDz4MdQ0tTF5uVL1j9UY"
TELEGRAM_CHAT_ID   = "1030432405" 

# =========================
# Email (Gmail App Password)
# =========================
EMAIL_ENABLED = True
SMTP_HOST = "smtp.gmail.com"
SMTP_PORT = 587
EMAIL_USER = "babbouomar12@gmail.com"
EMAIL_APP_PASSWORD = "jfhi pcpc rwmp pohd"
EMAIL_TO = "omar.babbou@insat.ucar.tn"

# =========================
# Alert anti-spam
# =========================
ALERT_COOLDOWN_SEC = 60
_last_alert_ts = 0
_alert_lock = threading.Lock()  # Thread-safe alerts

# PC LAN IP for viewing last image in browser
PC_LAN_IP = "192.168.1.37"
PC_PORT = 8000

# =========================
# DeepFace DB + uploads
# =========================
DB_PATH = "face_db.json"
SAVE_DIR = "uploads"
os.makedirs(SAVE_DIR, exist_ok=True)

THRESHOLD = 0.45

# =========================
# Utils
# =========================
def l2norm(x):
    x = np.asarray(x, dtype=np.float32)
    norm = np.linalg.norm(x)
    return x / (norm + 1e-12) if norm > 0 else x

def cosine_sim(a, b):
    return float(np.dot(l2norm(a), l2norm(b)))

def load_db():
    with open(DB_PATH, "r", encoding="utf-8") as f:
        db = json.load(f)
    if "authorized_centroid" not in db:
        raise RuntimeError("face_db.json missing 'authorized_centroid' (run enroll_deepface.py)")
    centroid = l2norm(np.array(db["authorized_centroid"], dtype=np.float32))
    return centroid, db

def fire_and_forget(fn, *args, **kwargs):
    threading.Thread(target=fn, args=args, kwargs=kwargs, daemon=True).start()

# =========================
# Receiver sender (with connection pooling)
# =========================
_receiver_session = requests.Session()
_receiver_session.headers.update({"Connection": "keep-alive"})

def send_to_receiver(payload: dict) -> bool:
    if not SEND_TO_RECEIVER:
        return False
    try:
        r = _receiver_session.post(RECEIVER_URL, json=payload, timeout=3)
        print(f"➡️ Receiver -> HTTP {r.status_code}", flush=True)
        return r.status_code == 200
    except Exception as e:
        print(f"❌ Receiver failed: {e}", flush=True)
        return False

# =========================
# Telegram (with connection pooling)
# =========================
_telegram_session = requests.Session()
_telegram_session.headers.update({"Connection": "keep-alive"})

def telegram_send_message(text: str) -> bool:
    if not TELEGRAM_ENABLED or not TELEGRAM_BOT_TOKEN or "PASTE_" in TELEGRAM_BOT_TOKEN:
        return False
    if not TELEGRAM_CHAT_ID or "PASTE_" in str(TELEGRAM_CHAT_ID):
        return False

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage"
    data = {"chat_id": TELEGRAM_CHAT_ID, "text": text, "disable_web_page_preview": True}
    try:
        r = _telegram_session.post(url, data=data, timeout=8)
        return r.status_code == 200
    except Exception as e:
        print(f"❌ Telegram error: {e}", flush=True)
        return False

def telegram_send_photo(caption: str, image_path: str) -> bool:
    if not TELEGRAM_ENABLED or not TELEGRAM_BOT_TOKEN or "PASTE_" in TELEGRAM_BOT_TOKEN:
        return False
    if not TELEGRAM_CHAT_ID or "PASTE_" in str(TELEGRAM_CHAT_ID):
        return False
    if not image_path or not os.path.exists(image_path):
        return False

    url = f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendPhoto"
    try:
        with open(image_path, "rb") as f:
            files = {"photo": f}
            data = {"chat_id": TELEGRAM_CHAT_ID, "caption": caption}
            r = requests.post(url, data=data, files=files, timeout=15)
        return r.status_code == 200
    except Exception as e:
        print(f"❌ Telegram photo error: {e}", flush=True)
        return False

# =========================
# Email (optimized with shorter timeouts)
# =========================
def email_send_message(subject: str, body: str, attachment_path: str | None = None) -> bool:
    if not EMAIL_ENABLED:
        return False
    if not EMAIL_USER or "@" not in EMAIL_USER:
        return False
    if not EMAIL_TO or "@" not in EMAIL_TO:
        return False
    if not EMAIL_APP_PASSWORD or "PASTE_" in EMAIL_APP_PASSWORD:
        return False

    app_pw = EMAIL_APP_PASSWORD.replace(" ", "")
    msg = EmailMessage()
    msg["From"] = EMAIL_USER
    msg["To"] = EMAIL_TO
    msg["Subject"] = subject
    msg.set_content(body)

    if attachment_path and os.path.exists(attachment_path):
        try:
            with open(attachment_path, "rb") as f:
                img_bytes = f.read()
            msg.add_attachment(img_bytes, maintype="image", subtype="jpeg",
                             filename=os.path.basename(attachment_path))
        except Exception as e:
            print(f"⚠️ Attachment failed: {e}", flush=True)

    # Try STARTTLS first (faster)
    try:
        with smtplib.SMTP(SMTP_HOST, 587, timeout=15) as smtp:
            smtp.ehlo()
            smtp.starttls()
            smtp.ehlo()
            smtp.login(EMAIL_USER, app_pw)
            smtp.send_message(msg)
        print("✅ Email sent via 587", flush=True)
        return True
    except Exception as e1:
        print(f"❌ Email 587 failed: {e1}", flush=True)
        try:
            with smtplib.SMTP_SSL(SMTP_HOST, 465, timeout=15) as smtp:
                smtp.ehlo()
                smtp.login(EMAIL_USER, app_pw)
                smtp.send_message(msg)
            print("✅ Email sent via 465", flush=True)
            return True
        except Exception as e2:
            print(f"❌ Email 465 failed: {e2}", flush=True)
            return False

# =========================
# Unified alert logic (thread-safe)
# =========================
def maybe_alert_owner(payload: dict):
    global _last_alert_ts

    if payload.get("authorized", False):
        return

    with _alert_lock:
        now = time.time()
        if (now - _last_alert_ts) < ALERT_COOLDOWN_SEC:
            return
        _last_alert_ts = now

    conf = payload.get("confidence", 0.0)
    reason = payload.get("reason", "n/a")
    saved_as = payload.get("saved_as", "n/a")
    latest_url = f"http://{PC_LAN_IP}:{PC_PORT}/latest.jpg"

    caption = (
        "🚨 Intruder alert!\n"
        f"authorized: false\n"
        f"confidence: {conf:.3f}\n"
        f"reason: {reason}\n"
        f"file: {saved_as}\n"
        f"latest: {latest_url}"
    )

    img_path = os.path.join(SAVE_DIR, saved_as) if saved_as else ""
    if not os.path.exists(img_path):
        img_path = os.path.join(SAVE_DIR, "latest.jpg")

    # Telegram first (faster)
    tg_ok = telegram_send_message(caption)
    tg_photo_ok = telegram_send_photo(caption, img_path) if tg_ok else False

    # Email fallback
    if not (tg_ok or tg_photo_ok):
        email_send_message("🚨 Smart_Device: Intruder", caption, img_path)

    if tg_photo_ok:
        print("✅ Alert sent (Telegram photo)", flush=True)
    elif tg_ok:
        print("✅ Alert sent (Telegram text)", flush=True)

# =========================
# Load DB & warm up model
# =========================
authorized_centroid, db = load_db()
MODEL_NAME = db.get("model_name", "Facenet")
DETECTOR = db.get("detector", "opencv")
EMB_DIM_DB = int(db.get("embedding_dim", 0))

print(f"🧠 Model={MODEL_NAME} detector={DETECTOR} dim={EMB_DIM_DB}", flush=True)
print("🧠 Warming up...", flush=True)
try:
    DeepFace.build_model(MODEL_NAME)
    print("✅ Model ready", flush=True)
except Exception as e:
    print(f"⚠️ Warmup failed: {e}", flush=True)

# =========================
# Background worker (increased workers for faster processing)
# =========================
NUM_WORKERS = 2  # Process 2 images in parallel
job_q: queue.Queue[tuple[str, str, str]] = queue.Queue(maxsize=20)

def worker_loop(worker_id: int):
    print(f"🔧 Worker {worker_id} started", flush=True)
    while True:
        img_path, fname, remote = job_q.get()
        try:
            process_inference_file(img_path, fname, remote)
        except Exception as e:
            print(f"❌ Worker {worker_id} error: {e}", flush=True)
        finally:
            job_q.task_done()

def start_workers():
    for i in range(NUM_WORKERS):
        threading.Thread(target=worker_loop, args=(i,), daemon=True).start()

# =========================
# Inference job (optimized)
# =========================
def process_inference_file(img_path: str, fname: str, remote_addr: str):
    t0 = time.time()

    # Fast file read
    try:
        with open(img_path, "rb") as f:
            raw = f.read()
    except Exception as e:
        print(f"❌ File read error: {e}", flush=True)
        return

    arr = np.frombuffer(raw, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)

    if img is None:
        payload = {"error": "bad_decode", "saved_as": fname, "from": remote_addr}
        print(f"JSON: {json.dumps(payload)}", flush=True)
        fire_and_forget(send_to_receiver, payload)
        return

    try:
        reps = DeepFace.represent(
            img_path=img,
            model_name=MODEL_NAME,
            detector_backend=DETECTOR,
            enforce_detection=True
        )

        if not reps:
            payload = {
                "authorized": False,
                "confidence": 0.0,
                "threshold": THRESHOLD,
                "reason": "no_face",
                "saved_as": fname
            }
        else:
            emb = l2norm(np.array(reps[0]["embedding"], dtype=np.float32))
            if EMB_DIM_DB and emb.shape[0] != EMB_DIM_DB:
                payload = {
                    "authorized": False,
                    "confidence": 0.0,
                    "threshold": THRESHOLD,
                    "reason": f"dim_mismatch",
                    "saved_as": fname
                }
            else:
                sim = cosine_sim(emb, authorized_centroid)
                authorized = sim >= THRESHOLD
                payload = {
                    "authorized": bool(authorized),
                    "confidence": float(sim),
                    "threshold": THRESHOLD,
                    "reason": "match" if authorized else "no_match",
                    "saved_as": fname
                }

    except Exception as e:
        payload = {
            "authorized": False,
            "confidence": 0.0,
            "threshold": THRESHOLD,
            "reason": f"error:{str(e)[:50]}",
            "saved_as": fname
        }

    payload["from"] = remote_addr
    payload["ms"] = int((time.time() - t0) * 1000)

    # Fire alerts and receiver updates in background
    fire_and_forget(send_to_receiver, dict(payload))
    fire_and_forget(maybe_alert_owner, dict(payload))

    print(f"JSON: {json.dumps(payload)}", flush=True)

# =========================
# Flask app
# =========================
app = Flask(__name__)

@app.get("/")
def home():
    return (
        "OK - DeepFace server\n"
        "POST /infer - Send JPEG\n"
        "GET /latest.jpg - Last image\n"
        "GET /files - List uploads\n"
        "GET /ping - Health check\n"
        f"Receiver: {SEND_TO_RECEIVER}\n"
        f"Telegram: {TELEGRAM_ENABLED}\n"
        f"Email: {EMAIL_ENABLED}\n"
    )

@app.get("/ping")
def ping():
    return "pong", 200

@app.get("/latest.jpg")
def latest():
    return send_from_directory(SAVE_DIR, "latest.jpg", mimetype="image/jpeg")

@app.get("/files")
def files():
    all_files = sorted([f for f in os.listdir(SAVE_DIR) if f.endswith(".jpg")])
    return jsonify(files=all_files[-50:])

@app.get("/test_alert")
def test_alert():
    payload = {
        "authorized": False,
        "confidence": 0.123,
        "threshold": THRESHOLD,
        "reason": "test",
        "saved_as": "latest.jpg"
    }
    fire_and_forget(maybe_alert_owner, payload)
    return jsonify(ok=True), 200

@app.post("/reload_db")
def reload_db():
    global authorized_centroid, db, MODEL_NAME, DETECTOR, EMB_DIM_DB
    authorized_centroid, db = load_db()
    MODEL_NAME = db.get("model_name", "Facenet")
    DETECTOR = db.get("detector", "opencv")
    EMB_DIM_DB = int(db.get("embedding_dim", 0))
    return jsonify({
        "ok": True,
        "samples": db.get("num_samples"),
        "dim": EMB_DIM_DB,
        "model": MODEL_NAME
    }), 200

@app.route("/infer", methods=["POST"])
def infer():
    raw = request.get_data()
    remote = request.remote_addr or "unknown"
    
    if not raw:
        return jsonify({"error": "empty"}), 400

    # Fast timestamp
    ts = time.strftime("%Y%m%d-%H%M%S")
    fname = f"{ts}.jpg"
    path = os.path.join(SAVE_DIR, fname)

    # Write files (blocking but fast)
    try:
        with open(path, "wb") as f:
            f.write(raw)
        with open(os.path.join(SAVE_DIR, "latest.jpg"), "wb") as f:
            f.write(raw)
    except Exception as e:
        print(f"❌ File write error: {e}", flush=True)
        return jsonify({"error": "write_failed"}), 500

    # Enqueue (non-blocking)
    try:
        job_q.put_nowait((path, fname, remote))
        print(f"📥 /infer from {remote} bytes={len(raw)} queued", flush=True)
    except queue.Full:
        print("⚠️ Queue full, dropping", flush=True)
        return jsonify({"error": "queue_full"}), 503

    # Immediate response
    return jsonify({"ok": True, "accepted": True, "saved_as": fname}), 202

if __name__ == "__main__":
    start_workers()
    # Use gevent or gunicorn in production for better performance
    app.run(host="0.0.0.0", port=8000, debug=False, threaded=True, use_reloader=False)