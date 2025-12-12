#include "esp_camera.h"
#include <WiFi.h>

/* ================= WIFI ================= */
const char* ssid = "Ooredoo-322652";
const char* password = "47A3D95BSr@54";
const char* serverIP = "192.168.0.132";
const uint16_t serverPort = 5000;
WiFiClient client;

/* ================= PINS ================= */
#define PIR_PIN            12
#define FLASH_GPIO_NUM      4

// AI Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

/* ================= TENSORFLOW LITE ================= */
#include "model.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/version.h"

constexpr int IMG_W = 96;
constexpr int IMG_H = 96;
constexpr int TENSOR_ARENA_SIZE = 60 * 1024;

static uint8_t tensor_arena[TENSOR_ARENA_SIZE];
static tflite::MicroInterpreter* interpreter;
static TfLiteTensor* input;
static TfLiteTensor* output;

/* ================= CAMERA INIT ================= */
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size   = FRAMESIZE_QQVGA; // 160x120
  config.fb_count     = 1;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Camera init failed");
    while (true);
  }
}

/* ================= ML INIT ================= */
void initML() {
  const tflite::Model* model_tflite = tflite::GetModel(model);
  static tflite::AllOpsResolver resolver;

  static tflite::MicroInterpreter static_interpreter(
    model_tflite, resolver, tensor_arena, TENSOR_ARENA_SIZE
  );

  interpreter = &static_interpreter;
  interpreter->AllocateTensors();

  input = interpreter->input(0);
  output = interpreter->output(0);

  Serial.println("✅ TensorFlow Lite Micro initialized");
}

/* ================= ANN INFERENCE ================= */
bool run_ann(camera_fb_t* fb, float* confidence) {

  // Resize 160x120 → 96x96
  for (int y = 0; y < IMG_H; y++) {
    for (int x = 0; x < IMG_W; x++) {
      int srcX = x * 160 / IMG_W;
      int srcY = y * 120 / IMG_H;
      input->data.uint8[y * IMG_W + x] =
        fb->buf[srcY * 160 + srcX];
    }
  }

  interpreter->Invoke();

  uint8_t auth_score = output->data.uint8[0]; // class 0 = authorized
  uint8_t unk_score  = output->data.uint8[1];

  *confidence = auth_score / 255.0;

  return auth_score > unk_score;
}

/* ================= SEND JSON ================= */
void sendJSON(bool authorized, float confidence) {
  if (!client.connected()) {
    if (!client.connect(serverIP, serverPort)) return;
  }

  String json =
    "{ \"authorized\": " + String(authorized ? "true" : "false") +
    ", \"confidence\": " + String(confidence, 2) + " }\n";

  client.print(json);
  Serial.println(json);
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(FLASH_GPIO_NUM, OUTPUT);

  initCamera();
  initML();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi connected");
}

/* ================= LOOP ================= */
void loop() {
  if (digitalRead(PIR_PIN) == HIGH) {

    digitalWrite(FLASH_GPIO_NUM, HIGH);
    delay(100);

    camera_fb_t* fb = esp_camera_fb_get();
    digitalWrite(FLASH_GPIO_NUM, LOW);
    if (!fb) return;

    float confidence;
    bool authorized = run_ann(fb, &confidence);
    esp_camera_fb_return(fb);

    sendJSON(authorized, confidence);
    delay(5000); // debounce PIR
  }
}
