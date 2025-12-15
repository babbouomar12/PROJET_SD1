#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

/* ===== WiFi ===== */
const char* ssid     = "gnet368649_plus";
const char* password = "368649368649";

/* ===== LED pins (change to your wiring) ===== */
#define LED_GREEN 13
#define LED_RED   12

/* ===== HTTP server ===== */
WebServer server(8081);

void setLED(bool green, bool red) {
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
  digitalWrite(LED_RED,   red   ? HIGH : LOW);
}

void handlePing() {
  server.send(200, "text/plain", "OK");
}

void handleResult() {
  // Read raw body
  String body = server.arg("plain");
  if (body.length() == 0) {
    Serial.println("\n❌ /result received EMPTY body");
    server.send(400, "application/json", "{\"error\":\"empty_body\"}");
    return;
  }

  // ✅ Print raw JSON exactly as received
  Serial.println("\n========== /result RECEIVED ==========");
  Serial.print("From IP: ");
  Serial.println(server.client().remoteIP());
  Serial.print("Length: ");
  Serial.println(body.length());
  Serial.println("RAW JSON:");
  Serial.println(body);
  Serial.println("======================================");

  // Parse JSON
  DynamicJsonDocument doc(1024);  // bigger to be safe
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(err.c_str());
    server.send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }

  // ✅ Pretty-print parsed JSON back to Serial
  Serial.println("PARSED JSON (pretty):");
  serializeJsonPretty(doc, Serial);
  Serial.println("\n======================================");

  // Extract fields (with defaults)
  bool authorized      = doc["authorized"] | false;
  float confidence     = doc["confidence"] | 0.0;
  float threshold      = doc["threshold"] | 0.0;
  const char* reason   = doc["reason"] | "n/a";
  const char* saved_as = doc["saved_as"] | "n/a";

  Serial.printf(
    "✅ Fields: authorized=%s confidence=%.3f threshold=%.3f reason=%s saved_as=%s\n",
    authorized ? "true" : "false",
    confidence,
    threshold,
    reason,
    saved_as
  );

  // LEDs
  if (authorized) setLED(true, false);
  else            setLED(false, true);

  server.send(200, "application/json", "{\"ok\":true}");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  setLED(false, false);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Serial.print("📡 Connecting WiFi");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 80) {
    delay(250);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ WiFi connected");
    Serial.print("Receiver ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("❌ WiFi failed (check SSID/password)");
  }

  server.on("/ping", HTTP_GET, handlePing);
  server.on("/result", HTTP_POST, handleResult);

  server.begin();
  Serial.println("✅ Receiver server listening on port 8081");
  Serial.println("Endpoints:");
  Serial.println("  GET  /ping");
  Serial.println("  POST /result   (JSON body)");
}

void loop() {
  server.handleClient();
}
