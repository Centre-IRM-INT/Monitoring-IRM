/**
 * ============================================================
 *  arduino_ntc_01_final.ino
 *  Station de mesure compresseur — Arduino MKR WiFi 1010
 * ============================================================
 *
 *  Description :
 *    Ce programme mesure toutes les 60 secondes :
 *      - La température via une sonde NTC (thermistance)
 *      - L'amplitude vibratoire RMS via un capteur piézoélectrique
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
 *    - Capteur piézoélectrique (branché sur A0) + résistance 10k
 *    - Signal état compresseur sur pin 2
 *    - Signal commande compresseur sur pin 4
 *
 *  Bibliothèques requises (Arduino Library Manager) :
 *    - WiFiNINA
 *    - ArduinoJson (>= v6)
 *
 *  Auteur  : [Romaiin / Centre IRM INT]
 *  Version : 1.0
 *  Date    : [22/05/2026]
 * ============================================================
 */

#include <WiFiNINA.h>
#include <ArduinoJson.h>


// ============================================================
//  CONFIGURATION 
// ============================================================

const char* WIFI_SSID   = "TP-Link_2D2A";       // Nom du réseau WiFi
const char* WIFI_PASS   = "35185260";            // Mot de passe WiFi
const char* DEVICE_ID   = "arduino_ntc_01";      // Identifiant de cet appareil
const char* SERVER_HOST = "192.168.0.101";       // Adresse IP du serveur
const int   SERVER_PORT = 8000;                  // Port du serveur
const char* SERVER_PATH = "/data";               // Route HTTP cible

// Client TCP pour les requêtes HTTP
WiFiClient client;


// ============================================================
//  SONDE NTC — Thermistance 10 kΩ sur pin A2
// ============================================================

const int   sensorPin = A2;     // Pin analogique de la sonde NTC

const float Rref = 10000.0;     // Résistance de référence du pont diviseur (Ω)
const float R0   = 10000.0;     // Résistance nominale de la NTC à T0 (Ω)
const float B    = 3380.0;      // Coefficient B de la thermistance (K)
const float T0   = 298.15;      // Température de référence (25°C en Kelvin)


// ============================================================
//  ENTRÉES DIGITALES — État et commande du compresseur
// ============================================================

const int pinSignal   = 2;  // Entrée : état du compresseur (marche/arrêt)
const int pinCommande = 4;  // Entrée : commande envoyée au compresseur


// ============================================================
//  CAPTEUR PIÉZO — Mesure vibratoire sur pin A0
// ============================================================

const int piezoPin = A0;

// Fréquence d'échantillonnage du signal vibratoire
const int           FS               = 1000;               // 1000 Hz
const unsigned long SAMPLE_PERIOD_US = 1000000UL / FS;     // Période en microsecondes (1000 µs)

// Variables du filtre passe-haut (supprime la composante continue du signal piézo)
float x_prev = 0.0;  // Échantillon brut précédent
float y_prev = 0.0;  // Échantillon filtré précédent
float alpha;         // Coefficient du filtre, calculé dans setup() selon la fréquence de coupure

// Lissage exponentiel appliqué après le filtre passe-haut
float y_smooth = 0.0;
const float SMOOTH_ALPHA = 0.08;  // Facteur de lissage 

// Accumulation RMS sur la fenêtre d'une minute
float         sommeCarresVib = 0.0;  // Somme des carrés des échantillons filtrés
unsigned long nVib           = 0;    // Nombre d'échantillons accumulés

// Horodatage du dernier échantillon vibratoire (en microsecondes)
unsigned long t_sample = 0;


// ============================================================
//  TIMERS — Déclenchement non bloquant des tâches périodiques
// ============================================================

unsigned long t_send = 0;   // Dernier envoi de données
unsigned long t_temp = 0;   // Dernier échantillon température

const unsigned long SEND_INTERVAL_MS        = 60000;  // Envoi toutes les 60 s
const unsigned long TEMP_SAMPLE_INTERVAL_MS = 100;    // Échantillon NTC toutes les 100 ms
const unsigned long WIFI_RETRY_INTERVAL     = 15000;  // Tentative WiFi toutes les 15 s


// ============================================================
//  MOYENNE TEMPÉRATURE — Accumulée sur 60 s
// ============================================================

float sommeT = 0.0;  // Somme des températures lues sur la fenêtre
int   n      = 0;    // Nombre de lectures valides


