/**
 * ============================================================
 *  arduino_atelier.ino
 *  Station de mesure local technique — Arduino MKR WiFi 1010
 * ============================================================
 *
 *  Description :
 *    Ce programme mesure toutes les 60 secondes :
 *      - La temperature via une sonde NTC (thermistance)
 *      - L'amplitude vibratoire (peak-to-peak filtre) via un capteur piezoelectrique
 *      - L'etat du compresseur (entree digitale)
 *      - La commande compresseur (entree digitale)
 *    Les donnees sont envoyees a un serveur HTTP local en JSON via WiFi.
 *    Un ecran OLED affiche en local l'etat WiFi, la temperature et
 *    l'etat de la derniere communication avec le serveur.
 *
 *    Si le WiFi est indisponible, les mesures sont stockees dans une
 *    file d'attente (jusqu'a 120 entrees) et envoyees groupees des que
 *    la connexion est retablie. Un offset temporel permet au serveur
 *    de reconstituer le bon timestamp pour chaque mesure.
 *
 *    Le code integre egalement les memes mecanismes de resilience que
 *    les autres modules du projet Monitocrio (arduino_atmo, arduino_cta) :
 *    file d'attente persistante en RAM survivant aux reboots, sonde TCP
 *    de joignabilite du serveur, reset materiel cible de la puce WiFi
 *    NINA-W102 en cas de blocage prolonge, reboot complet en tout
 *    dernier recours, et watchdog materiel anti-blocage. L'objectif est
 *    un fonctionnement autonome et fiable sans intervention manuelle,
 *    meme en cas de coupure reseau ou de dysfonctionnement de la NINA.
 *
 *  Materiel requis :
 *    - Arduino MKR WiFi 1010
 *    - Sonde NTC
 *    - Capteur piezoelectrique (branche sur A0) + resistance 1M en decharge
 *    - Signal etat compresseur sur pin 2
 *    - Signal commande compresseur sur pin 4
 *    - Ecran OLED SSD1306 128x64 I2C, adresse 0x3C
 *
 *  Bibliotheques requises (Arduino Library Manager) :
 *    - WiFiNINA
 *    - ArduinoJson (>= v6)
 *    - Adafruit_SleepyDog (watchdog materiel)
 *    - Wire, Adafruit_GFX, Adafruit_SSD1306
 *
 *  Auteur  : Romaiin
 *  Version : 3.0
 *  Date    : 11/08/2026
 * ============================================================
 */

#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SleepyDog.h>

/* Broche de reset materiel de la puce NINA-W102.
   Normalement definie par le core (pins_arduino.h du MKR WiFi 1010),
   valeur confirmee = 31, active a l'etat bas. Le define de secours
   ci-dessous evite toute surprise si jamais le core ne l'exposait pas. */
#ifndef NINA_RESETN
#define NINA_RESETN 31
#endif


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

const unsigned long MEASURE_INTERVAL        = 60000;  // Acquisition toutes les 60 s
const unsigned long SEND_DELAY              = 30000;  // Envoi 30 s apres l'acquisition
const unsigned long TEMP_SAMPLE_INTERVAL_MS = 100;
const unsigned long WIFI_RETRY_INTERVAL     = 15000;  // tentative WiFi toutes les 15s
const unsigned long WATCHDOG_TIMEOUT        = 600000; // 10 min sans envoi reussi -> reset NINA

unsigned long lastMeasure        = 0;
unsigned long lastSendAttempt    = 0;
unsigned long lastWifiRetry      = 0;
unsigned long t_temp             = 0;
unsigned long lastSuccessfulSend = 0;

bool pendingSend      = false;
bool wifiWasConnected = false;

/* Nombre de resets NINA consecutifs sans envoi reussi. Au-dela de
   MAX_NINA_RESETS, on tente un reboot complet en tout dernier recours.
   Remis a zero a chaque envoi reussi (et au demarrage du programme). */
#define MAX_NINA_RESETS 3
int ninaResetCount = 0;


