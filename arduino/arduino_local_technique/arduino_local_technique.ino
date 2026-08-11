/**
 * ============================================================
 *  arduino_atelier.ino
 *  Station de mesure local technique — Arduino MKR WiFi 1010
 * ============================================================
 *
 *  Description :
 *    Ce programme mesure toutes les 60 secondes :
 *      - La température via une sonde NTC (thermistance)
 *      - L'amplitude vibratoire (peak-to-peak filtré) via un capteur piézoélectrique
 *      - L'état du compresseur (entrée digitale)
 *      - La commande compresseur (entrée digitale)
 *    Les données sont envoyées à un serveur HTTP local en JSON via WiFi.
 *
 *    Si le WiFi est indisponible, les mesures sont stockées dans une
 *    file d'attente (jusqu'à 120 entrées) et envoyées groupées dès que
 *    la connexion est rétablie. Un offset temporel permet au serveur
 *    de reconstituer le bon timestamp pour chaque mesure.
 *
 *  Matériel requis :
 *    - Arduino MKR WiFi 1010
 *    - Sonde NTC 
 *    - Capteur piézoélectrique (branché sur A0) + résistance 1M en décharge
 *    - Signal état compresseur sur pin 2
 *    - Signal commande compresseur sur pin 4
 *
 *  Bibliothèques requises (Arduino Library Manager) :
 *    - WiFiNINA
 *    - ArduinoJson (>= v6)
 *
 *  Auteur  : Romaiin 
 *  Version : 2.8
 *  Date    : 07/08/2026
 * ============================================================
 */

#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>


// ============================================================
//  CONFIGURATION
// ============================================================

const char* WIFI_SSID   = "TP-Link_2D2A";
const char* WIFI_PASS   = "35185260";
const char* DEVICE_ID   = "arduino_ntc_01";
const char* SERVER_HOST = "192.168.0.101";
const int   SERVER_PORT = 8000;
const char* SERVER_PATH = "/data";

WiFiClient client;


// ============================================================
//  SONDE NTC — Thermistance 10 kΩ sur pin A2
// ============================================================

const int   sensorPin = A2;

const float Rref = 10000.0;
const float R0   = 10000.0;
const float B    = 3380.0;
const float T0   = 298.15;


// ============================================================
//  ENTRÉES DIGITALES — État et commande du compresseur
// ============================================================

const int pinSignal   = 2;
const int pinCommande = 4;


// ============================================================
//  CAPTEUR PIÉZO — Amplitude peak-to-peak sur pin A0
// ============================================================

const int piezoPin = A0;

const float ALPHA_FILTRE = 0.1;
const unsigned long VIB_WINDOW_MS = 300;

float vibFiltered = 0.0;

int           vibWindowMin;
int           vibWindowMax;
unsigned long vibWindowStart = 0;

float         sommeCarresVib = 0.0;
unsigned long nVib           = 0;


// ============================================================
//  ÉCRAN OLED — I2C, 128x64, adresse 0x3C
// ============================================================

const int SCREEN_WIDTH  = 128;
const int SCREEN_HEIGHT = 64;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool lastServerOk    = false;
bool serverEverTried = false;

void updateDisplay(float temperature, bool wifiOk) {
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.print("WiFi : ");
    display.println(wifiOk ? "Connecte" : "Deconnecte");

    display.setCursor(0, 24);
    display.setTextSize(2);
    display.print(temperature, 1);
    display.println(" C");

    display.setTextSize(1);
    display.setCursor(0, 54);
    display.print("Serveur : ");
    if (!serverEverTried) {
        display.println("En attente");
    } else {
        display.println(lastServerOk ? "OK" : "Erreur");
    }

    display.display();
}


// ============================================================
//  TIMERS
// ============================================================

const unsigned long MEASURE_INTERVAL    = 60000;  // Acquisition toutes les 60 s
const unsigned long SEND_DELAY          = 30000;  // Envoi 30 s après l'acquisition
const unsigned long TEMP_SAMPLE_INTERVAL_MS = 100;
const unsigned long WIFI_RETRY_INTERVAL = 15000;

unsigned long lastMeasure     = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastWifiRetry   = 0;
unsigned long t_temp          = 0;

bool pendingSend = false;


// ============================================================
//  MOYENNE TEMPÉRATURE — Accumulée sur 60 s
// ============================================================

float sommeT = 0.0;
int   n      = 0;


