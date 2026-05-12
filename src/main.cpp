#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LED_PIN D5
#define BTN_PIN D6

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// --- STRUCTS ---
struct PlantConfig {
    String name = "Waiting...";
    float humMin = 0, humMax = 100;
    float tempMin = 0, tempMax = 50;
    float soilHumMin = 0, soilHumMax = 100;
};

struct SensorReadings {
    float humidity = 0;
    float temperature = 0;
    float soilHum = 0;
    float luminosity = 0;
};

PlantConfig currentConfig;
SensorReadings currentReadings;

// --- TIMERS ---
unsigned long lastSensorPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastDebounceTime = 0;
bool lastButtonState = HIGH;


void updateOLED(bool isOnline) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);

    // Taglia il nome se è troppo lungo
    String displayName = currentConfig.name.length() > 12 ? currentConfig.name.substring(0, 11) + "." : currentConfig.name;
    display.print(displayName);
    display.setCursor(100, 0);
    display.print(isOnline ? "ON" : "OFF");

    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    display.setCursor(0, 14);
    display.printf("Temp: %.1f C\n", currentReadings.temperature);
    display.setCursor(0, 26);
    display.printf("Hum:  %.1f %%\n", currentReadings.humidity);
    display.setCursor(0, 38);
    display.printf("Soil: %.1f %%\n", currentReadings.soilHum);

    display.display();
}

void setupWiFi() {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Connecting WiFi...");
    display.display();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connesso");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }

    String topicStr = String(topic);

    // Toggle LED
    if (topicStr == String("device/") + DEVICE_TOKEN) {
        if (message == "ON") digitalWrite(LED_PIN, HIGH);
        else if (message == "OFF") digitalWrite(LED_PIN, LOW);
    }

    // Parsing della configurazione
    if (topicStr == String("device/") + DEVICE_TOKEN + "/config") {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, message);
        if (!err) {
            currentConfig.name = doc["plant_name"].as<String>();
            currentConfig.humMin = doc["hum_min"] | 0.0f;
            currentConfig.humMax = doc["hum_max"] | 100.0f;
            currentConfig.tempMin = doc["temp_min"] | 0.0f;
            currentConfig.tempMax = doc["temp_max"] | 50.0f;
            currentConfig.soilHumMin = doc["soil_hum_min"] | 0.0f;
            currentConfig.soilHumMax = doc["soil_hum_max"] | 100.0f;

            updateOLED(mqttClient.connected());
        }
    }
}

void connectMQTT() {
    if (!mqttClient.connected()) {
        String clientId = String("ESP-") + DEVICE_TOKEN;
        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN).c_str(), 1);
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/config").c_str(), 1);
            mqttClient.publish((String("device/") + DEVICE_TOKEN + "/status").c_str(), "ONLINE");
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/updates").c_str(), 1);
        }
    }
}

void handleButton() {
    bool currentButtonState = digitalRead(BTN_PIN);

    // Controlla se il bottone è appena stato premuto (da HIGH a LOW)
    if (lastButtonState == HIGH && currentButtonState == LOW) {

        // Debounce accetta il click solo se sono passati 200ms dall'ultimo
        if (millis() - lastDebounceTime > 200) {
            mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), "BUTTON_PRESSED");            Serial.println("Click inviato!");
            lastDebounceTime = millis();
        }
    }

    lastButtonState = currentButtonState;
}

void readSensors() {
    // TO-DO Lettura dei sonsori
}

void publishTelemetry() {
    // Ogni 60 secondi
    if (millis() - lastSensorPublish > 60000) {
        lastSensorPublish = millis();

        String payload = "{";
        payload += "\"type\":\"sensor_data\",";
        payload += "\"humidity\":" + String(currentReadings.humidity, 1) + ",";
        payload += "\"temperature\":" + String(currentReadings.temperature, 1) + ",";
        payload += "\"soil_humidity\":" + String(currentReadings.soilHum, 1) + ",";
        payload += "\"luminosity\":" + String(currentReadings.luminosity, 1);
        payload += "}";

        mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), payload.c_str());    }
}


void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP);

    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

    espClient.setInsecure();
    setupWiFi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
}

void loop() {
    connectMQTT();
    mqttClient.loop();

    handleButton();
    readSensors();
    publishTelemetry();

    // Aggiornamento scehrmo ogni 2 secondi
    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        updateOLED(mqttClient.connected());
    }
}