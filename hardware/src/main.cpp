#include <Arduino.h>
#include <libpax_api.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "config.h"
#include <string>

count_payload_t ble_device_count;

WiFiClientSecure netClient;
PubSubClient mqttClient(netClient);

// Called by LibPax when BLE counts update, publishes count to MQTT server
void on_count() {

    std::string topic = "middlines/" + std::string(NODE_LOCATION) + "/count";
    std::string count = std::to_string(ble_device_count.ble_count);    

    if (mqttClient.publish(topic.c_str(), count.c_str())) {
        Serial.printf("[BLE %lu] Published BLE count: %lu\n", millis(), ble_device_count.ble_count);
    } else {
        Serial.printf("[BLE %lu] Publish failed! MQTT state: %d\n", millis(), mqttClient.state());
    }
}

void init_libpax() {
    Serial.printf("[INIT %lu] Initializing LibPax BLE scanning...\n", millis());

    libpax_config_t cfg;
    libpax_default_config(&cfg);

    cfg.blecounter = 1;               // enable BLE counting
    cfg.blescantime = BLE_SCAN_TIME;
    cfg.wificounter = 0;              // disable Wi-Fi
    cfg.ble_rssi_threshold = -100;    // capture all nearby devices

    libpax_update_config(&cfg);
    libpax_counter_init(on_count, &ble_device_count, UPDATE_INTERVAL, 0);
    libpax_counter_start();

    Serial.printf("[INIT %lu] LibPax started\n", millis());
}

void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected!");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
}

void connectMQTT() {
    netClient.setCACert(AWS_ROOT_CA);
    netClient.setCertificate(AWS_CERTIFICATE);
    netClient.setPrivateKey(AWS_PRIVATE_KEY);
    mqttClient.setServer(AWS_ENDPOINT, 8883);

    while (!mqttClient.connected()) {
        if (mqttClient.connect(CLIENT_ID)) break;
        delay(5000);
    }
}

void setup() {
    Serial.begin(115200); while (!Serial) {};
    delay(100);

    connectWiFi();
    connectMQTT();
    init_libpax();
}

void loop() {

    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    yield(); 
    delay(500);
}