// ============================================================
//  FILE D'ATTENTE
// ============================================================
// Logique des offsets (identique à arduino_atmo) :
// t=0  → acquisition → offset 0  → mis en queue
// t=60 → nouvelle acquisition → offset 0 → mis en queue
//         l'ancienne mesure vieillit automatiquement → offset -1
// t=90 → envoi de toute la queue avec les bons offsets

#define QUEUE_MAX 120

struct QueuedMeasure {
    int   offset;
    float temperature;
    float amplitude;
    int   etat;
    int   commande;
};

QueuedMeasure queue[QUEUE_MAX];
int queueSize = 0;


void addToQueue(float temp, float amplitude, int etat, int commande) {
    // Vieillit tous les offsets existants de -1 minute
    for (int i = 0; i < queueSize; i++) {
        queue[i].offset--;
    }

    // Si pleine, supprime la plus ancienne
    if (queueSize >= QUEUE_MAX) {
        for (int i = 0; i < QUEUE_MAX - 1; i++) {
            queue[i] = queue[i + 1];
        }
        queueSize = QUEUE_MAX - 1;
        Serial.println("[QUEUE] Pleine — ancienne mesure supprimee.");
    }

    // Nouvelle mesure avec offset 0
    queue[queueSize] = { 0, temp, amplitude, etat, commande };
    queueSize++;

    Serial.print("[QUEUE] ");
    Serial.print(queueSize);
    Serial.println(" mesure(s) en attente.");
}


void clearQueue() {
    queueSize = 0;
    Serial.println("[QUEUE] File videe.");
}


// ============================================================
//  GESTION WIFI — Reconnexion non bloquante
// ============================================================

void resetWiFiModule() {
    Serial.println("[WIFI] Reset du module...");
    client.stop();
    WiFi.disconnect();
    WiFi.end();
    delay(3000);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Reset — reconnexion lancee.");
}

void tryReconnectWiFi() {
    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

bool manageWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    unsigned long now = millis();
    if (now - lastWifiRetry < WIFI_RETRY_INTERVAL) return false;
    lastWifiRetry = now;

    static int retryCount = 0;
    retryCount++;

    Serial.print("[WIFI] Deconnecte. Tentative #");
    Serial.println(retryCount);

    if (retryCount >= 4) {
        resetWiFiModule();
        retryCount = 0;
    } else {
        tryReconnectWiFi();
    }

    return false;
}


// ============================================================
//  LECTURE TEMPÉRATURE — Échantillon NTC toutes les 100 ms
// ============================================================

void readTemperatureSample() {
    int adc = analogRead(sensorPin);

    if (adc <= 0 || adc >= 4095) return;

    float Rntc = Rref * adc / (4095.0 - adc);
    float lnR  = log(Rntc / R0);
    float T    = 1.0 / (1.0 / T0 + lnR / B);
    float Tc   = T - 273.15;

    sommeT += Tc;
    n++;
}


// ============================================================
//  LECTURE VIBRATION — Peak-to-peak filtré, fenêtres de 300 ms
// ============================================================

void readVibrationSample() {
    int raw = analogRead(piezoPin);

    vibFiltered = ALPHA_FILTRE * raw + (1 - ALPHA_FILTRE) * vibFiltered;

    if (vibFiltered < vibWindowMin) vibWindowMin = vibFiltered;
    if (vibFiltered > vibWindowMax) vibWindowMax = vibFiltered;

    unsigned long now = millis();
    if (now - vibWindowStart >= VIB_WINDOW_MS) {
        int peakToPeak = vibWindowMax - vibWindowMin;

        sommeCarresVib += peakToPeak;
        nVib++;

        vibWindowMin   = 4095;
        vibWindowMax   = 0;
        vibWindowStart = now;
    }
}


// ============================================================
//  ENVOI HTTP — Envoie uniquement la queue
// ============================================================

