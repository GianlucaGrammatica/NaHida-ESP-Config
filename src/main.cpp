#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include "config.h"

// TODO: quando arrivano i sensori, aggiungere le librerie qui
// #include <DHT.h>
// #define DHT_PIN D4
// #define DHT_TYPE DHT22
// DHT dht(DHT_PIN, DHT_TYPE);

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define LED_PIN D5
#define BTN_PIN D6

#define SENSOR_INTERVAL  60000UL // pubblica sensori ogni 60s
#define PING_INTERVAL    30000UL // ping online ogni 30s

// Setup Display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Setup WiFi & MQTT
WiFiClientSecure espClient;
PubSubClient client(espClient);

// Timer
unsigned long lastSensorPublish = 0;
unsigned long lastPing          = 0;

// Condizioni ottimali ricevute dal server
String plantName   = "In attesa...";
float  humMin      = 0,   humMax     = 100;
float  tempMin     = 0,   tempMax    = 50;
float  soilHumMin  = 0,   soilHumMax = 100;

// Ultime letture sensori
float curHumidity    = 0;
float curTemperature = 0;
float curSoilHum     = 0;

// ---------------------------------------------------------------

void drawDisplay(bool online) {
    display.clearDisplay();

    // Nome piana e stato connessione
    display.setTextSize(1);
    display.setCursor(0, 0);
    String name = plantName.length() > 12 ? plantName.substring(0, 11) + "." : plantName;
    display.print(name);

    display.setCursor(128 - (5 * 6), 0);
    display.print(online ? "* ONL" : "  OFF");

    // Linea
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    // Controllo range ottimali
    bool tempOk = (curTemperature >= tempMin && curTemperature <= tempMax);
    bool humOk  = (curHumidity   >= humMin  && curHumidity   <= humMax);
    bool soilOk = (curSoilHum    >= soilHumMin && curSoilHum <= soilHumMax);

    display.setCursor(0, 14);
    display.print("Temp: "); display.print(curTemperature, 1); display.print("C  ");
    display.println(tempOk ? "OK" : "!!");

    display.setCursor(0, 26);
    display.print("Hum:  "); display.print(curHumidity, 1); display.print("%  ");
    display.println(humOk ? "OK" : "!!");

    display.setCursor(0, 38);
    display.print("Soil: "); display.print(curSoilHum, 1); display.print("%  ");
    display.println(soilOk ? "OK" : "!!");

    // Faccina stato generale
    bool allOk = tempOk && humOk && soilOk;
    display.setCursor(104, 52);
    display.print(allOk ? ":)" : ":(");

    display.display();
}

void setup_wifi() {
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("Connecting WiFi...");
    display.display();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connesso!");
}

void callback(char* topic, byte* payload, unsigned int length) {
    String incomingMsg;
    for (unsigned int i = 0; i < length; i++) {
        incomingMsg += (char)payload[i];
    }

    Serial.println("Topic: " + String(topic));
    Serial.println("Payload: " + incomingMsg);

    String topicStr = String(topic);

    // Comandi semplici (LED, ecc.)
    if (topicStr == String("device/") + DEVICE_TOKEN) {
        if (incomingMsg == "ON") {
            digitalWrite(LED_PIN, HIGH);
        } else if (incomingMsg == "OFF") {
            digitalWrite(LED_PIN, LOW);
        }
        return;
    }

    // Configurazione pianta
    if (topicStr == String("device/") + DEVICE_TOKEN + "/config") {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, incomingMsg);

        if (err) {
            Serial.println("Errore JSON config: " + String(err.c_str()));
            return;
        }

        plantName  = doc["plant_name"].as<String>();
        humMin     = doc["hum_min"]     | 0.0f;
        humMax     = doc["hum_max"]     | 100.0f;
        tempMin    = doc["temp_min"]    | 0.0f;
        tempMax    = doc["temp_max"]    | 50.0f;
        soilHumMin = doc["soil_hum_min"] | 0.0f;
        soilHumMax = doc["soil_hum_max"] | 100.0f;

        Serial.println("Config ricevuta: " + plantName);
        drawDisplay(true);
        return;
    }
}

void reconnect() {
    while (!client.connected()) {
        display.clearDisplay();
        display.setCursor(0, 10);
        display.println("Connecting MQTT...");
        display.display();

        String clientId = String("ESP-") + DEVICE_TOKEN;

        if (client.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
            client.subscribe((String("device/") + DEVICE_TOKEN).c_str(), 1);
            client.subscribe((String("device/") + DEVICE_TOKEN + "/config").c_str(), 1);

            Serial.println("MQTT connesso!");

            // Ping connessione iniziale
            client.publish(
                (String("device/") + DEVICE_TOKEN + "/status").c_str(),
                "ONLINE"
            );
        } else {
            Serial.print("MQTT fallito, rc=");
            Serial.println(client.state());
            delay(5000);
        }
    }
}

void publishPing() {
    if (millis() - lastPing < PING_INTERVAL) return;
    lastPing = millis();

    client.publish(
        (String("device/") + DEVICE_TOKEN + "/status").c_str(),
        "ONLINE"
    );
}

void publishSensorData() {
    if (millis() - lastSensorPublish < SENSOR_INTERVAL) return;
    lastSensorPublish = millis();

    // TODO: sostituire con letture reali quando arrivano i sensori
    // curHumidity    = dht.readHumidity();
    // curTemperature = dht.readTemperature();
    // curSoilHum     = map(analogRead(SOIL_PIN), 0, 1023, 100, 0);
    float luminosity = 0.0;

    String payload = "{";
    payload += "\"type\":\"sensor_data\",";
    payload += "\"humidity\":"      + String(curHumidity,    1) + ",";
    payload += "\"temperature\":"   + String(curTemperature, 1) + ",";
    payload += "\"soil_humidity\":" + String(curSoilHum,     1) + ",";
    payload += "\"luminosity\":"    + String(luminosity,     1);
    payload += "}";

    client.publish(
        (String("device/") + DEVICE_TOKEN + "/updates").c_str(),
        payload.c_str()
    );

    Serial.println("Sensori inviati: " + payload);
}

// ---------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("Display OLED non trovato");
        for (;;);
    }
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("NaHida Starting...");
    display.display();

    espClient.setInsecure();
    setup_wifi();

    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(callback);
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    // Bottone fisico test
    static bool lastBtnState = HIGH;
    bool currentBtnState = digitalRead(BTN_PIN);

    if (lastBtnState == HIGH && currentBtnState == LOW) {
        client.publish(
            (String("device/") + DEVICE_TOKEN + "/updates").c_str(),
            "BUTTON_PRESSED"
        );
        Serial.println("BUTTON_PRESSED inviato");
        delay(200);
    }
    lastBtnState = currentBtnState;

    publishPing();
    publishSensorData();

    // Aggiorna display ogni 5 secondi
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw > 5000) {
        lastDraw = millis();
        drawDisplay(client.connected());
    }
}