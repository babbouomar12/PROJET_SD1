#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>

/* =========================
   WiFi
   ========================= */
static const char* SSID     = "gnet368649_plus";
static const char* PASS     = "368649368649";

/* =========================
   PC Server (Flask)
   ========================= */
static const char* PC_IP    = "192.168.1.37";
static const uint16_t PC_PORT = 8000;
static const char* PC_PATH  = "/infer";   // POST raw JPEG bytes

/* =========================
   PIR
   ========================= */
#define PIR_GPIO 12
#define PIR_ACTIVE_HIGH true
static const uint32_t PIR_WARMUP_MS   = 5000;
static const uint32_t MIN_GAP_MS      = 3000;  // can reduce safely
static const uint32_t TRIGGER_HOLD_MS = 120;

/* =========================
   Flash (GPIO4) using LEDC
   ========================= */
#define FLASH_GPIO 4
#define FLASH_CH   2
#define FLASH_FREQ 5000
#define FLASH_RES  8
static inline void setFlash(uint8_t level) { ledcWrite(FLASH_CH, level); }

/* =========================
   AI Thinker ESP32-CAM pins
   ========================= */
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

static uint32_t bootMs = 0;
static uint32_t lastCaptureMs = 0;

/* =========================
   Fast WiFi connect / reconnect
   ========================= */
static void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Serial.print("📡 WiFi connecting");
  WiFi.begin(SSID, PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("✅ WiFi OK, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("❌ WiFi connect failed");
  }
}

/* =========================
   Camera init (stable)
   ========================= */
static bool initCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;

  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;

  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href  = HREF_GPIO_NUM;
  c.pin_sscb_sda = SIOD_GPIO_NUM;
  c.pin_sscb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;

  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;

  // Keep this stable. You can move to QVGA later if needed.
  c.frame_size   = FRAMESIZE_QQVGA; // 160x120
  c.jpeg_quality = 14;             // lower = better quality, higher = smaller
  c.grab_mode    = CAMERA_GRAB_LATEST;

  // More buffers if PSRAM exists (faster / less blocking)
  const bool psram = psramFound();
  c.fb_location = psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.fb_count    = psram ? 2 : 1;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init failed: 0x%x\n", (unsigned)err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  // Conservative tuning
  s->set_framesize(s, FRAMESIZE_QQVGA);
  s->set_quality(s, 14);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  s->set_whitebal(s, 1);

  Serial.printf("✅ Camera ready (psram=%s fb_count=%d)\n", psram ? "YES" : "NO", c.fb_count);
  return true;
}

/* =========================
   Read HTTP status line quickly
   ========================= */
static bool readHttpStatus(WiFiClient& client, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!client.available() && client.connected() && (millis() - t0) < timeoutMs) {
    delay(2);
  }
  if (!client.available()) return false;

  // Read first line: "HTTP/1.1 202 ACCEPTED"
  char line[80];
  size_t n = client.readBytesUntil('\n', line, sizeof(line) - 1);
  line[n] = '\0';

  // Trim \r
  if (n > 0 && line[n - 1] == '\r') line[n - 1] = '\0';

  Serial.print("📥 Status: ");
  Serial.println(line);

  // Accept 200 or 202
  return (strstr(line, " 200 ") != nullptr) || (strstr(line, " 202 ") != nullptr);
}

/* =========================
   HTTP POST raw bytes (optimized, minimal allocations)
   ========================= */
