"""
main.py — Serveur de supervision IRM
=====================================
Serveur FastAPI tournant sur Raspberry Pi.
Rôle central du système : réception des mesures envoyées par les Arduinos,
datalogging CSV, transmission vers Supabase, diffusion WebSocket vers le
dashboard local, et déclenchement des alertes.

Auteur : Romaiin / Centre IRM INT
Version : 1.0
Date : 26/05/2026
"""

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel
from datetime import datetime, timezone, timedelta
from typing import List, Optional, Union
import threading
import time
import csv
from pathlib import Path
import requests
import os
from mail.alerte_manager import traiter_alertes, signaler_mesure_recue, demarrer_watchdog


# ==============================
# CONFIG GÉNÉRALE
# ==============================

# Liste des identifiants des appareils surveillés.
# Utilisée pour référence et pour les templates HTML.
DEVICES = [
    "arduino_atmo",        # Capteurs atmosphériques salle IRM (temp, humidité, pression)
    "arduino_mokescaner",  # Réservé — non encore déployé
    "arduino_ntc_01",      # Sonde NTC eau glacée + vibrations compresseur hélium
    "arduino_groupe_froid" # Réservé — groupe froid
]

# Dossier de stockage des fichiers CSV de datalogging.
# Créé automatiquement au démarrage s'il n'existe pas.
DATA_DIR = Path("data")
DATA_DIR.mkdir(exist_ok=True)

# Paramètres de logging (non utilisés activement, conservés pour référence).
LOG_INTERVAL_SEC = 60
LINES_PER_FILE   = 10

# Fuseau horaire France heure d'été (UTC+2).
# Hardcodé — à adapter en UTC+1 en hiver ou migrer vers zoneinfo.
FRANCE_TZ = timezone(timedelta(hours=2))


# ==============================
# SUPABASE — STOCKAGE CLOUD
# ==============================

# Les clés sont lues depuis les variables d'environnement système (définies dans ~/.bashrc).
# Ne jamais les écrire en dur dans le code source.
SUPABASE_URL = os.getenv("SUPABASE_URL")
SUPABASE_KEY = os.getenv("SUPABASE_KEY")

SUPABASE_ENDPOINT = f"{SUPABASE_URL}/rest/v1/measurements"

# En-têtes HTTP requis par l'API REST Supabase.
HEADERS = {
    "apikey": SUPABASE_KEY,
    "Authorization": f"Bearer {SUPABASE_KEY}",
    "Content-Type": "application/json"
}


def push_to_supabase(point):
    """
    Envoie un point de mesure vers la table 'measurements' de Supabase.

    Args:
        point (DataPoint): point de mesure à envoyer.

    Returns:
        bool: True si l'envoi a réussi (HTTP 201), False sinon.
    """
    payload = {
        "timestamp":            point.timestamp,
        "device_id":            point.device_id,
        "temperature":          point.temperature,
        "humidity":             point.humidity,
        "pressure":             point.pressure,
        "illuminance":          point.illuminance,
        "battery":              point.battery,
        "amplitude":            point.amplitude,
        "etat_gf":              point.etat_gf,
        "comptfetat":           point.comptfetat,
        "commande_compresseur": point.commande_compresseur
    }

    try:
        response = requests.post(
            SUPABASE_ENDPOINT,
            json=payload,
            headers=HEADERS,
            timeout=5
        )
        if response.status_code == 201:
            print("Valeur transmise avec succès à SUPABASE")
            return True
        else:
            print("Erreur SUPABASE :", response.status_code, response.text)
            return False

    except Exception as e:
        # Pas de connexion internet ou Supabase indisponible — on continue sans bloquer.
        print("Connexion SUPABASE impossible :", e)
        return False


# ==============================
# FASTAPI — APPLICATION
# ==============================

app = FastAPI()

# Moteur de templates Jinja2 pour servir le dashboard HTML local.
templates = Jinja2Templates(directory="app/templates")


# ==============================
# MODÈLES PYDANTIC
# ==============================

