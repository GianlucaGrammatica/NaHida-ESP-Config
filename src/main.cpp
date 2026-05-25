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
#include <DFRobotDFPlayerMini.h>
#include <EEPROM.h>
#include "config.h"

// =============================================================
// PIN
// =============================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define LED_PIN     D0  // verde: online e sensori ok
#define LED_RED_PIN D3  // rosso: offline o fuori range (logica invertita: LOW=acceso, resistenza a VCC)
#define BTN_PIN     D4  // bottone annaffiatura
#define DHT_PIN     D5
#define DHT_TYPE    DHT11
#define SOIL_PIN    A0
#define SOIL_DRY    770  // valore ADC terreno secco
#define SOIL_WET    260  // valore ADC terreno bagnato
#define DF_RX       D6   // DFPlayer TX -> ESP RX
#define DF_TX       D7   // ESP TX -> DFPlayer RX (resistenza 1kΩ in serie)

// =============================================================
// TRACCE AUDIO
// =============================================================
#define SND_CONNESSO          1   // connessione MQTT riuscita
#define SND_ACQUA             2   // annaffiatura confermata
#define SND_ALERT             3   // sensore fuori range / errore
#define SND_INTERAZIONE_LUNGO 4   // interazione lunga
#define SND_AVVIO             5   // suono di avvio al boot
// 6-10: musica di sottofondo (riprodotta via MQTT, vedi TODO sotto)
#define SND_DISCONNESSO       11  // disconnessione MQTT

// =============================================================
// EEPROM
// =============================================================
#define EEPROM_SIZE  256
#define EEPROM_MAGIC 0xAB  // sentinella: se diverso l'EEPROM è vuota

// Layout EEPROM:
// 0        uint8_t  magic
// 1..32    char[32] plant name
// 33       float    humMin
// 37       float    humMax
// 41       float    tempMin
// 45       float    tempMax
// 49       float    soilHumMin
// 53       float    soilHumMax
// 57       float    luxMin
// 61       float    luxMax

// =============================================================
// OGGETTI
// =============================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
DHT dht(DHT_PIN, DHT_TYPE);
BH1750 lightMeter;
SoftwareSerial dfSerial(DF_RX, DF_TX, true); // true = logica invertita per transistor BC547
DFRobotDFPlayerMini dfPlayer;

bool dfReady = false;

// =============================================================
// STRUTTURE DATI
// =============================================================
struct PlantConfig {
    String name      = "Waiting...";
    float humMin     = 0,   humMax     = 100;
    float tempMin    = 0,   tempMax    = 50;
    float soilHumMin = 0,   soilHumMax = 100;
    float luxMin     = 0,   luxMax     = 100000;
};

struct SensorReadings {
    float humidity    = 0;
    float temperature = 0;
    float soilHum     = 0;
    float luminosity  = 0;
};

PlantConfig currentConfig;
SensorReadings currentReadings;

// =============================================================
// STATO E TIMERS
// =============================================================
unsigned long lastSensorPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastDebounceTime  = 0;
unsigned long lastAlertCheck    = 0;
unsigned long lastMqttAttempt   = 0;
bool lastButtonState = HIGH;
bool wasConnected    = false;

// =============================================================
// EEPROM: salva e carica la configurazione della pianta
// =============================================================
void saveConfig() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0,  (uint8_t)EEPROM_MAGIC);
    char nameBuf[32] = {};
    currentConfig.name.toCharArray(nameBuf, 32);
    EEPROM.put(1,  nameBuf);
    EEPROM.put(33, currentConfig.humMin);
    EEPROM.put(37, currentConfig.humMax);
    EEPROM.put(41, currentConfig.tempMin);
    EEPROM.put(45, currentConfig.tempMax);
    EEPROM.put(49, currentConfig.soilHumMin);
    EEPROM.put(53, currentConfig.soilHumMax);
    EEPROM.put(57, currentConfig.luxMin);
    EEPROM.put(61, currentConfig.luxMax);
    EEPROM.commit();
    EEPROM.end();
    Serial.println("Config salvata in EEPROM");
}

void loadConfig() {
    EEPROM.begin(EEPROM_SIZE);
    uint8_t magic;
    EEPROM.get(0, magic);
    if (magic != EEPROM_MAGIC) {
        Serial.println("EEPROM vuota, uso valori default");
        EEPROM.end();
        return;
    }
    char nameBuf[32] = {};
    EEPROM.get(1,  nameBuf);
    EEPROM.get(33, currentConfig.humMin);
    EEPROM.get(37, currentConfig.humMax);
    EEPROM.get(41, currentConfig.tempMin);
    EEPROM.get(45, currentConfig.tempMax);
    EEPROM.get(49, currentConfig.soilHumMin);
    EEPROM.get(53, currentConfig.soilHumMax);
    EEPROM.get(57, currentConfig.luxMin);
    EEPROM.get(61, currentConfig.luxMax);
    EEPROM.end();
    currentConfig.name = String(nameBuf);
    Serial.println("Config caricata da EEPROM: " + currentConfig.name);
}

