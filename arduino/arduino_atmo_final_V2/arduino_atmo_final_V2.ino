#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <Arduino_MKRENV.h>

// --------------------------
// CONFIGURATION GENERALE
// --------------------------
const char* WIFI_SSID   = "TP-Link_2D2A";
const char* WIFI_PASS   = "35185260";
const char* DEVICE_ID   = "arduino_atmo";
const char* SERVER_HOST = "192.168.0.101";
const int   SERVER_PORT = 8000;
const char* SERVER_PATH = "/data";

// --------------------------
// TIMERS
// --------------------------
const unsigned long MEASURE_INTERVAL    = 60000; // acquisition toutes les 60s
const unsigned long SEND_DELAY          = 30000; // envoi 30s après l'acquisition
const unsigned long WIFI_RETRY_INTERVAL = 15000; // tentative WiFi toutes les 15s

unsigned long lastMeasure     = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastWifiRetry   = 0;

bool pendingSend = false;

WiFiClient client;

// --------------------------
// FILE D'ATTENTE
// --------------------------
// Logique des offsets :
// t=0  → acquisition → offset 0  → mis en queue
// t=60 → nouvelle acquisition → offset 0 → mis en queue
//         l'ancienne mesure vieillit automatiquement → offset -1
// t=90 → envoi de toute la queue avec les bons offsets
#define QUEUE_MAX 120

struct QueuedMeasure {
    int   offset;
    float temperature;
    float humidity;
    float pressure;
};

QueuedMeasure queue[QUEUE_MAX];
int queueSize = 0;

void addToQueue(float t, float h, float p) {
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

    // Nouvelle mesure avec offset 0 = vient d'être acquise
    queue[queueSize] = { 0, t, h, p };
    queueSize++;

    Serial.print("[QUEUE] ");
    Serial.print(queueSize);
    Serial.println(" mesure(s) en attente.");
}

void clearQueue() {
    queueSize = 0;
    Serial.println("[QUEUE] File videe.");
}

// --------------------------
// WIFI — NON BLOQUANT
// --------------------------
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

// --------------------------
// ENVOI HTTP — envoie uniquement la queue
// --------------------------
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

    // On envoie la queue telle quelle — les offsets sont déjà corrects
    StaticJsonDocument<12288> doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < queueSize; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["device_id"]   = DEVICE_ID;
        obj["offset"]      = queue[i].offset;
        obj["temperature"] = queue[i].temperature;
        obj["humidity"]    = queue[i].humidity;
        obj["pressure"]    = queue[i].pressure;
    }

    char buffer[12288];
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

// --------------------------
// SETUP
// --------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    if (!ENV.begin()) {
        Serial.println("[ENV] Erreur ENV Shield.");
        while (true);
    }
    Serial.println("[ENV] ENV Shield OK");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Connexion lancee en arriere-plan...");

    // Acquisition immédiate au démarrage
    lastMeasure = millis() - MEASURE_INTERVAL;
}

// --------------------------
// LOOP
// --------------------------
void loop() {
    unsigned long now = millis();

    // ── 1. WiFi en arrière-plan ───────────────────────────
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

    // ── 2. Acquisition — t=0, t=60, t=120... ─────────────
    // Mise en queue avec offset 0.
    // Les mesures précédentes voient leur offset décrémenter dans addToQueue().
    // Exemple après 2 acquisitions sans envoi :
    //   queue[0] = { offset: -1, ... }  ← acquise il y a 1 min
    //   queue[1] = { offset:  0, ... }  ← acquise maintenant
    if (now - lastMeasure >= MEASURE_INTERVAL) {
        lastMeasure     = now;
        lastSendAttempt = now; // démarre le compte à rebours de 30s
        pendingSend     = true;

        float temperature = ENV.readTemperature();
        float humidity    = ENV.readHumidity();
        float pressure    = ENV.readPressure() * 10.0; // kPa → hPa

        Serial.print("[MESURE] T=");
        Serial.print(temperature, 1);
        Serial.print("C  H=");
        Serial.print(humidity, 1);
        Serial.print("%  P=");
        Serial.print(pressure, 1);
        Serial.println(" hPa");

        addToQueue(temperature, humidity, pressure);
    }

    // ── 3. Envoi — t=30, t=90, t=150... ──────────────────
    // 30s après chaque acquisition, on tente d'envoyer toute la queue.
    // Si succès → queue vidée.
    // Si échec → queue conservée, les offsets vieilliront au prochain cycle.
    if (pendingSend && now - lastSendAttempt >= SEND_DELAY) {
        pendingSend = false;

        if (WiFi.status() == WL_CONNECTED) {
            bool ok = sendQueue();
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
