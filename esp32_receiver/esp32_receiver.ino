#include <WiFi.h>

/* ================= WIFI ================= */
const char* ssid = "Ooredoo-322652";
const char* password = "47A3D95BSr@54";

WiFiServer server(5000);

/* ================= LED PINS ================= */
#define RED_PIN    12
#define GREEN_PIN  13
#define BLUE_PIN   14   // optional

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  // All LEDs OFF at start
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, LOW);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println("\n✅ ESP32 Receiver connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.begin();
  Serial.println("🟢 TCP server started");
}

/* ================= LOOP ================= */
void loop() {
  WiFiClient client = server.available();
  if (!client) return;

  String msg = client.readStringUntil('\n');
  Serial.println("⬅ Received: " + msg);

  // Simple JSON parsing (robust enough for this project)
  if (msg.indexOf("\"authorized\": true") >= 0) {
    // ACCESS GRANTED → GREEN
    digitalWrite(GREEN_PIN, HIGH);
    digitalWrite(RED_PIN, LOW);
    digitalWrite(BLUE_PIN, LOW);

    Serial.println("✅ ACCESS GRANTED (GREEN)");
  }
  else if (msg.indexOf("\"authorized\": false") >= 0) {
    // ACCESS DENIED → RED
    digitalWrite(RED_PIN, HIGH);
    digitalWrite(GREEN_PIN, LOW);
    digitalWrite(BLUE_PIN, LOW);

    Serial.println("❌ ACCESS DENIED (RED)");
  }

  client.stop();
}