// ============================================================
//  MOYENNE TEMPÉRATURE — Accumulée sur 60 s
// ============================================================

float sommeT = 0.0;
int   n      = 0;


// ============================================================
//  FILE D'ATTENTE — CONSERVEE AU REBOOT
// ============================================================
/*
 * La file est placee en section .noinit : le code de demarrage du SAMD21
 * ne la remet PAS a zero au boot. Tant que l'alimentation tient (reboot
 * watchdog, NVIC_SystemReset, bouton reset), la RAM garde son contenu.
 * Seule une vraie coupure de courant l'efface.
 *
 * On distingue un reboot a chaud (file valide a conserver) d'un demarrage
 * a froid (RAM = poubelle) avec un nombre magique + un checksum FNV-1a.
 *
 * Logique des offsets (identique a arduino_atmo / arduino_cta) :
 * t=0  → acquisition → offset 0  → mis en queue
 * t=60 → nouvelle acquisition → offset 0 → mis en queue
 *         l'ancienne mesure vieillit automatiquement → offset -1
 * t=90 → envoi de toute la queue avec les bons offsets
 */

#define QUEUE_MAX   120
#define QUEUE_MAGIC 0xC0FFEE42UL

struct QueuedMeasure {
    int   offset;
    float temperature;
    float amplitude;
    int   etat;
    int   commande;
};

__attribute__((section(".noinit"))) uint32_t      rtMagic;
__attribute__((section(".noinit"))) uint32_t      rtChecksum;
__attribute__((section(".noinit"))) uint32_t      rtRebootCount;
__attribute__((section(".noinit"))) int           queueSize;
__attribute__((section(".noinit"))) QueuedMeasure queue[QUEUE_MAX];

/*
 * Checksum FNV-1a sur queueSize + les entrees reellement utilisees.
 * Sert a valider que la file conservee en RAM n'est pas corrompue
 * apres un reboot (et a rejeter une RAM poubelle au demarrage a froid).
 */
uint32_t computeQueueChecksum() {
    if (queueSize < 0 || queueSize > QUEUE_MAX) return 0;
    uint32_t h = 2166136261UL;
    const uint8_t* p = (const uint8_t*)&queueSize;
    for (size_t i = 0; i < sizeof(queueSize); i++) {
        h ^= p[i];
        h *= 16777619UL;
    }
    p = (const uint8_t*)queue;
    size_t n2 = (size_t)queueSize * sizeof(QueuedMeasure);
    for (size_t i = 0; i < n2; i++) {
        h ^= p[i];
        h *= 16777619UL;
    }
    return h;
}

/* Met a jour magie + checksum apres chaque modification de la file. */
void saveQueue() {
    rtMagic    = QUEUE_MAGIC;
    rtChecksum = computeQueueChecksum();
}

/* Vrai si la RAM contient une file valide issue d'un reboot a chaud. */
bool warmBootQueueValid() {
    if (rtMagic != QUEUE_MAGIC)                 return false;
    if (queueSize < 0 || queueSize > QUEUE_MAX) return false;
    if (rtChecksum != computeQueueChecksum())   return false;
    return true;
}

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
    saveQueue();

    Serial.print("[QUEUE] ");
    Serial.print(queueSize);
    Serial.println(" mesure(s) en attente.");
}

void clearQueue() {
    queueSize = 0;
    saveQueue();
    Serial.println("[QUEUE] File videe.");
}


// ============================================================
//  WIFI — NON BLOQUANT
// ============================================================

void resetWiFiModule() {
    Serial.println("[WIFI] Reset logiciel du module...");
    client.stop();
    WiFi.disconnect();
    WiFi.end();
    delay(3000);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiWasConnected = false;
    Serial.println("[WIFI] Reset — reconnexion lancee.");
}