// ============================================================
//  FILE D'ATTENTE — Stockage des mesures hors-ligne
// ============================================================
// Quand le WiFi est absent, les mesures sont accumulées ici.
// Dès que la connexion revient, toutes les mesures en attente
// sont envoyées en un seul appel HTTP.

#define QUEUE_MAX 120  // ~2 h de stockage à 1 mesure/min

/**
 * Structure représentant une mesure complète en attente d'envoi.
 * L'offset indique le décalage temporel (en nombre de mesures)
 * par rapport au moment de l'envoi, pour que le serveur puisse
 * reconstruire le bon timestamp : timestamp_réel = now + offset * 60s
 */
struct QueuedMeasure {
    int   offset;      // Décalage temporel (0 = maintenant, -1 = il y a 60s, etc.)
    float temperature; // Température moyenne sur la fenêtre (°C)
    float amplitude;   // RMS vibratoire sur la fenêtre
    int   etat;        // État du compresseur (0 ou 1)
    int   commande;    // Commande compresseur (0 ou 1)
};

QueuedMeasure queue[QUEUE_MAX];  // Tableau des mesures en attente
int queueSize = 0;               // Nombre de mesures actuellement en file


/**
 * Ajoute une mesure à la file d'attente.
 *
 * Avant l'ajout, tous les offsets existants sont décrémentés
 * pour refléter l'avancement du temps.
 * Si la file est pleine, la mesure la plus ancienne est écrasée.
 */
void addToQueue(float temp, float amplitude, int etat, int commande) {
    // Décale tous les offsets d'un pas (le temps qui passe)
    for (int i = 0; i < queueSize; i++) queue[i].offset--;

    // Si la file est pleine, supprime la mesure la plus ancienne
    if (queueSize >= QUEUE_MAX) {
        for (int i = 0; i < QUEUE_MAX - 1; i++) queue[i] = queue[i + 1];
        queueSize = QUEUE_MAX - 1;
        Serial.println("[QUEUE] Pleine — ancienne mesure supprimee.");
    }

    // Ajoute la nouvelle mesure en fin de file
    queue[queueSize] = { -1, temp, amplitude, etat, commande };
    queueSize++;

    Serial.print("[QUEUE] ");
    Serial.print(queueSize);
    Serial.println(" mesure(s) en attente.");
}


/**
 * Vide complètement la file d'attente.
 * Appelé après un envoi HTTP réussi.
 */
void clearQueue() {
    queueSize = 0;
    Serial.println("[QUEUE] File videe.");
}


// ============================================================
//  GESTION WIFI — Reconnexion non bloquante
// ============================================================

unsigned long lastWifiRetry = 0;  // Horodatage de la dernière tentative


/**
 * Lance une tentative de reconnexion WiFi sans attendre le résultat.
 */
void tryReconnectWiFi() {
    Serial.println("[WIFI] Tentative de reconnexion (non bloquante)...");
    WiFi.disconnect();
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}


/**
 * Réinitialise complètement le module WiFi.
 * Utilisé après plusieurs échecs consécutifs.
 */
void resetWiFiModule() {
    Serial.println("[WIFI] Reset du module...");
    client.stop();
    WiFi.disconnect();
    WiFi.end();
    delay(3000);
    Serial.println("[WIFI] Module reset — reconnexion au prochain cycle.");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}


/**
 * Gère la connexion WiFi en arrière-plan.
 * À appeler à chaque tour de loop().
 *
 * - Si connecté : retourne true immédiatement.
 * - Si déconnecté : attend WIFI_RETRY_INTERVAL avant de réessayer.
 * - Après 4 échecs consécutifs : réinitialise le module WiFi.
 *
 * @return true si le WiFi est connecté, false sinon.
 */
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

/**
 * Lit un échantillon de température via la sonde NTC et l'accumule.
 *
 * Utilise la loi de Steinhart-Hart simplifiée (modèle B) :
 *   1/T = 1/T0 + (1/B) * ln(Rntc / R0)
 *
 * Les échantillons invalides (ADC saturé) sont ignorés.
 * La moyenne sera calculée à l'envoi.
 */
void readTemperatureSample() {
    int adc = analogRead(sensorPin);

    // Ignore les valeurs saturées (circuit ouvert ou court-circuit)
    if (adc <= 0 || adc >= 4095) return;

    // Calcul de la résistance NTC via le pont diviseur de tension
    float Rntc = Rref * adc / (4095.0 - adc);

    // Conversion résistance → température (Kelvin) via le modèle B
    float lnR = log(Rntc / R0);
    float T   = 1.0 / (1.0 / T0 + lnR / B);
    float Tc  = T - 273.15;  // Conversion Kelvin → Celsius

    sommeT += Tc;
    n++;
}