// =============================================================
// AUDIO
// =============================================================
void playSound(int track) {
    if (dfReady) dfPlayer.play(track);
}

// =============================================================
// LED
// Aggiorna i due LED in base allo stato di connessione e sensori
// Gestisce anche i suoni di connessione/disconnessione
// =============================================================
void updateLED() {
    bool isConnected = mqttClient.connected();
    bool online = isConnected && currentConfig.name != "Waiting...";

    if (isConnected && !wasConnected)  playSound(SND_CONNESSO);
    if (!isConnected && wasConnected)  playSound(SND_DISCONNESSO);
    wasConnected = isConnected;

    bool fuoriRange = online && (
        currentReadings.temperature < currentConfig.tempMin    ||
        currentReadings.temperature > currentConfig.tempMax    ||
        currentReadings.humidity    < currentConfig.humMin     ||
        currentReadings.humidity    > currentConfig.humMax     ||
        currentReadings.soilHum     < currentConfig.soilHumMin ||
        currentReadings.soilHum     > currentConfig.soilHumMax ||
        currentReadings.luminosity  < currentConfig.luxMin     ||
        currentReadings.luminosity  > currentConfig.luxMax
    );

    digitalWrite(LED_PIN,     online && !fuoriRange ? HIGH : LOW);
    digitalWrite(LED_RED_PIN, !online || fuoriRange ? LOW  : HIGH);
}

// =============================================================
// DISPLAY
// =============================================================
void updateOLED(bool isOnline) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    // riga 0: nome pianta + stato connessione
    String displayName = currentConfig.name.length() > 12
        ? currentConfig.name.substring(0, 11) + "."
        : currentConfig.name;
    display.setCursor(0, 0);
    display.print(displayName);
    display.setCursor(100, 0);
    display.print(isOnline ? "ON" : "OFF");
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    // righe sensori
    display.setCursor(0, 14); display.printf("Temp: %.1f C",  currentReadings.temperature);
    display.setCursor(0, 26); display.printf("Hum:  %.1f %%", currentReadings.humidity);
    display.setCursor(0, 38); display.printf("Soil: %.1f %%", currentReadings.soilHum);
    display.setCursor(0, 50); display.printf("Lux:  %.0f",    currentReadings.luminosity);

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

// =============================================================
// WIFI
// =============================================================
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

// =============================================================
// MQTT
// =============================================================
void mqttCallback(char* topic, const byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) message += static_cast<char>(payload[i]);
    String topicStr = String(topic);

    // -- config pianta (inviata dal server con retain=true) --
    if (topicStr == String("device/") + DEVICE_TOKEN + "/config") {
        JsonDocument doc;
        if (!deserializeJson(doc, message)) {
            currentConfig.name       = doc["plant_name"].as<String>();
            currentConfig.humMin     = doc["hum_min"]      | 0.0f;
            currentConfig.humMax     = doc["hum_max"]      | 100.0f;
            currentConfig.tempMin    = doc["temp_min"]     | 0.0f;
            currentConfig.tempMax    = doc["temp_max"]     | 50.0f;
            currentConfig.soilHumMin = doc["soil_hum_min"] | 0.0f;
            currentConfig.soilHumMax = doc["soil_hum_max"] | 100.0f;
            currentConfig.luxMin     = doc["lux_min"]      | 0.0f;
            currentConfig.luxMax     = doc["lux_max"]      | 100000.0f;
            saveConfig();
            updateOLED(mqttClient.connected());
        }
    }

    // -- comandi real-time dal server --
    if (topicStr == String("device/") + DEVICE_TOKEN + "/updates") {
        JsonDocument command;
        if (!deserializeJson(command, message)) {
            Serial.println("JSON da comandi arrivato");
            if (command["command"] == String("PLAY_MUSIC")) {
                int source = String(command["source"]).toInt();
                Serial.printf("%s %d", "Play Music with source: ", source);
                playSound(source);
            }
        }
        // Il server invia es. "PLAY_MUSIC:6" per avviare la musica di sottofondo.
        // Estrarre il numero dopo ":" con message.substring(message.indexOf(':') + 1).toInt()
        // e chiamare playSound(trackNumber).
        // Valutare se usare dfPlayer.playMp3Folder() o dfPlayer.play() a seconda
        // di come sono organizzati i file sulla SD.
    }
}