void tryReconnectWiFi() {
    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

/*
 * Reboot complet du SAMD21 (tout dernier recours).
 *
 * ATTENTION : NVIC_SystemReset() ne coupe PAS l'alimentation de la NINA
 * et ne la sort donc pas d'un etat fige — c'est exactement pour ca qu'on
 * prefere resetNINA() en recuperation principale. On ne garde ce reboot
 * que comme ultime filet de securite si plusieurs resets NINA ont echoue.
 * La file en .noinit est conservee.
 */
void rebootBoard(const char* raison) {
    Serial.print("[REBOOT] ");
    Serial.println(raison);
    Serial.flush();
    delay(50);
    NVIC_SystemReset();
}

/*
 * Reset MATERIEL de la seule puce NINA-W102, sans toucher au SAMD21.
 *
 * La broche NINA_RESETN (31) est active a l'etat bas : on la tire a LOW
 * pour couper la NINA, puis a HIGH pour la relancer. C'est un simple
 * toggle de GPIO : ca ne peut jamais bloquer, contrairement a WiFi.end()
 * qui dialogue en SPI avec la NINA et resterait coince si elle est figee.
 *
 * Le SAMD21 ne reboote pas : la file en .noinit reste donc intacte, sans
 * meme solliciter le mecanisme magic-number / checksum. C'est l'equivalent
 * logiciel du power-cycle physique fait a la main.
 */
void resetNINA() {
    Serial.println("[NINA] Reset materiel de la puce WiFi...");
    Watchdog.reset();

    client.stop();

    pinMode(NINA_RESETN, OUTPUT);
    digitalWrite(NINA_RESETN, LOW);
    delay(100);
    digitalWrite(NINA_RESETN, HIGH);
    delay(750);

    Watchdog.reset();

    wifiWasConnected = false;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[NINA] Reset effectue — reconnexion WiFi lancee.");

    Watchdog.reset();
}

/*
 * Verifie la connectivite WiFi.
 *
 * Probleme connu sur MKR WiFi 1010 / puce NINA :
 * WiFi.status() peut rester WL_CONNECTED alors que la connexion
 * est morte. On le detecte ailleurs en tentant une connexion TCP
 * reelle au serveur.
 *
 * Retourne true si WiFi.status() annonce la connexion.
 */
bool manageWiFi() {
    int status = WiFi.status();
    if (status == WL_CONNECTED) {
        if (!wifiWasConnected) {
            Serial.print("[WIFI] Connecte. IP : ");
            Serial.println(WiFi.localIP());
            wifiWasConnected = true;
        }
        return true;
    }
    if (wifiWasConnected) {
        Serial.println("[WIFI] Connexion perdue.");
        wifiWasConnected = false;
    }
    unsigned long now = millis();
    if (now - lastWifiRetry < WIFI_RETRY_INTERVAL) return false;
    lastWifiRetry = now;
    static int retryCount = 0;
    retryCount++;
    Serial.print("[WIFI] Deconnecte. Tentative #");
    Serial.println(retryCount);
    /*
     * Cas legers uniquement : on alterne reconnexion simple et reset
     * logiciel du module. La recuperation lourde (NINA figee) n'est PLUS
     * geree ici : c'est le watchdog "10 min sans envoi" dans loop() qui
     * declenche un vrai reset materiel de la NINA. On evite ainsi de
     * rebooter / reset pour une simple coupure WiFi passagere.
     */
    if (retryCount % 4 == 0) {
        resetWiFiModule();
    } else {
        tryReconnectWiFi();
    }
    return false;
}

/*
 * Verifie que la connexion TCP au serveur est reellement possible.
 * Utilise pour detecter le cas ou WiFi.status() == WL_CONNECTED
 * mais la connexion reseau est en realite morte (bug NINA).
 *
 * Retourne true si le serveur repond, false sinon.
 */
bool checkServerReachable() {
    WiFiClient probe;
    bool ok = probe.connect(SERVER_HOST, SERVER_PORT);
    probe.stop();
    return ok;
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
        Watchdog.reset(); // on rafraichit pendant l'attente reseau legitime
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
    /*
     * Watchdog materiel active EN PREMIER : si quoi que ce soit se fige
     * ensuite, la carte reboote toute seule au bout de ~16s.
     */
    int wdt = Watchdog.enable(16000);
    Serial.begin(115200);
    delay(500);
    Serial.print("[WDT] Watchdog materiel actif : ");
    Serial.print(wdt);
    Serial.println(" ms.");

    // Restauration de la file conservee en RAM
    if (warmBootQueueValid()) {
        rtRebootCount++;
        Serial.print("[BOOT] Reboot a chaud #");
        Serial.print(rtRebootCount);
        Serial.print(" — file conservee : ");
        Serial.print(queueSize);
        Serial.println(" mesure(s).");
    } else {
        queueSize     = 0;
        rtRebootCount = 0;
        saveQueue();
        Serial.println("[BOOT] Demarrage a froid — file videe.");
    }
    Watchdog.reset();

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

    Watchdog.reset();

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Connexion lancee en arriere-plan...");

    // Acquisition immédiate au démarrage
    lastMeasure        = millis() - MEASURE_INTERVAL;
    lastSuccessfulSend = millis(); // initialise le watchdog logiciel
    Watchdog.reset();
}


// ============================================================
//  LOOP
// ============================================================

void loop() {
    Watchdog.reset(); // rafraichi a chaque tour — si la loop gele, reboot materiel
    unsigned long now = millis();


    // ── 1. WiFi en arrière-plan ──────────────────────────────
    bool wifiOk = manageWiFi();


    // ── 2. Watchdog 10 min sans envoi reussi : la NINA est probablement
    //       figee. On la reset MATERIELLEMENT (sans rebooter le SAMD21,
    //       donc la file est conservee). Si plusieurs resets NINA ne
    //       suffisent pas, on reboote la carte en tout dernier recours.
    if (now - lastSuccessfulSend > WATCHDOG_TIMEOUT) {
        ninaResetCount++;
        Serial.print("[WATCHDOG] 10 min sans envoi — reset NINA #");
        Serial.println(ninaResetCount);

        if (ninaResetCount >= MAX_NINA_RESETS) {
            rebootBoard("NINA toujours injoignable apres plusieurs resets.");
        }

        resetNINA();
        lastSuccessfulSend = millis(); // on redonne 10 min avant la prochaine action
    }


    // ── 3. Échantillonnage vibration (peak-to-peak, fenêtres 300 ms) ──
    readVibrationSample();


    // ── 4. Échantillonnage température toutes les 100 ms ─────
    if (now - t_temp >= TEMP_SAMPLE_INTERVAL_MS) {
        t_temp = now;
        readTemperatureSample();
    }


    // ── 5. Acquisition — t=0, t=60, t=120... ─────────────────
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

        updateDisplay(moyenneTemp, wifiOk);

        // Reset des accumulateurs
        sommeT         = 0.0;
        n              = 0;
        sommeCarresVib = 0.0;
        nVib           = 0;
    }


    // ── 6. Envoi — t=30, t=90, t=150... ──────────────────────
    // 30 s après chaque acquisition, on tente d'envoyer toute la queue.
    // Si succès → queue vidée.
    // Si échec → queue conservée, les offsets vieilliront au prochain cycle.
    if (pendingSend && now - lastSendAttempt >= SEND_DELAY) {
        pendingSend = false;

        if (wifiOk) {
            /*
             * Verification TCP reelle avant envoi (detecte le faux
             * WL_CONNECTED de la NINA). Si le serveur est injoignable,
             * on NE fait rien de brutal : on garde la file et on laisse
             * le watchdog 10 min decider s'il faut reset la NINA.
             */
            if (!checkServerReachable()) {
                Serial.println("[SEND] Serveur injoignable — file conservee, on attend.");
            } else {
                bool ok = sendQueue();
                serverEverTried = true;
                lastServerOk    = ok;

                if (ok) {
                    clearQueue();
                    lastSuccessfulSend = millis();
                    ninaResetCount = 0; // envoi OK → compteur de recuperation remis a zero
                } else {
                    Serial.println("[SEND] Echec — queue conservee.");
                }
            }
        } else {
            Serial.println("[SEND] WiFi indisponible — envoi reporte.");
        }
    }
}