class DataIn(BaseModel):
    """
    Modèle de validation des données reçues des Arduinos via POST /data.
    Tous les champs de mesure sont optionnels car chaque Arduino
    n'envoie que les grandeurs qu'il mesure.
    L'offset permet de reconstruire le vrai timestamp des mesures
    mises en file d'attente pendant une coupure réseau.
    """
    device_id:              str
    offset:                 Optional[int]   = None   # Décalage en minutes (0 = mesure actuelle)
    temperature:            Optional[float] = None
    humidity:               Optional[float] = None
    pressure:               Optional[float] = None
    illuminance:            Optional[float] = None
    battery:                Optional[int]   = None
    amplitude:              Optional[float] = None   # Amplitude RMS vibrations compresseur
    etat_gf:                Optional[int]   = None   # État groupe froid (0/1)
    comptfetat:             Optional[int]   = None   # État compresseur TF (0/1)
    commande_compresseur:   Optional[int]   = None   # Commande compresseur (0/1)


class DataPoint(BaseModel):
    """
    Modèle interne d'un point de mesure horodaté.
    Construit à partir d'un DataIn après calcul du timestamp réel.
    Utilisé pour le buffer mémoire, le CSV et la diffusion WebSocket.
    """
    timestamp:              str
    device_id:              str
    temperature:            Optional[float] = None
    humidity:               Optional[float] = None
    pressure:               Optional[float] = None
    illuminance:            Optional[float] = None
    battery:                Optional[int]   = None
    amplitude:              Optional[float] = None
    etat_gf:                Optional[int]   = None
    comptfetat:             Optional[int]   = None
    commande_compresseur:   Optional[int]   = None
    offset:                 Optional[int]   = None


# ==============================
# BUFFER MÉMOIRE
# ==============================

# Stockage en mémoire des derniers points reçus (toutes sources confondues).
# Limite à MAX_POINTS pour éviter la saturation RAM du Pi.
data_buffer: List[DataPoint] = []
MAX_POINTS = 5000


# ==============================
# WEBSOCKET — DIFFUSION TEMPS RÉEL
# ==============================

class ConnectionManager:
    """
    Gestionnaire des connexions WebSocket actives.
    Permet de diffuser les nouvelles mesures à tous les dashboards
    connectés simultanément.
    """

    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        """Accepte et enregistre une nouvelle connexion WebSocket."""
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        """Supprime une connexion fermée ou en erreur."""
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)

    async def broadcast(self, message: dict):
        """
        Envoie un message JSON à toutes les connexions actives.
        Les connexions mortes sont détectées et supprimées automatiquement.
        """
        dead_connections = []
        for connection in self.active_connections:
            try:
                await connection.send_json(message)
            except Exception:
                dead_connections.append(connection)
        for connection in dead_connections:
            self.disconnect(connection)


manager = ConnectionManager()


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    """
    Endpoint WebSocket — le dashboard s'y connecte pour recevoir
    les mesures en temps réel sans avoir à rafraîchir la page.
    """
    await manager.connect(websocket)
    try:
        # Maintient la connexion ouverte en attendant des messages entrants
        # (le dashboard n'envoie rien, on attend juste une déconnexion).
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        manager.disconnect(websocket)


# ==============================
# DATALOGGING CSV
# ==============================

# Verrou pour éviter les écritures simultanées dans le CSV
# (le serveur peut recevoir plusieurs Arduinos en parallèle).
_csv_lock = threading.Lock()

# Colonnes du fichier CSV — ordre fixe pour cohérence entre les fichiers.
COLUMNS = [
    "timestamp", "device_id", "temperature", "humidity", "pressure",
    "illuminance", "battery", "amplitude", "etat_gf", "comptfetat",
    "commande_compresseur"
]