void connectMQTT() {
    if (mqttClient.connected()) return;
    if (millis() - lastMqttAttempt < 5000) return;
    lastMqttAttempt = millis();

    Serial.println("MQTT disconnesso, riconnessione...");
    String clientId = String("ESP-") + DEVICE_TOKEN;
    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println("MQTT connesso");
        mqttClient.subscribe((String("device/") + DEVICE_TOKEN).c_str(), 1);
        mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/config").c_str(), 1);
        mqttClient.subscribe((String("device/") + DEVICE_TOKEN + "/updates").c_str(), 1);
        mqttClient.publish((String("device/") + DEVICE_TOKEN + "/status").c_str(), "ONLINE");
    } else {
        Serial.print("MQTT fallito, rc=");
        Serial.println(mqttClient.state());
    }
}

// =============================================================
// BOTTONE
// =============================================================
void showButtonFeedback() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    if (!mqttClient.connected()) {
        display.setCursor(10, 20);
        display.println("Offline!");
        display.setCursor(10, 32);
        display.println("Riprova dopo.");
    } else {
        display.setCursor(20, 20);
        display.println("Annaffiata!");
    }

    display.display();
    delay(600);
    yield();
    lastDisplayUpdate = millis();
    updateOLED(mqttClient.connected());
}

void handleButton() {
    bool currentButtonState = digitalRead(BTN_PIN);
    if (lastButtonState == HIGH && currentButtonState == LOW) {
        if (millis() - lastDebounceTime > 200) {
            lastDebounceTime = millis();
            if (mqttClient.connected()) {
                mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), "BUTTON_PRESSED");
                Serial.println("Click inviato!");
                playSound(SND_ACQUA);
            } else {
                Serial.println("Click ignorato: offline");
                playSound(SND_ALERT);
            }
            showButtonFeedback();
        }
    }
    lastButtonState = currentButtonState;
}

// =============================================================
// SENSORI
// =============================================================
void readSensors() {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    int rawSoil = analogRead(SOIL_PIN);
    float soilPercent = static_cast<float>(map(rawSoil, SOIL_DRY, SOIL_WET, 0, 100));
    currentReadings.soilHum = constrain(soilPercent, 0.0f, 100.0f);

    float lux = lightMeter.readLightLevel();
    if (lux >= 0) currentReadings.luminosity = lux;

    if (!isnan(h) && !isnan(t)) {
        currentReadings.humidity    = h;
        currentReadings.temperature = t - 2.0f;
    } else {
        Serial.println("DHT11: lettura fallita");
    }
}

void publishTelemetry() {
    if (millis() - lastSensorPublish < 30000) return;
    lastSensorPublish = millis();

    String payload = "{";
    payload += "\"type\":\"sensor_data\",";
    payload += "\"humidity\":"      + String(currentReadings.humidity, 1)    + ",";
    payload += "\"temperature\":"   + String(currentReadings.temperature, 1) + ",";
    payload += "\"soil_humidity\":" + String(currentReadings.soilHum, 1)     + ",";
    payload += "\"luminosity\":"    + String(currentReadings.luminosity, 1);
    payload += "}";
    mqttClient.publish((String("device/") + DEVICE_TOKEN + "/updates").c_str(), payload.c_str());
}

// =============================================================
// ALERT
// Controlla ogni 5 minuti se i sensori sono fuori range
// =============================================================
void checkAlerts() {
    if (millis() - lastAlertCheck < 300000) return;
    lastAlertCheck = millis();
    if (currentConfig.name == "Waiting...") return;

    bool fuoriRange =
        currentReadings.temperature < currentConfig.tempMin    ||
        currentReadings.temperature > currentConfig.tempMax    ||
        currentReadings.humidity    < currentConfig.humMin     ||
        currentReadings.humidity    > currentConfig.humMax     ||
        currentReadings.soilHum     < currentConfig.soilHumMin ||
        currentReadings.soilHum     > currentConfig.soilHumMax ||
        currentReadings.luminosity  < currentConfig.luxMin     ||
        currentReadings.luminosity  > currentConfig.luxMax;

    if (fuoriRange) {
        Serial.println("Alert: sensore fuori range!");
        playSound(SND_ALERT);
    }
}

// =============================================================
// SETUP
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("BOOT");
    Serial.println(ESP.getResetReason());

    pinMode(LED_PIN, OUTPUT);     digitalWrite(LED_PIN, LOW);
    pinMode(LED_RED_PIN, OUTPUT); digitalWrite(LED_RED_PIN, HIGH);
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

    loadConfig();

    dfSerial.begin(9600);
    delay(1000);
    if (dfPlayer.begin(dfSerial, false, false)) {
        dfReady = true;
        dfPlayer.volume(30);
        Serial.println("DFPlayer pronto");
        delay(200);
        playSound(SND_AVVIO);
    } else {
        Serial.println("DFPlayer non trovato, continuo senza audio");
    }

    espClient.setInsecure();
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    setupWiFi();

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setCallback(mqttCallback);
}

// =============================================================
// LOOP
// =============================================================
void loop() {
    connectMQTT();
    mqttClient.loop();

    handleButton();
    readSensors();
    publishTelemetry();
    checkAlerts();
    updateLED();

    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        updateOLED(mqttClient.connected());
    }
}