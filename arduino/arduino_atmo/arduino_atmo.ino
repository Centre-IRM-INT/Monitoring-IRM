/*
 Monitocrio - Module de supervision environnementale (arduino_atmo)
 Version 3.0 - 11/08/2026
 Auteur : Romaiin

 Ce module Arduino MKR WiFi 1010 releve en continu la temperature,
 l'humidite et la pression atmospherique de la salle IRM via le
 shield MKR ENV, puis transmet ces mesures au serveur central
 (Raspberry Pi) par requete HTTP au format JSON.

 Le code integre plusieurs mecanismes de resilience developpes pour
 fiabiliser le fonctionnement en environnement contraint (salle IRM,
 reseau WiFi partage) :
 - une file d'attente persistante en RAM (.noinit) qui conserve les
   mesures non envoyees a travers les reboots du microcontroleur,
 - une detection active de la joignabilite du serveur (sonde TCP),
   car le statut WiFi renvoye par la puce NINA peut etre errone,
 - un reset materiel cible de la puce WiFi NINA-W102 en cas de blocage
   prolonge, sans redemarrer le reste de la carte,
 - un reboot complet du microcontroleur en tout dernier recours si les
   resets WiFi successifs echouent,
 - un watchdog materiel qui redemarre automatiquement la carte si le
   programme venait a se figer.

 L'objectif est d'assurer un fonctionnement autonome et fiable sur de
 longues periodes, sans intervention manuelle, meme en cas de coupure
 reseau ou de dysfonctionnement temporaire de la puce WiFi.
*/

#include <WiFiNINA.h>
#include <ArduinoJson.h>
#include <Arduino_MKRENV.h>
#include <Adafruit_SleepyDog.h>

/* Broche de reset materiel de la puce NINA-W102.
   Normalement definie par le core (pins_arduino.h du
 MKR WiFi 1010),
   valeur confirmee = 31, active a l'etat bas. Le define de secours
   ci-dessous evite toute surprise si jamais le core ne l'exposait pas. */
#ifndef NINA_RESETN
#define NINA_RESETN 31
#endif

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
const unsigned long MEASURE_INTERVAL    = 60000;  // acquisition toutes les 60s
const unsigned long SEND_DELAY          = 30000;  // envoi 30s apres acquisition
const unsigned long WIFI_RETRY_INTERVAL = 15000;  // tentative WiFi toutes les 15s
const unsigned long WATCHDOG_TIMEOUT    = 600000; // 10 min sans envoi reussi -> reset NINA
unsigned long lastMeasure     = 0;
unsigned long lastSendAttempt = 0;
unsigned long lastWifiRetry   = 0;
unsigned long lastSuccessfulSend = 0; // watchdog logiciel 10 min
bool pendingSend = false;
bool wifiWasConnected = false;

/* Nombre de resets NINA consecutifs sans envoi reussi. Au-dela de
   MAX_NINA_RESETS, on tente un reboot complet en tout dernier recours.
   Remis a zero a chaque envoi reussi (et au demarrage du programme). */
#define MAX_NINA_RESETS 3
int ninaResetCount = 0;

WiFiClient client;
// --------------------------
// FILE D'ATTENTE — CONSERVEE AU REBOOT
// --------------------------
/*
 * La file est placee en section .noinit : le code de demarrage du SAMD21
 * ne la remet PAS a zero au boot. Tant que l'alimentation tient (reboot
 * watchdog, NVIC_SystemReset, bouton reset), la RAM garde son contenu.
 * Seule une vraie coupure de courant l'efface.
 *
 * On distingue un reboot a chaud (file valide a conserver) d'un demarrage
 * a froid (RAM = poubelle) avec un nombre magique + un checksum FNV-1a.
 */
#define QUEUE_MAX   120
#define QUEUE_MAGIC 0xC0FFEE42UL
struct QueuedMeasure {
    int   offset;
    float temperature;
    float humidity;
    float pressure;
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
    size_t n = (size_t)queueSize * sizeof(QueuedMeasure);
    for (size_t i = 0; i < n; i++) {
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
    if (rtMagic != QUEUE_MAGIC)           return false;
    if (queueSize < 0 || queueSize > QUEUE_MAX) return false;
    if (rtChecksum != computeQueueChecksum())   return false;
    return true;
}
void addToQueue(float t, float h, float p) {
    for (int i = 0; i < queueSize; i++) {
        queue[i].offset--;
    }
    if (queueSize >= QUEUE_MAX) {
        for (int i = 0; i < QUEUE_MAX - 1; i++) {
            queue[i] = queue[i + 1];
        }
        queueSize = QUEUE_MAX - 1;
        Serial.println("[QUEUE] Pleine — ancienne mesure supprimee.");
    }
    queue[queueSize] = { 0, t, h, p };
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
// --------------------------
// WIFI — NON BLOQUANT
// --------------------------
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

    client.stop();                     /* on lache toute socket ouverte */

    pinMode(NINA_RESETN, OUTPUT);
    digitalWrite(NINA_RESETN, LOW);    /* NINA maintenue en reset */
    delay(100);
    digitalWrite(NINA_RESETN, HIGH);   /* relache : la NINA reboote */
    delay(750);                        /* laisse son firmware redemarrer */

    Watchdog.reset();

    wifiWasConnected = false;
    WiFi.begin(WIFI_SSID, WIFI_PASS);  /* re-init de la pile WiFi */
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
    // WiFi perdu
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
// --------------------------
// ENVOI HTTP
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
// --------------------------
// SETUP
// --------------------------
void setup() {
    /*
     * Watchdog materiel active EN PREMIER : si quoi que ce soit se fige
     * ensuite (y compris ENV.begin ou un appel SPI NINA bloquant), la
     * carte reboote toute seule au bout de ~16s. C'est ce qui sort la
     * carte de l'etat fige observe en salle IRM.
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
    if (!ENV.begin()) {
        Serial.println("[ENV] Erreur ENV Shield. Reboot dans ~16s.");
        while (true); // pas de Watchdog.reset() ici → reboot materiel et nouvelle tentative
    }
    Serial.println("[ENV] ENV Shield OK");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Connexion lancee en arriere-plan...");
    lastMeasure        = millis() - MEASURE_INTERVAL;
    lastSuccessfulSend = millis(); // initialise le watchdog logiciel
    Watchdog.reset();
}
// --------------------------
// LOOP
// --------------------------
void loop() {
    Watchdog.reset(); // rafraichi a chaque tour — si la loop gele, reboot materiel
    unsigned long now = millis();
    // ── 1. WiFi en arriere-plan ───────────────────────────
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
    // ── 3. Acquisition ────────────────────────────────────
    if (now - lastMeasure >= MEASURE_INTERVAL) {
        lastMeasure     = now;
        lastSendAttempt = now;
        pendingSend     = true;
        float temperature = ENV.readTemperature() - 5.1;
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
    // ── 4. Envoi ──────────────────────────────────────────
    if (pendingSend && now - lastSendAttempt >= SEND_DELAY) {
        pendingSend = false;
        if (wifiOk) {
            /*
             * Verification TCP reelle avant envoi (detecte le faux
             * WL_CONNECTED de la NINA). Si le serveur est injoignable,
             * on NE fait rien de brutal : on garde la file et on laisse
             * le watchdog 10 min decider s'il faut reset la NINA. Ca
             * donne la tolerance voulue — un trou passager ne declenche
             * aucun reset.
             */
            if (!checkServerReachable()) {
                Serial.println("[SEND] Serveur injoignable — file conservee, on attend.");
            } else {
                bool ok = sendQueue();
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