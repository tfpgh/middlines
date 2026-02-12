#include "secrets.h"
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_task_wdt.h>
#include <libpax_api.h>
#include <string>

#define UPDATE_INTERVAL 30
#define BLE_SCAN_TIME 0 // 0 = constant scanning

#define CLIENT_ID "Atwater-ESP32"
#define NODE_LOCATION "Atwater"

#define MQTT_HOST "mqtt.middlines.com"
#define MQTT_PORT 1883

// If loop doesn't run for this long, restart ESP
#define WDT_TIMEOUT_S 60

#define WIFI_CONNECT_TIMEOUT_MS 10000
#define WIFI_RETRY_DELAY_MS 5000
#define MQTT_RETRY_DELAY_MS 5000
#define MAX_CONSECUTIVE_MQTT_FAILURES 5

count_payload_t ble_device_count;
WiFiClient netClient;
PubSubClient mqttClient(netClient);

volatile bool newCountAvailable = false;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastStatusLog = 0;
int mqttConsecutiveFails = 0;

// libpax callback
void on_count() {
  newCountAvailable = true;
  Serial.printf("[BLE  %lu] New count ready: %u\n", millis(),
                ble_device_count.ble_count);
}

void init_libpax() {
  Serial.printf("[INIT %lu] Initializing LibPax BLE scanning...\n", millis());

  libpax_config_t cfg;
  libpax_default_config(&cfg);

  cfg.blecounter = 1;
  cfg.blescantime = BLE_SCAN_TIME;
  cfg.wificounter = 0;
  cfg.ble_rssi_threshold = -120;

  libpax_update_config(&cfg);
  libpax_counter_init(on_count, &ble_device_count, UPDATE_INTERVAL, 0);
  libpax_counter_start();

  Serial.printf("[INIT %lu] LibPax started (scan=%ds, interval=%ds)\n",
                millis(), BLE_SCAN_TIME, UPDATE_INTERVAL);
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED)
    return true;

  unsigned long now = millis();
  if (now - lastWifiAttempt < WIFI_RETRY_DELAY_MS)
    return false;
  lastWifiAttempt = now;

  Serial.printf("[WIFI %lu] Connecting to %s ...\n", millis(), WIFI_SSID);

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI %lu] Connected! IP: %s  RSSI: %d dBm\n", millis(),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.printf("[WIFI %lu] Connection FAILED (status=%d), will retry in %ds\n",
                millis(), WiFi.status(), WIFI_RETRY_DELAY_MS / 1000);
  return false;
}

bool connectMQTT() {
  if (mqttClient.connected())
    return true;
  if (WiFi.status() != WL_CONNECTED)
    return false;

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_DELAY_MS)
    return false;
  lastMqttAttempt = now;

  Serial.printf("[MQTT %lu] Connecting to %s:%d as \"%s\"...\n", millis(),
                MQTT_HOST, MQTT_PORT, CLIENT_ID);

  if (mqttClient.connect(CLIENT_ID)) {
    Serial.printf("[MQTT %lu] Connected!\n", millis());
    mqttConsecutiveFails = 0;
    return true;
  }

  mqttConsecutiveFails++;
  Serial.printf("[MQTT %lu] Failed (state=%d, consecutive=%d)\n", millis(),
                mqttClient.state(), mqttConsecutiveFails);

  if (mqttConsecutiveFails >= MAX_CONSECUTIVE_MQTT_FAILURES) {
    Serial.printf("[MQTT %lu] Too many failures — forcing WiFi reconnect\n",
                  millis());
    WiFi.disconnect(true);
    mqttConsecutiveFails = 0;
  }

  return false;
}

void publishCount() {
  if (!newCountAvailable)
    return;
  if (!mqttClient.connected())
    return;

  std::string topic = "middlines/" + std::string(NODE_LOCATION) + "/count";
  std::string count = std::to_string(ble_device_count.ble_count);

  if (mqttClient.publish(topic.c_str(), count.c_str())) {
    Serial.printf("[PUB  %lu] %s -> %s\n", millis(), topic.c_str(),
                  count.c_str());
    newCountAvailable = false;
  } else {
    Serial.printf("[PUB  %lu] Publish FAILED (state=%d)\n", millis(),
                  mqttClient.state());
    // leave newCountAvailable = true so we retry next loop
  }
}

void logStatus() {
  unsigned long now = millis();
  if (now - lastStatusLog < 30000)
    return;
  lastStatusLog = now;

  Serial.printf("[STAT %lu] WiFi=%s RSSI=%d MQTT=%s FreeHeap=%u\n", millis(),
                WiFi.status() == WL_CONNECTED ? "OK" : "DOWN", WiFi.RSSI(),
                mqttClient.connected() ? "OK" : "DOWN", ESP.getFreeHeap());
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  };
  delay(1000);

  Serial.printf("\n\n[INIT %lu] ========== BOOT ==========\n", millis());
  Serial.printf("[INIT %lu] Reset reason: %d\n", millis(), esp_reset_reason());
  Serial.printf("[INIT %lu] Free heap: %u\n", millis(), ESP.getFreeHeap());

  // Hardware watchdog — reboots the board if loop() stops running
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // MQTT settings — generous keepalive to survive BLE scan windows
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setKeepAlive(60);     // 60s keepalive (default is 15)
  mqttClient.setSocketTimeout(10); // 10s socket timeout

  // Connect WiFi (blocking on first boot is fine)
  while (!connectWiFi()) {
    esp_task_wdt_reset();
    delay(1000);
  }

  // Connect MQTT (also block on first boot)
  while (!connectMQTT()) {
    esp_task_wdt_reset();
    delay(1000);
  }

  // Start BLE scanning last, after networking is up
  init_libpax();

  Serial.printf("[INIT %lu] ========== READY ==========\n", millis());
}

void loop() {
  esp_task_wdt_reset();

  connectWiFi();
  connectMQTT();

  mqttClient.loop();

  publishCount();

  logStatus();

  delay(50);
}
