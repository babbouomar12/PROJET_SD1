import tensorflow as tf
import numpy as np
import cv2
import os

# ================= CONFIG =================
IMG_SIZE = 96
EPOCHS = 20
BATCH_SIZE = 8
DATASET_PATH = "dataset"

# ================= LOAD DATA =================
def load_dataset(path):
    X, y = [], []
    class_map = {"authorized": 0, "unauthorized": 1}

    for class_name, label in class_map.items():
        folder = os.path.join(path, class_name)
        for file in os.listdir(folder):
            img_path = os.path.join(folder, file)
            img = cv2.imread(img_path, cv2.IMREAD_GRAYSCALE)
            if img is None:
                continue
            img = cv2.resize(img, (IMG_SIZE, IMG_SIZE))
            X.append(img)
            y.append(label)

    X = np.array(X, dtype=np.float32) / 255.0
    y = tf.keras.utils.to_categorical(y, 2)

    X = X[..., np.newaxis]  # (N, 96, 96, 1)
    return X, y

X, y = load_dataset(DATASET_PATH)

print("Dataset shape:", X.shape, y.shape)

# ================= MODEL =================
model = tf.keras.Sequential([
    tf.keras.layers.Input(shape=(IMG_SIZE, IMG_SIZE, 1)),

    tf.keras.layers.Conv2D(8, (3,3), activation='relu'),
    tf.keras.layers.MaxPooling2D(),

    tf.keras.layers.Conv2D(16, (3,3), activation='relu'),
    tf.keras.layers.MaxPooling2D(),

    tf.keras.layers.Flatten(),
    tf.keras.layers.Dense(32, activation='relu'),
    tf.keras.layers.Dense(2, activation='softmax')
])

model.compile(
    optimizer='adam',
    loss='categorical_crossentropy',
    metrics=['accuracy']
)

model.summary()

# ================= TRAIN =================
model.fit(
    X, y,
    epochs=EPOCHS,
    batch_size=BATCH_SIZE,
    validation_split=0.2,
    shuffle=True
)

# ================= QUANTIZATION =================
def representative_dataset():
    for i in range(min(50, len(X))):
        yield [X[i:i+1]]

converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.uint8
converter.inference_output_type = tf.uint8

tflite_model = converter.convert()

# ================= SAVE =================
with open("model.tflite", "wb") as f:
    f.write(tflite_model)

print("✅ model.tflite generated")
print("📦 Model size:", len(tflite_model) / 1024, "KB")