static bool postJpeg(const uint8_t* buf, size_t len) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  client.setNoDelay(true);
  client.setTimeout(6000); // read timeout for stream ops

  Serial.printf("➡️ TCP %s:%u ...\n", PC_IP, PC_PORT);
  if (!client.connect(PC_IP, PC_PORT)) {
    Serial.println("❌ TCP connect failed");
    client.stop();
    return false;
  }

  // Build headers with snprintf (no String)
  char hdr[220];
  int hdrLen = snprintf(
    hdr, sizeof(hdr),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Connection: close\r\n"
    "Content-Length: %u\r\n\r\n",
    PC_PATH, PC_IP, (unsigned)len
  );

  if (hdrLen <= 0 || hdrLen >= (int)sizeof(hdr)) {
    Serial.println("❌ Header build failed");
    client.stop();
    return false;
  }

  if (client.write((const uint8_t*)hdr, (size_t)hdrLen) != (size_t)hdrLen) {
    Serial.println("❌ Header send failed");
    client.stop();
    return false;
  }

  // Send body in chunks (fast + stable)
  Serial.printf("➡️ Upload %u bytes...\n", (unsigned)len);
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = (len - sent > 2048) ? 2048 : (len - sent);
    int w = client.write(buf + sent, chunk);
    if (w <= 0) {
      Serial.printf("❌ write failed at %u/%u\n", (unsigned)sent, (unsigned)len);
      client.stop();
      return false;
    }
    sent += (size_t)w;
    delay(0);
  }

  // Read status quickly; don’t block reading full response
  bool ok = readHttpStatus(client, 6000);

  client.stop();
  return ok;
}

/* =========================
   PIR confirm
   ========================= */
static bool pirMotionConfirmed() {
  uint32_t t0 = millis();
  while (millis() - t0 < TRIGGER_HOLD_MS) {
    int v = digitalRead(PIR_GPIO);
    bool active = PIR_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
    if (!active) return false;
    delay(5);
  }
  return true;
}

/* =========================
   Capture + Upload
   ========================= */
static void captureAndUpload(const char* reason) {
  Serial.printf("\n📸 Capture (reason=%s)\n", reason);

  // flash on briefly
  setFlash(255);
  delay(200);

  // discard one frame for exposure
  camera_fb_t* tmp = esp_camera_fb_get();
  if (tmp) esp_camera_fb_return(tmp);

  camera_fb_t* fb = esp_camera_fb_get();
  setFlash(0);

  if (!fb) {
    Serial.println("❌ Capture failed");
    return;
  }

  Serial.printf("🖼️ JPEG bytes=%u\n", (unsigned)fb->len);

  bool ok = postJpeg(fb->buf, fb->len);

  esp_camera_fb_return(fb);

  Serial.println(ok ? "✅ Uploaded (200/202)" : "❌ Upload failed");
}

/* =========================
   Serial trigger (robust, fast)
   - Works with any line ending
   ========================= */
static bool serialCaptureRequested() {
  static bool sawC = false;

  while (Serial.available() > 0) {
    char ch = (char)Serial.read();

    if (ch == 'c' || ch == 'C') {
      sawC = true;
    }

    // consider command complete on newline OR if user just sends 'c' with no ending
    if (ch == '\n' || ch == '\r') {
      bool r = sawC;
      sawC = false;
      return r;
    }
  }

  // If Serial Monitor is "No line ending", we still trigger immediately
  if (sawC) {
    sawC = false;
    return true;
  }

  return false;
}

/* =========================
   Setup / Loop
   ========================= */
void setup() {
  Serial.begin(115200);
  delay(250);

  bootMs = millis();

  // Flash PWM
  ledcSetup(FLASH_CH, FLASH_FREQ, FLASH_RES);
  ledcAttachPin(FLASH_GPIO, FLASH_CH);
  setFlash(0);

  pinMode(PIR_GPIO, INPUT_PULLDOWN);   // instead of INPUT


  ensureWiFi();

  if (!initCamera()) {
    while (true) delay(1000);
  }

  Serial.println("\n✅ Ready:");
  Serial.println("  - Type 'c' to capture");
  Serial.println("  - PIR motion triggers capture");
}

void loop() {
  const uint32_t now = millis();

  // Serial trigger
  if (serialCaptureRequested()) {
    if (now - lastCaptureMs >= 500) {
      lastCaptureMs = now;
      captureAndUpload("serial_c");
    } else {
      Serial.println("⚠️ Too fast (debounce 500ms)");
    }
  }

  // PIR trigger
  if (now - bootMs >= PIR_WARMUP_MS) {
    if (now - lastCaptureMs >= MIN_GAP_MS) {
      int v = digitalRead(PIR_GPIO);
      bool active = PIR_ACTIVE_HIGH ? (v == HIGH) : (v == LOW);
      if (active && pirMotionConfirmed()) {
        lastCaptureMs = now;
        captureAndUpload("pir_motion");
      }
    }
  }

  delay(15);
}