// ============================================================
//  LECTURE VIBRATION — Échantillonnage à 1 kHz
// ============================================================

/**
 * Échantillonne le signal piézo à 1 kHz et accumule le RMS.
 *
 * Traitement du signal :
 *   1. Filtre passe-haut (fc = 5 Hz) : supprime la dérive continue du piézo
 *   2. Lissage exponentiel (passe-bas) : réduit le bruit haute fréquence
 *   3. Accumulation des carrés pour le calcul RMS à l'envoi
 *
 * Doit être appelée à chaque tour de loop() pour respecter
 * la fréquence d'échantillonnage de 1 kHz.
 */
void readVibrationSample() {
    unsigned long now_us = micros();

    if (now_us - t_sample >= SAMPLE_PERIOD_US) {
        t_sample += SAMPLE_PERIOD_US;  // Avance le timer (évite la dérive d'horodatage)

        int   raw = analogRead(piezoPin);
        float x   = raw;

        // Filtre passe-haut : y[n] = alpha * (y[n-1] + x[n] - x[n-1])
        float y = alpha * (y_prev + x - x_prev);
        x_prev = x;
        y_prev = y;

        // Lissage exponentiel (passe-bas)
        y_smooth = y_smooth + SMOOTH_ALPHA * (y - y_smooth);

        // Accumulation pour le RMS
        sommeCarresVib += y_smooth * y_smooth;
        nVib++;
    }
}


// ============================================================
//  ENVOI HTTP — Transmission des mesures au serveur
// ============================================================

/**
 * Envoie toutes les mesures en file + la mesure actuelle au serveur.
 *
 * Construit un tableau JSON et envoie une requête POST HTTP/1.1.
 *
 * Format JSON :
 * [
 *   { "device_id": "arduino_ntc_01", "offset": -2, "temperature": 45.2,
 *     "amplitude": 12.3, "comptfetat": 1, "commande_compresseur": 1 },
 *   ...
 *   { "device_id": "arduino_ntc_01", "offset": 0, ... }  ← mesure actuelle
 * ]
 *
 * @param temp       Température moyenne (°C)
 * @param amplitude  RMS vibratoire
 * @param etat       État compresseur (0 ou 1)
 * @param commande   Commande compresseur (0 ou 1)
 * @return true si HTTP 200/201, false sinon.
 */
