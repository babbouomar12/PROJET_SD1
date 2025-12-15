import os, json
import numpy as np
import cv2
from deepface import DeepFace

AUTH_DIR = "dataset/authorized"
OUT_DB = "face_db.json"

MODEL_NAME = "Facenet"          # IMPORTANT: must match server
DETECTOR = "opencv"             # fast; you can change to "retinaface" for better detection

def l2norm(x):
    x = np.asarray(x, dtype=np.float32)
    return x / (np.linalg.norm(x) + 1e-12)

embs = []
kept = 0

for fn in sorted(os.listdir(AUTH_DIR)):
    if not fn.lower().endswith((".jpg", ".jpeg", ".png")):
        continue

    path = os.path.join(AUTH_DIR, fn)
    img = cv2.imread(path)
    if img is None:
        print("Skip (bad read):", fn)
        continue

    try:
        reps = DeepFace.represent(
            img_path=img,
            model_name=MODEL_NAME,
            detector_backend=DETECTOR,
            enforce_detection=True
        )
        if not reps:
            print("No face:", fn)
            continue

        emb = l2norm(np.array(reps[0]["embedding"], dtype=np.float32))
        embs.append(emb)
        kept += 1
        print("OK:", fn)

    except Exception as e:
        print("Fail:", fn, "->", str(e))

if kept < 5:
    raise SystemExit("Not enough usable face images. Add more (try 10-30) with clear faces.")

centroid = l2norm(np.mean(np.stack(embs), axis=0))

db = {
    "engine": "deepface",
    "model_name": MODEL_NAME,
    "detector": DETECTOR,
    "embedding_dim": int(centroid.shape[0]),
    "num_samples": kept,
    "authorized_centroid": centroid.tolist()
}

with open(OUT_DB, "w", encoding="utf-8") as f:
    json.dump(db, f, indent=2)

print(f"\n✅ Saved {OUT_DB} with {kept} samples (dim={db['embedding_dim']})")