bool sendQueue() {
    if (queueSize == 0) return true;

    client.stop();
    delay(100);

    Serial.println("[HTTP] Connexion serveur...");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("[HTTP] Echec connexion serveur.");
        client.stop();
        return false;
    }

    StaticJsonDocument<16384> doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < queueSize; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["device_id"]            = DEVICE_ID;
        obj["offset"]               = queue[i].offset;
        obj["temperature"]          = queue[i].temperature;
        obj["amplitude"]            = queue[i].amplitude;
        obj["comptfetat"]           = queue[i].etat;
        obj["commande_compresseur"] = queue[i].commande;
    }

    char buffer[16384];
    size_t len = serializeJson(doc, buffer);

    Serial.print("[HTTP] Envoi de ");
    Serial.print(queueSize);
    Serial.println(" mesure(s)...");

    client.print("POST ");
    client.print(SERVER_PATH);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(SERVER_HOST);
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(len);
    client.println("Connection: close");
    client.println();
    client.write((const uint8_t*)buffer, len);

    unsigned long timeout = millis();
    String responseLine   = "";
    bool firstLine        = true;
    bool success          = false;

    while (millis() - timeout < 10000) {
        while (client.available()) {
            char c = client.read();
            Serial.write(c);
            if (firstLine) {
                if (c == '\n') {
                    firstLine = false;
                    if (responseLine.indexOf("200") >= 0 ||
                        responseLine.indexOf("201") >= 0) {
                        success = true;
                    }
                } else {
                    responseLine += c;
                }
            }
            timeout = millis();
        }
        if (!client.connected()) break;
    }

    client.stop();
    Serial.println();
    Serial.println(success ? "[HTTP] Envoi reussi." : "[HTTP] Echec envoi.");
    return success;
}


// ============================================================
//  SETUP
// ============================================================

void setup() {
    Serial.begin(115200);

    analogReadResolution(12);

    pinMode(pinSignal,   INPUT);
    pinMode(pinCommande, INPUT);

    vibFiltered    = analogRead(piezoPin);
    vibWindowMin   = 4095;
    vibWindowMax   = 0;
    vibWindowStart = millis();

    t_temp = millis();

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] Ecran non detecte.");
    } else {
        display.clearDisplay();
        display.display();
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Connexion lancee en arriere-plan...");

    // Acquisition immédiate au démarrage
    lastMeasure = millis() - MEASURE_INTERVAL;
}


// ============================================================
//  LOOP
// ============================================================

void loop() {
    unsigned long now = millis();


    // ── 1. WiFi en arrière-plan ──────────────────────────────
    if (WiFi.status() == WL_CONNECTED) {
        static bool announced = false;
        if (!announced) {
            Serial.print("[WIFI] Connecte. IP : ");
            Serial.println(WiFi.localIP());
            announced = true;
        }
    } else {
        static bool announced = false;
        announced = false;
        manageWiFi();
    }


    // ── 2. Échantillonnage vibration (peak-to-peak, fenêtres 300 ms) ──
    readVibrationSample();


    // ── 3. Échantillonnage température toutes les 100 ms ─────
    if (now - t_temp >= TEMP_SAMPLE_INTERVAL_MS) {
        t_temp = now;
        readTemperatureSample();
    }


    // ── 4. Acquisition — t=0, t=60, t=120... ─────────────────
    // Mise en queue avec offset 0.
    // Les mesures précédentes voient leur offset décrémenter dans addToQueue().
    if (now - lastMeasure >= MEASURE_INTERVAL) {
        lastMeasure     = now;
        lastSendAttempt = now;
        pendingSend     = true;

        float moyenneTemp = NAN;
        if (n > 0) moyenneTemp = sommeT / n;

        float amplitudeVibration = 0.0;
        if (nVib > 0) amplitudeVibration = sommeCarresVib / nVib;

        int etat     = digitalRead(pinSignal);
        int commande = digitalRead(pinCommande);

        Serial.print("[MESURE] T=");
        Serial.print(moyenneTemp, 1);
        Serial.print("C  Amplitude=");
        Serial.print(amplitudeVibration, 2);
        Serial.print("  Etat=");
        Serial.print(etat);
        Serial.print("  Cmd=");
        Serial.println(commande);

        addToQueue(moyenneTemp, amplitudeVibration, etat, commande);

        updateDisplay(moyenneTemp, WiFi.status() == WL_CONNECTED);

        // Reset des accumulateurs
        sommeT         = 0.0;
        n              = 0;
        sommeCarresVib = 0.0;
        nVib           = 0;
    }


    // ── 5. Envoi — t=30, t=90, t=150... ──────────────────────
    // 30 s après chaque acquisition, on tente d'envoyer toute la queue.
    // Si succès → queue vidée.
    // Si échec → queue conservée, les offsets vieilliront au prochain cycle.
    if (pendingSend && now - lastSendAttempt >= SEND_DELAY) {
        pendingSend = false;

        if (WiFi.status() == WL_CONNECTED) {
            bool ok = sendQueue();
            serverEverTried = true;
            lastServerOk    = ok;

            if (ok) {
                clearQueue();
            } else {
                Serial.println("[SEND] Echec — queue conservee.");
            }
        } else {
            Serial.println("[SEND] WiFi indisponible — envoi reporte.");
        }
    }
}
