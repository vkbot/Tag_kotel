// Minimal ESP8266 example for Cloudflare Worker integration.
// Replace placeholders and call sendTelemetry()/pollCommands() from loop().

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASS";
const char* WORKER_BASE = "https://your-worker.workers.dev";
const char* DEVICE_SECRET = "your-device-secret"; // must match Worker DEVICE_SECRET

WiFiClientSecure client;

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  client.setInsecure(); // for simplicity
}

void sendTelemetry(float t, float h, bool relayState) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(WORKER_BASE) + "/device";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_SECRET);

  StaticJsonDocument<256> doc;
  doc["temperature"] = t;
  doc["humidity"] = h;
  doc["relayState"] = relayState;
  doc["timestamp"] = "2026-04-13T00:00:00Z";

  String body;
  serializeJson(doc, body);
  http.POST(body);
  http.end();
}

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(WORKER_BASE) + "/device/commands";
  http.begin(client, url);
  http.addHeader("Authorization", String("Bearer ") + DEVICE_SECRET);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, payload)) return;

  JsonArray commands = doc["commands"].as<JsonArray>();
  bool hadRelayCommand = false;

  for (JsonVariant c : commands) {
    const char* type = c["type"] | "";
    const char* value = c["value"] | "";

    if (String(type) == "relay") {
      hadRelayCommand = true;

      if (String(value) == "on") {
        // TODO: turn relay ON
      } else if (String(value) == "off") {
        // TODO: turn relay OFF
      }
    }
  }

  if (hadRelayCommand) {
    HTTPClient ack;
    String ackUrl = String(WORKER_BASE) + "/device/commands/ack";
    ack.begin(client, ackUrl);
    ack.addHeader("Authorization", String("Bearer ") + DEVICE_SECRET);
    ack.addHeader("Content-Type", "application/json");
    ack.POST("{}");
    ack.end();
  }
}