def log_csv(point):
    """
    Enregistre un point de mesure dans le fichier CSV du jour.
    Un fichier par jour au format YYYY-MM-DD.csv dans le dossier data/.
    L'en-tête est ajoutée automatiquement à la création du fichier.
    """
    today    = datetime.now(FRANCE_TZ).strftime("%Y-%m-%d")
    filepath = DATA_DIR / f"{today}.csv"
    row      = {col: getattr(point, col, None) for col in COLUMNS}

    with _csv_lock:
        file_exists = filepath.exists()
        with filepath.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=COLUMNS, delimiter=";")
            if not file_exists:
                writer.writeheader()
            writer.writerow(row)


def send_daily_summary():
    """
    Envoi quotidien vers Supabase d'un résumé du CSV de la veille.
    Déclenché automatiquement à minuit par summary_scheduler().
    Permet de vérifier côté cloud que le datalogging local fonctionne.
    """
    yesterday = (datetime.now(FRANCE_TZ) - timedelta(days=1)).strftime("%Y-%m-%d")
    filepath  = DATA_DIR / f"{yesterday}.csv"

    if not filepath.exists():
        print("[SUMMARY] Pas de CSV trouvé pour hier")
        return

    with filepath.open("r", encoding="utf-8") as f:
        line_count = sum(1 for _ in csv.DictReader(f, delimiter=";"))

    payload = {
        "timestamp":  datetime.now(FRANCE_TZ).isoformat(),
        "device_id":  f"{yesterday}.csv",
        "line_count": line_count
    }

    try:
        response = requests.post(SUPABASE_ENDPOINT, json=payload, headers=HEADERS, timeout=5)
        if response.status_code == 201:
            print(f"[SUMMARY] {yesterday}.csv → {line_count} lignes envoyées")
        else:
            print(f"[SUMMARY] Erreur :", response.status_code, response.text)
    except Exception as e:
        print(f"[SUMMARY] Connexion impossible :", e)


def summary_scheduler():
    """
    Thread en arrière-plan qui attend minuit chaque jour
    puis déclenche l'envoi du résumé CSV.
    Tourne en daemon — s'arrête automatiquement avec le serveur.
    """
    while True:
        now           = datetime.now(FRANCE_TZ)
        next_midnight = (now + timedelta(days=1)).replace(
            hour=0, minute=0, second=0, microsecond=0
        )
        wait_seconds = (next_midnight - now).total_seconds()
        print(f"[SUMMARY] Prochain envoi dans {wait_seconds/3600:.1f}h")
        time.sleep(wait_seconds)
        send_daily_summary()


@app.on_event("startup")
def start_summary_scheduler():
    """
    Lancé automatiquement au démarrage de FastAPI.
    Démarre le scheduler de résumé quotidien et le watchdog d'alertes.
    """
    threading.Thread(target=summary_scheduler, daemon=True).start()
    print("[SUMMARY] résumé démarré")
    demarrer_watchdog()


# ==============================
# HELPERS
# ==============================

def compute_timestamp(offset: Optional[int]) -> str:
    """
    Calcule le timestamp réel d'une mesure à partir de l'offset Arduino.

    Les Arduinos stockent les mesures en file d'attente pendant une coupure
    réseau avec un offset négatif en minutes (ex: -3 = mesure prise il y a 3 min).
    Cette fonction reconstruit le timestamp réel pour maintenir la cohérence
    temporelle dans Supabase et les CSV.

    Args:
        offset (int | None): décalage en minutes. 0 ou None = mesure actuelle.

    Returns:
        str: timestamp ISO 8601 avec fuseau horaire France.
    """
    now = datetime.now(FRANCE_TZ)
    if offset is not None and offset != 0:
        return (now + timedelta(minutes=offset)).isoformat()
    return now.isoformat()


def build_point(item: DataIn) -> DataPoint:
    """
    Convertit un DataIn (données brutes Arduino) en DataPoint horodaté.

    Args:
        item (DataIn): données reçues via POST /data.

    Returns:
        DataPoint: point de mesure prêt pour le buffer, le CSV et Supabase.
    """
    return DataPoint(
        timestamp=              compute_timestamp(item.offset),
        device_id=              item.device_id,
        temperature=            item.temperature,
        humidity=               item.humidity,
        pressure=               item.pressure,
        illuminance=            item.illuminance,
        battery=                item.battery,
        amplitude=              item.amplitude,
        etat_gf=                item.etat_gf,
        comptfetat=             item.comptfetat,
        commande_compresseur=   item.commande_compresseur
    )


