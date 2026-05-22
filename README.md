# NaHida ESP Firmware

Firmware per NodeMCU ESP8266 del progetto NaHida, un monitor IoT per piante con sensori ambientali, display OLED, feedback audio e integrazione MQTT via HiveMQ Cloud.

---

## Requisiti

- PlatformIO (VS Code o standalone)
- NodeMCU v2 (ESP8266)
- File `include/config.h` compilato a partire da `config.h.example`

---

## Setup

### 1. Clona il repo e configura

Copia `include/config.h.example` in `include/config.h` e compila con le tue credenziali:

```h
const char* WIFI_SSID    = "tua-rete";
const char* WIFI_PASS    = "tua-password";
const char* MQTT_SERVER  = "xxx.hivemq.cloud";
const char* MQTT_USER    = "utente";
const char* MQTT_PASS    = "password";
const int   MQTT_PORT    = 8883;
const char* DEVICE_TOKEN = "token-dispositivo";
```

> `config.h` è nel `.gitignore`, non pusharlo mai.

### 2. Installa le dipendenze e flasha

```bash
pio run --target upload
```

> Se l'upload va in timeout, stacca i cavi del DFPlayer (D6, D7) prima di flashare e ricollegali dopo.

---

## Pinout

| Pin ESP | Uso |
|---------|-----|
| D0 | LED stato (acceso = connesso + config ricevuta) |
| D1 | I2C SCL (OLED + BH1750) |
| D2 | I2C SDA (OLED + BH1750) |
| D4 | Bottone annaffiatura |
| D5 | DHT11 (temperatura + umidità aria) |
| D6 | DFPlayer RX (ESP riceve dal TX del player) |
| D7 | DFPlayer TX (ESP trasmette verso RX del player) — via transistor BC547 |
| A0 | Sensore umidità suolo (analogico) |

### Nota sul DFPlayer

Il modulo MP3-TF-16P clone lavora a 5V mentre l'ESP trasmette a 3.3V. Per fare il level shifting si usa un transistor **BC547** sul pin TX (D7):

```
D7 (ESP) ---[1kΩ]--- Base (BC547)
                     Collector --- RX del DFPlayer --- [10kΩ] --- 5V (VV)
                     Emitter  --- GND
```

Il SoftwareSerial è inizializzato con `inverse_logic = true` per compensare l'inversione del segnale introdotta dal transistor.

Il DFPlayer va alimentato a **5V (pin VV dell'ESP)**, non a 3.3V.

### SD card

Formato FAT32, file nella cartella `mp3`. I file vanno copiati nell'ordine numerico, uno alla volta:

```
mp3/
├── 0001.mp3   suono breve interazione (basso volume, probabilmente inutilizzato)
├── 0002.mp3   suono interazione standard / annaffiata
├── 0003.mp3   suono allerta
├── 0004.mp3   suono interazione lungo
├── 0005.mp3   suono avvio
├── 0006.mp3   musica: Lily & Daisy
├── 0007.mp3   musica: City Lights
├── 0008.mp3   musica: Dreamy Days
├── 0009.mp3   musica: Sunny Symphony
└── 0010.mp3   musica: Jazzberry Jam
```

File mp3: mono, 128kbps, 44100Hz.

> Il DFPlayer usa l'ordine di copia sulla SD, non il nome del file. Copia sempre un file alla volta nell'ordine corretto.

---

## Architettura del codice

Il firmware è tutto in `src/main.cpp`, organizzato in sezioni:

### Structs

`PlantConfig` contiene la configurazione della pianta ricevuta via MQTT (nome, range temperatura, umidità aria, umidità suolo). Viene aggiornata ogni volta che arriva un messaggio sul topic `/config`.

`SensorReadings` contiene le ultime letture dei sensori, aggiornate nel loop ogni ciclo.

### Loop principale

Il loop gira senza `delay` bloccanti (tranne nel feedback del bottone). Ogni funzione usa un timer basato su `millis()`:

| Funzione | Frequenza |
|----------|-----------|
| `readSensors()` | ogni ciclo (~centinaia di ms) |
| `publishTelemetry()` | ogni 60 secondi |
| `checkAlerts()` | ogni 5 minuti |
| `updateOLED()` | ogni 2 secondi |
| `updateLED()` | ogni ciclo |

### MQTT

Il dispositivo si connette a HiveMQ Cloud su TLS porta 8883 con `espClient.setInsecure()` (no verifica certificato).

Topic sottoscritti:

| Topic | Contenuto |
|-------|-----------|
| `device/{token}` | comandi diretti (non usato attivamente) |
| `device/{token}/config` | configurazione JSON della pianta |
| `device/{token}/updates` | aggiornamenti (sottoscritto ma non usato in ricezione) |

Topic pubblicati:

| Topic | Contenuto |
|-------|-----------|
| `device/{token}/updates` | telemetria JSON ogni 60s + `BUTTON_PRESSED` |
| `device/{token}/status` | `ONLINE` alla connessione |

Payload telemetria:
```json
{
  "type": "sensor_data",
  "humidity": 55.3,
  "temperature": 22.1,
  "soil_humidity": 67.0,
  "luminosity": 212
}
```

### Riconnessione MQTT

`connectMQTT()` non è bloccante: riprova ogni 5 secondi senza bloccare il loop. Il WiFi invece non ha riconnessione automatica nel loop (la funzione `checkWiFi()` esiste ma non è chiamata per evitare blocchi).

### Audio

`playSound(track)` chiama `dfPlayer.play(track)` solo se `dfReady` è true. Se il DFPlayer non viene rilevato all'avvio, il sistema continua senza audio senza bloccarsi.

### Calibrazione sensore suolo

I valori `SOIL_DRY` (880) e `SOIL_WET` (390) sono la lettura grezza di A0 rispettivamente in aria e in acqua. Vanno ricalibrati se si cambia sensore.

---

## Dipendenze

```ini
adafruit/Adafruit SSD1306 @ ^2.5.7
adafruit/Adafruit GFX Library @ ^1.11.5
adafruit/DHT sensor library @ ^1.4.6
adafruit/Adafruit Unified Sensor @ ^1.1.14
knolleary/PubSubClient @ ^2.8
bblanchon/ArduinoJson @ ^7.0.0
claws/BH1750 @ ^1.3.0
DFRobotDFPlayerMini
```
