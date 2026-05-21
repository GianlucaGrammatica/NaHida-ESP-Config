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
#include "config.h"

// --- PIN ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define RGB_R D0
#define RGB_G D4
#define RGB_B D3
#define BTN_PIN D5
#define DHT_PIN D7
#define DHT_TYPE DHT11
#define SOIL_PIN A0
#define SOIL_DRY 880
#define SOIL_WET 390

// --- AUDIO (non disponibile: moduli MP3-TF-16P cloni incompatibili) ---
// #include <DFPlayer.h>
// #define SND_AVVIO 1
// #define SND_ACQUA 2
// #define SND_ALERT 3
// DFPlayer mp3;
// bool dfReady = false;
// void playSound(int track) { if (dfReady) mp3.playMP3Folder(track); }

// --- OGGETTI ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;

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
unsigned long lastMqttAttempt = 0;
unsigned long lastLedBlink = 0;
bool lastButtonState = HIGH;
bool ledBlinkState = false;

// --- LED RGB ---
// Stato: 0=offline(blu), 1=ok(verde), 2=warning(giallo), 3=alert(rosso lampeggiante)
int ledState = 0;

void setRGB(bool r, bool g, bool b) {
    digitalWrite(RGB_R, r);
    digitalWrite(RGB_G, g);
    digitalWrite(RGB_B, b);
}

void updateLED() {
    switch (ledState) {
        case 0: setRGB(false, false, true);  break; // blu = offline
        case 1: setRGB(false, true,  false); break; // verde = ok
        case 2: setRGB(true,  true,  false); break; // giallo = warning
        case 3:                                      // rosso lampeggiante = alert
            if (millis() - lastLedBlink > 500) {
                lastLedBlink = millis();
                ledBlinkState = !ledBlinkState;
                setRGB(ledBlinkState, false, false);
            }
            break;
        default: break;
    }
}

void computeLedState() {
    if (!mqttClient.connected()) { ledState = 0; return; }
    if (currentConfig.name == "Waiting...") { ledState = 1; return; }

    bool critico =
        currentReadings.temperature < currentConfig.tempMin ||
        currentReadings.temperature > currentConfig.tempMax ||
        currentReadings.humidity < currentConfig.humMin ||
        currentReadings.humidity > currentConfig.humMax;

    bool warning =
        currentReadings.soilHum < currentConfig.soilHumMin ||
        currentReadings.soilHum > currentConfig.soilHumMax;

    if (critico) ledState = 3;
    else if (warning) ledState = 2;
    else ledState = 1;
}

// --- DISPLAY ---
void updateOLED(bool isOnline) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);

    String displayName = currentConfig.name.length() > 12
        ? currentConfig.name.substring(0, 11) + "."
        : currentConfig.name;
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

// --- WIFI ---
void setupWiFi() {
    int tentativi = 0;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED && tentativi < 20) {
        delay(500);
        yield();
        tentativi++;
        display.setCursor(tentativi * 6, 56);
        display.print(".");
        display.display();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi fallito, continuo offline");
    }
}

void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi caduto, riconnessione...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(500);
            yield();
        }
        Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi riconnesso" : "WiFi fallito");
    }
}

// --- MQTT ---
void mqttCallback(char* topic, const byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) message += static_cast<char>(payload[i]);
    String topicStr = String(topic);

    if (topicStr == String("device/") + DEVICE_TOKEN + "/config") {
        JsonDocument doc;
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
        if (millis() - lastMqttAttempt < 5000) return;
        lastMqttAttempt = millis();

        Serial.println("MQTT disconnesso, riconnessione...");
        String clientId = String("ESP-") + DEVICE_TOKEN;
        if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            Serial.println("MQTT connesso");
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN).c_str(), 1);
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/config").c_str(), 1);
            mqttClient.publish((String("device/") + DEVICE_TOKEN + "/status").c_str(), "ONLINE");
            mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/updates").c_str(), 1);
        } else {
            Serial.print("MQTT fallito, rc=");
            Serial.println(mqttClient.state());
        }
    }
}

// --- BOTTONE ---
void showButtonFeedback() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 20);
    display.println("Annaffiata!");
    display.display();
    setRGB(true, true, true); // bianco per feedback visivo
    delay(600);
    yield();
    updateOLED(mqttClient.connected());
}

void handleButton() {
    bool currentButtonState = digitalRead(BTN_PIN);
    if (lastButtonState == HIGH && currentButtonState == LOW) {
        if (millis() - lastDebounceTime > 200) {
            mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), "BUTTON_PRESSED");
            Serial.println("Click inviato!");
            lastDebounceTime = millis();
            showButtonFeedback();
        }
    }
    lastButtonState = currentButtonState;
}

// --- SENSORI ---
void readSensors() {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    int rawSoil = analogRead(SOIL_PIN);
    float soilPercent = static_cast<float>(map(rawSoil, SOIL_DRY, SOIL_WET, 0, 100));
    currentReadings.soilHum = constrain(soilPercent, 0.0f, 100.0f);
    float lux = lightMeter.readLightLevel();
    if (lux >= 0) currentReadings.luminosity = lux;

    if (!isnan(h) && !isnan(t)) {
        currentReadings.humidity = h;
        currentReadings.temperature = t - 2.0f;
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

// --- ALERT ---
void checkAlerts() {
    if (millis() - lastAlertCheck < 300000) return;
    lastAlertCheck = millis();
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
        // playSound(SND_ALERT);  // audio disabilitato
    }
}

// --- SETUP ---
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("BOOT");
    Serial.println(ESP.getResetReason());

    pinMode(RGB_R, OUTPUT);
    pinMode(RGB_G, OUTPUT);
    pinMode(RGB_B, OUTPUT);
    setRGB(false, false, false);
    pinMode(BTN_PIN, INPUT_PULLUP);
    dht.begin();

    Wire.begin();
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("SSD1306 non trovato!");
        while (true) { yield(); }
    }

    display.clearDisplay();
    display.display();
    showSplash();

    espClient.setInsecure();
    setupWiFi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setCallback(mqttCallback);
}

// --- LOOP ---
void loop() {
    connectMQTT();
    mqttClient.loop();

    handleButton();
    readSensors();
    publishTelemetry();
    checkAlerts();
    computeLedState();
    updateLED();

    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        updateOLED(mqttClient.connected());
    }
}