bool sendAll(float temp, float amplitude, int etat, int commande) {
    client.stop();
    delay(100);

    Serial.println("[HTTP] Connexion serveur...");
    if (!client.connect(SERVER_HOST, SERVER_PORT)) {
        Serial.println("[HTTP] Echec connexion serveur.");
        client.stop();
        return false;
    }

    // Taille du buffer : suffisante pour 120 mesures en file + 1 actuelle
    StaticJsonDocument<16384> doc;
    JsonArray arr = doc.to<JsonArray>();

    // Ajout des mesures en file d'attente
    for (int i = 0; i < queueSize; i++) {
        JsonObject obj = arr.createNestedObject();
        obj["device_id"]            = DEVICE_ID;
        obj["offset"]               = queue[i].offset;
        obj["temperature"]          = queue[i].temperature;
        obj["amplitude"]            = queue[i].amplitude;
        obj["comptfetat"]           = queue[i].etat;
        obj["commande_compresseur"] = queue[i].commande;
    }

    // Ajout de la mesure actuelle (offset = 0)
    JsonObject current = arr.createNestedObject();
    current["device_id"]            = DEVICE_ID;
    current["offset"]               = 0;
    current["temperature"]          = temp;
    current["amplitude"]            = amplitude;
    current["comptfetat"]           = etat;
    current["commande_compresseur"] = commande;

    char buffer[16384];
    size_t len = serializeJson(doc, buffer);

    Serial.print("[HTTP] Envoi de ");
    Serial.print(queueSize + 1);
    Serial.println(" mesure(s)...");

    // Requête HTTP POST
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

    // Lecture de la réponse HTTP (vérifie le code de statut)
    unsigned long timeout = millis();
    String responseLine = "";
    bool firstLine = true;
    bool success = false;

    while (millis() - timeout < 5000) {
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
//  SETUP — Initialisation au démarrage
// ============================================================

void setup() {
    Serial.begin(115200);

    analogReadResolution(12);  // Résolution ADC 12 bits (0–4095)

    pinMode(pinSignal,   INPUT);
    pinMode(pinCommande, INPUT);

    // Calcul du coefficient alpha du filtre passe-haut (fc = 5 Hz)
    // alpha = RC / (RC + T)  avec RC = 1/(2*pi*fc) et T = 1/FS
    float fc = 5.0;
    float T  = 1.0 / FS;
    float RC = 1.0 / (2.0 * 3.1416 * fc);
    alpha = RC / (RC + T);

    // Initialisation des timers
    t_sample = micros();
    t_send   = millis();
    t_temp   = millis();

    // Lance la connexion WiFi en arrière-plan
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[WIFI] Connexion lancee en arriere-plan...");

    // Force une mesure + envoi immédiat au premier tour de loop()
    t_send = millis() - SEND_INTERVAL_MS;
}


// ============================================================
//  LOOP — Boucle principale 
// ============================================================

void loop() {
    unsigned long now = millis();


    // ── Tâche 1 : Gestion WiFi en arrière-plan ──────────────
    bool wifiOk = manageWiFi();

    // Affiche l'IP une seule fois lors de la (re)connexion
    if (wifiOk && WiFi.status() == WL_CONNECTED) {
        static bool wifiJustConnected = false;
        if (!wifiJustConnected) {
            Serial.print("[WIFI] Connecte. IP : ");
            Serial.println(WiFi.localIP());
            wifiJustConnected = true;
        }
    } else {
        static bool wifiJustConnected = false;
        wifiJustConnected = false;
    }


    // ── Tâche 2 : Échantillonnage vibration à 1 kHz ─────────
    // Appelé à chaque tour de loop() — l'intervalle est géré
    // en interne via micros() dans readVibrationSample()
    readVibrationSample();


    // ── Tâche 3 : Échantillonnage température toutes les 100 ms
    if (now - t_temp >= TEMP_SAMPLE_INTERVAL_MS) {
        t_temp = now;
        readTemperatureSample();
    }


    // ── Tâche 4 : Calcul des moyennes + envoi toutes les 60 s
    if (now - t_send >= SEND_INTERVAL_MS) {
        t_send = now;

        // Calcul de la température moyenne sur la fenêtre
        float moyenneTemp = NAN;  // NAN = "pas de donnée valide"
        if (n > 0) moyenneTemp = sommeT / n;

        // Calcul du RMS vibratoire sur la fenêtre
        float rmsVibration = 0.0;
        if (nVib > 0) rmsVibration = sqrt(sommeCarresVib / nVib);

        // Lecture des entrées digitales
        int etat     = digitalRead(pinSignal);
        int commande = digitalRead(pinCommande);

        Serial.print("[MESURE] T=");
        Serial.print(moyenneTemp, 1);
        Serial.print("C  RMS=");
        Serial.print(rmsVibration, 2);
        Serial.print("  Etat=");
        Serial.print(etat);
        Serial.print("  Cmd=");
        Serial.println(commande);

        if (WiFi.status() == WL_CONNECTED) {
            // WiFi dispo : envoi de la file + mesure actuelle
            bool ok = sendAll(moyenneTemp, rmsVibration, etat, commande);
            if (ok) {
                clearQueue();  // Succès → vide la file
            } else {
                addToQueue(moyenneTemp, rmsVibration, etat, commande);
            }
        } else {
            // Pas de WiFi → stocke pour envoi ultérieur
            addToQueue(moyenneTemp, rmsVibration, etat, commande);
            Serial.println("[SEND] WiFi indisponible — mesure mise en queue.");
        }

        // Reset des accumulateurs pour la prochaine fenêtre
        sommeT         = 0.0;
        n              = 0;
        sommeCarresVib = 0.0;
        nVib           = 0;
    }


    // ── Tâche 5 : Rattrapage de la file entre deux mesures ──
    // Si des mesures sont en attente ET que le WiFi vient de
    // revenir, tente un envoi sans attendre la prochaine acquisition.
    static unsigned long lastSend = 0;
    if (now - lastSend >= SEND_INTERVAL_MS && queueSize > 0 && WiFi.status() == WL_CONNECTED) {
        lastSend = now;
        Serial.println("[SEND] Tentative de rattrapage de la queue...");

        QueuedMeasure& latest = queue[queueSize - 1];
        int savedOffset = latest.offset;
        latest.offset = 0;  // Marque temporairement comme mesure de référence

        bool ok = sendAll(latest.temperature, latest.amplitude, latest.etat, latest.commande);
        if (ok) {
            clearQueue();
        } else {
            latest.offset = savedOffset;  // Restaure l'offset si échec
        }
    }
}