# ==============================
# ENDPOINT PRINCIPAL — RÉCEPTION DES DONNÉES
# ==============================

@app.post("/data")
async def receive_data(payload: Union[List[DataIn], DataIn]):
    """
    Endpoint POST /data — reçoit les mesures envoyées par les Arduinos.

    Accepte indifféremment :
    - Un objet JSON unique (ancien format sans file d'attente)
    - Un tableau JSON (nouveau format avec file d'attente incluse)

    Pour chaque mesure reçue :
    1. Construit un DataPoint horodaté
    2. Signale la réception au watchdog d'alertes
    3. Stocke dans le buffer mémoire
    4. Enregistre dans le CSV du jour
    5. Déclenche les alertes si c'est la mesure actuelle (offset 0)
    6. Envoie vers Supabase

    Seule la mesure la plus récente (dernière de la liste) est diffusée
    via WebSocket au dashboard — pour ne pas surcharger l'interface.

    Returns:
        dict: statut et nombre de mesures traitées.
    """
    # Normalise toujours en liste pour un traitement uniforme.
    items: List[DataIn] = payload if isinstance(payload, list) else [payload]

    if len(items) > 1:
        print(f"[DATA] Réception de {len(items)} mesure(s) (file d'attente incluse)")

    last_point = None

    for item in items:
        point = build_point(item)
        signaler_mesure_recue(item.device_id)

        # Buffer mémoire — supprime le plus ancien si plein.
        data_buffer.append(point)
        if len(data_buffer) > MAX_POINTS:
            data_buffer.pop(0)

        log_csv(point)

        # Affichage console des champs non-nuls pour le monitoring terminal.
        fields = []
        if point.temperature          is not None: fields.append(f"T={point.temperature}°C")
        if point.humidity             is not None: fields.append(f"H={point.humidity}%")
        if point.pressure             is not None: fields.append(f"P={point.pressure} hPa")
        if point.illuminance          is not None: fields.append(f"Lux={point.illuminance}")
        if point.battery              is not None: fields.append(f"Bat={point.battery}%")
        if point.amplitude            is not None: fields.append(f"Amp={point.amplitude}")
        if point.etat_gf              is not None: fields.append(f"GF={'ON' if point.etat_gf else 'OFF'}")
        if point.comptfetat           is not None: fields.append(f"CompTF={'ON' if point.comptfetat else 'OFF'}")
        if point.commande_compresseur is not None: fields.append(f"CmdComp={'ON' if point.commande_compresseur else 'OFF'}")
        print(f"{point.device_id} @ {point.timestamp} | " + " | ".join(fields))

        # Les alertes ne se déclenchent que sur la mesure actuelle (offset 0 ou None).
        # Les mesures de rattrapage (offset négatif) ne génèrent pas d'alertes
        # pour éviter des faux positifs sur des événements déjà passés.
        if item.offset is None or item.offset == 0:
            traiter_alertes(point)

        push_to_supabase(point)

        last_point = point

    # Diffusion WebSocket — uniquement la mesure la plus récente.
    # Le dashboard reçoit ainsi toujours l'état courant du système.
    if last_point:
        await manager.broadcast(last_point.dict())

    return {"status": "ok", "received": len(items)}


# ==============================
# DASHBOARD LOCAL
# ==============================

@app.get("/", response_class=HTMLResponse)
async def dashboard(request: Request):
    """
    Sert le dashboard de supervision locale (dashboard.html).
    Accessible depuis tout navigateur du réseau local sur http://<ip_pi>:8000/
    """
    return templates.TemplateResponse(
        "dashboard.html",
        {
            "request": request,
            "devices": DEVICES
        }
    )
