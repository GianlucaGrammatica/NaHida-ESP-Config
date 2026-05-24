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

### 2. Flasha

```bash
pio run --target upload
```

> Se l'upload va in timeout, stacca i cavi del DFPlayer (D6, D7) prima di flashare e ricollegali dopo.

---

## Pinout

| Pin ESP | Uso |
|---------|-----|
| D0 | LED verde (acceso = connesso + config ricevuta + sensori in range) |
| D3 | LED rosso (acceso = offline o sensore fuori range; logica invertita, LOW = acceso) |
| D4 | Bottone annaffiatura |
| D5 | DHT11 (temperatura + umidità aria) |
| D6 | DFPlayer RX (ESP riceve dal TX del player) |
| D7 | DFPlayer TX (ESP trasmette verso RX del player, via transistor BC547) |
| D1 | I2C SCL (OLED + BH1750) |
| D2 | I2C SDA (OLED + BH1750) |
| A0 | Sensore umidità suolo (analogico) |

### LED rosso

Il LED rosso è collegato con resistenza a VCC (pull-up), quindi la logica è invertita: `LOW` = acceso, `HIGH` = spento. Si accende quando il dispositivo è offline oppure quando almeno un sensore è fuori range.

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
├── 0001.mp3   connessione MQTT riuscita
├── 0002.mp3   annaffiatura confermata
├── 0003.mp3   sensore fuori range / errore
├── 0004.mp3   interazione lunga
├── 0005.mp3   avvio
├── 0006.mp3   musica: Lily & Daisy
├── 0007.mp3   musica: City Lights
├── 0008.mp3   musica: Dreamy Days
├── 0009.mp3   musica: Sunny Symphony
├── 0010.mp3   musica: Jazzberry Jam
└── 0011.mp3   disconnessione MQTT
```

File mp3: mono, 128kbps, 44100Hz.

> Il DFPlayer usa l'ordine di copia sulla SD, non il nome del file. Copia sempre un file alla volta nell'ordine corretto.

---

## Architettura del codice

Il firmware è tutto in `src/main.cpp`, organizzato in sezioni.

### Structs

`PlantConfig` contiene la configurazione della pianta ricevuta via MQTT: nome, range di temperatura, umidità aria, umidità suolo e luminosità. Viene aggiornata ogni volta che arriva un messaggio sul topic `/config` e salvata in EEPROM.

`SensorReadings` contiene le ultime letture dei quattro sensori, aggiornate ogni ciclo del loop.

### EEPROM

La configurazione della pianta viene salvata in EEPROM alla ricezione di ogni messaggio `/config`, così sopravvive ai riavvii. Al boot `loadConfig()` controlla il byte sentinella `0xAB` all'indirizzo 0: se presente carica i valori salvati, altrimenti usa i default.

Layout:

| Indirizzo | Tipo | Campo |
|-----------|------|-------|
| 0 | `uint8_t` | magic (`0xAB`) |
| 1..32 | `char[32]` | nome pianta |
| 33 | `float` | humMin |
| 37 | `float` | humMax |
| 41 | `float` | tempMin |
| 45 | `float` | tempMax |
| 49 | `float` | soilHumMin |
| 53 | `float` | soilHumMax |
| 57 | `float` | luxMin |
| 61 | `float` | luxMax |

### Loop principale

Il loop gira senza `delay` bloccanti (tranne nel feedback del bottone). Ogni funzione usa un timer basato su `millis()`:

| Funzione | Frequenza |
|----------|-----------|
| `readSensors()` | ogni ciclo |
| `publishTelemetry()` | ogni 30 secondi *(ridotto per test)* |
| `checkAlerts()` | ogni 5 minuti |
| `updateOLED()` | ogni 2 secondi |
| `updateLED()` | ogni ciclo |

### MQTT

Il dispositivo si connette a HiveMQ Cloud su TLS porta 8883 con `espClient.setInsecure()` (nessuna verifica del certificato).

Topic sottoscritti:

| Topic | Contenuto |
|-------|-----------|
| `device/{token}` | comandi diretti (non usato attivamente) |
| `device/{token}/config` | configurazione JSON della pianta (retain=true) |
| `device/{token}/updates` | comandi real-time dal server (TODO: PLAY_MUSIC) |

Topic pubblicati:

| Topic | Contenuto |
|-------|-----------|
| `device/{token}/updates` | telemetria JSON ogni 30s + `BUTTON_PRESSED` al click |
| `device/{token}/status` | `ONLINE` alla connessione |

Payload telemetria:

```json
{
  "type": "sensor_data",
  "humidity": 55.3,
  "temperature": 22.1,
  "soil_humidity": 67.0,
  "luminosity": 212.0
}
```

### Riconnessione MQTT

`connectMQTT()` non è bloccante: riprova ogni 5 secondi senza fermare il loop. Il WiFi usa `setAutoReconnect(true)` di sistema, senza logica di riconnessione manuale nel loop.

### Audio

`playSound(track)` chiama `dfPlayer.play(track)` solo se `dfReady` è true. Se il DFPlayer non viene rilevato all'avvio il sistema continua normalmente senza audio. La traccia 11 (`SND_DISCONNESSO`) non è in sequenza con le altre per lasciare i file 6-10 liberi per la musica di sottofondo.

### Calibrazione sensore suolo

I valori `SOIL_DRY` (770) e `SOIL_WET` (260) sono la lettura grezza di A0 rispettivamente con il sensore in aria e in acqua. Vanno ricalibrati se si cambia sensore.

La temperatura letta dal DHT11 viene corretta di -2°C (`t - 2.0f`) per compensare il calore generato dall'ESP stesso.

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