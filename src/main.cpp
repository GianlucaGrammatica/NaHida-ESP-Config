#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <BH1750.h>
#include <SoftwareSerial.h>
#include <DFPlayerMini_Fast.h>
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LED_PIN D0
#define BTN_PIN D5
#define DHT_PIN D7
#define DHT_TYPE DHT11
#define SOIL_PIN A0
#define SOIL_DRY 880
#define SOIL_WET 390
#define DF_RX D8
#define DF_TX D6

// Indici mp3 nella cartella /mp3 della SD
#define SND_AVVIO   1   // 0001.mp3
#define SND_ACQUA   2   // 0002.mp3
#define SND_ALERT   3   // 0003.mp3

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
SoftwareSerial dfSerial(DF_RX, DF_TX);
DFPlayerMini_Fast dfPlayer;

bool dfReady = false;

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
unsigned long lastAlertCheck = 0;
bool lastButtonState = HIGH;


void playSound(int track) {
    if (dfReady) {
        dfPlayer.play(track);
    }
}

void updateOLED(bool isOnline) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);

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
    display.setCursor(0, 50);
    display.printf("Lux:  %.0f\n", currentReadings.luminosity);

    display.display();
}

void showSplash() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(10, 5);
    display.print("NaHida");
    display.setTextSize(1);
    display.setCursor(20, 28);
    display.print("Plant Monitor");
    display.drawLine(0, 38, 128, 38, SSD1306_WHITE);
    display.setCursor(0, 44);
    display.print("Connessione WiFi...");
    display.display();
}

void setupWiFi() {
    int tentativi = 0;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED && tentativi < 20) {
        delay(500);
        tentativi++;
        display.setCursor(tentativi * 6, 56);
        display.print(".");
        display.display();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi fallito, continuo offline");
    }
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

void showButtonFeedback() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 20);
    display.println("Annaffiata!");
    display.display();
    delay(600);
    updateOLED(mqttClient.connected());
}

void handleButton() {
    bool currentButtonState = digitalRead(BTN_PIN);

    if (lastButtonState == HIGH && currentButtonState == LOW) {
        if (millis() - lastDebounceTime > 200) {
            mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), "BUTTON_PRESSED");
            Serial.println("Click inviato!");
            lastDebounceTime = millis();
            playSound(SND_ACQUA);
            showButtonFeedback();
        }
    }

    lastButtonState = currentButtonState;
}

void readSensors() {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    int rawSoil = analogRead(SOIL_PIN);
    float soilPercent = map(rawSoil, SOIL_DRY, SOIL_WET, 0, 100);
    currentReadings.soilHum = soilPercent;
    float lux = lightMeter.readLightLevel();
    if (lux >= 0) {
        currentReadings.luminosity = lux;
    }

    if (!isnan(h) && !isnan(t)) {
        currentReadings.humidity = h;
        currentReadings.temperature = t - 2.0;
    } else {
        Serial.println("DHT11: lettura fallita");
    }
}

void publishTelemetry() {
    if (millis() - lastSensorPublish > 60000) {
        lastSensorPublish = millis();

        String payload = "{";
        payload += "\"type\":\"sensor_data\",";
        payload += "\"humidity\":" + String(currentReadings.humidity, 1) + ",";
        payload += "\"temperature\":" + String(currentReadings.temperature, 1) + ",";
        payload += "\"soil_humidity\":" + String(currentReadings.soilHum, 1) + ",";
        payload += "\"luminosity\":" + String(currentReadings.luminosity, 1);
        payload += "}";

        mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), payload.c_str());
    }
}

// Controlla se i valori sono fuori range e suona l'alert
void checkAlerts() {
    if (millis() - lastAlertCheck < 300000) return;
    lastAlertCheck = millis();

    // Controlla solo se la config è stata ricevuta
    if (currentConfig.name == "Waiting...") return;

    bool fuoriRange =
        currentReadings.temperature < currentConfig.tempMin ||
        currentReadings.temperature > currentConfig.tempMax ||
        currentReadings.humidity < currentConfig.humMin ||
        currentReadings.humidity > currentConfig.humMax ||
        currentReadings.soilHum < currentConfig.soilHumMin ||
        currentReadings.soilHum > currentConfig.soilHumMax;

    if (fuoriRange) {
        Serial.println("Alert: sensore fuori range!");
        playSound(SND_ALERT);
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP);
    dht.begin();

    Wire.begin();
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 non trovato!");
        while (true);
    }

    display.clearDisplay();
    display.display();

    showSplash();

    // Inizializzazione DFPlayer
    dfSerial.begin(9600);
    delay(500);
    if (dfPlayer.begin(dfSerial, false)) {
        dfReady = true;
        dfPlayer.volume(15);
        Serial.println("DFPlayer pronto");
        playSound(SND_AVVIO);
    } else {
        Serial.println("DFPlayer non trovato, continuo senza audio");
    }

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
    checkAlerts();

    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        updateOLED(mqttClient.connected());
    }
}