"""
=============================================================================
alerte_manager.py  –  Gestionnaire d'alertes de la plateforme de monitoring IRM
=============================================================================

Ce module gère deux systèmes d'alertes indépendants, tous deux basés sur
l'envoi d'e-mails via SMTP (Gmail) :

  1. WATCHDOG (surveillance de silence)
     ─────────────────────────────────
     Un thread tourne en permanence en arrière-plan et vérifie, toutes les
     60 secondes, si chaque Arduino a bien envoyé une mesure récemment.
     Si un module ne répond plus, des mails d'escalade sont envoyés selon
     le calendrier défini dans config_seuils.json :
       - +15 min  → 1er mail d'alerte
       - +30 min  → rappel intermédiaire
       - +60 min  → rappel horaire, puis toutes les heures
     Dès que le module reprend, un mail de confirmation est envoyé.

  2. ALERTES SUR SEUILS (surveillance des valeurs mesurées)
     ────────────────────────────────────────────────────────
     À chaque mesure reçue, les valeurs sont comparées aux seuils définis
     dans config_seuils.json (min, max, expected).
     - Dépassement → mail d'alerte immédiat, puis rappel toutes les heures
     - Retour à la normale → mail de confirmation automatique

Intégration dans le projet :
  - main.py appelle signaler_mesure_recue(device_id) à chaque réception HTTP
  - main.py appelle traiter_alertes(point) pour vérifier les seuils
  - main.py appelle demarrer_watchdog() au démarrage de FastAPI (@app.on_event)

Fichier de configuration associé : config_seuils.json
  Format attendu :
  {
    "<device_id>": {
      "<champ>": {
        "min": float,         # seuil bas (optionnel)
        "max": float,         # seuil haut (optionnel)
        "expected": [val],    # valeurs autorisées (optionnel, pour les états 0/1)
        "enabled": bool,      # active ou désactive cette règle
        "label": str,         # nom lisible affiché dans les mails
        "unit": str,          # unité (°C, %, hPa…)
        "message_low": str,   # texte du mail si valeur < min
        "message_high": str,  # texte du mail si valeur > max
        "message_expected": str  # texte du mail si valeur hors expected
      }
    },
    "watchdog": {
      "delai_alerte_minutes": int,        # délai avant le 1er mail de silence
      "rappels_minutes": [int, int],      # paliers intermédiaires de rappel
      "rappel_horaire_apres_minutes": int # seuil à partir duquel on rappelle toutes les heures
    }
  }
=============================================================================
"""

import json
import smtplib
import threading
import time
from pathlib import Path
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
from email.mime.text import MIMEText


# =============================================================================
# SECTION 1 — CONFIGURATION GÉNÉRALE
# =============================================================================

# Chemin absolu vers le fichier JSON de configuration des seuils.
# Ce fichier est lu dynamiquement à chaque vérification, ce qui permet de
# modifier les seuils sans redémarrer le serveur FastAPI.
CONFIG_SEUILS_FILE = Path("/home/monitocrio/Documents/Test-com/seuils/config_seuils.json")

# Paramètres du serveur SMTP Gmail.
# STARTTLS est utilisé sur le port 587 (chiffrement explicite après connexion).
SMTP_SERVER = "smtp.gmail.com"
SMTP_PORT   = 587

# Compte Gmail expéditeur (mot de passe d'application Google, pas le mot de passe du compte).
# Pour générer un mot de passe d'application : Compte Google > Sécurité > Mots de passe des applications.
EMAIL    = "automate.irm.2026@gmail.com"
PASSWORD = "nvkv jhfz fqco ogdu"   # Mot de passe d'application (16 caractères, sans espaces réels)

# Liste des adresses e-mail destinataires des alertes.
# Tous reçoivent le même mail simultanément.
DESTINATAIRES = [
    "romain.poos@univ-amu.fr",
    "bruno.nazarian@univ-amu.fr"
]

# Délai minimal entre deux mails pour une même alerte de seuil.
# Évite le spam en cas de valeur oscillant autour du seuil.
# Configurable ici uniquement (pas dans le JSON pour les alertes seuils).
DELAI_RAPPEL_ALERTE = timedelta(hours=1)

# Liste des identifiants des modules Arduino surveillés par le watchdog.
# Doit être cohérente avec les device_id envoyés dans les payloads HTTP.
DEVICES = [
    "arduino_atmo",
    "arduino_mokescaner",
    "arduino_ntc_01",
    "arduino_groupe_froid"
]

# Dictionnaire global des alertes seuils actives.
# Clé   : identifiant unique de l'alerte (format : "<device_id>.<champ>.<low|high|expected>")
# Valeur : dictionnaire avec les métadonnées de l'alerte (voir traiter_alertes)
alertes_actives = {}


# =============================================================================
# SECTION 2 — UTILITAIRES DATE / HEURE
# =============================================================================

def heure_locale() -> datetime:
    """
    Retourne l'heure courante en fuseau horaire Europe/Paris (UTC+1 ou UTC+2 selon DST).
    Utilisé systématiquement à la place de datetime.now() pour garantir la cohérence
    des horodatages dans les mails, indépendamment du fuseau du système hôte.

    Returns:
        datetime: heure locale avec timezone (aware datetime)
    """
    return datetime.now(ZoneInfo("Europe/Paris"))


def formater_heure(dt: datetime) -> str:
    """
    Formate un datetime en chaîne lisible pour les corps de mails.

    Args:
        dt (datetime): objet datetime (aware ou naive)

    Returns:
        str: chaîne au format "JJ/MM/AAAA à HH:MM" (ex: "23/05/2025 à 14:37")
    """
    return dt.strftime("%d/%m/%Y à %H:%M")


# =============================================================================
# SECTION 3 — ENVOI D'E-MAIL
# =============================================================================

def envoyer_mail(sujet: str, message: str) -> bool:
    """
    Envoie un e-mail en texte brut à tous les DESTINATAIRES configurés.

    Protocole utilisé : SMTP avec STARTTLS (port 587).
    La connexion est ouverte et fermée à chaque appel (pas de connexion persistante)
    pour éviter les déconnexions sur les serveurs avec timeout court.

    Args:
        sujet   (str): Objet de l'e-mail (apparaît dans la boîte de réception)
        message (str): Corps du mail en texte brut (UTF-8)

    Returns:
        bool: True si l'envoi a réussi, False en cas d'erreur réseau ou d'auth
    """
    # Construction du message MIME
    msg = MIMEText(message, "plain", "utf-8")
    msg["Subject"] = sujet
    msg["From"]    = EMAIL
    msg["To"]      = ", ".join(DESTINATAIRES)  # Tous visibles dans le champ To

    try:
        # Connexion au serveur SMTP avec timeout de 10 s (évite les blocages infinis)
        with smtplib.SMTP(SMTP_SERVER, SMTP_PORT, timeout=10) as server:
            server.starttls()                             # Activation du chiffrement TLS
            server.login(EMAIL, PASSWORD)                 # Authentification Gmail
            server.send_message(msg, from_addr=EMAIL, to_addrs=DESTINATAIRES)

        print("[MAIL] Mail envoyé :", sujet)
        return True

    except Exception as e:
        # L'erreur est loguée mais ne lève pas d'exception pour ne pas bloquer main.py
        print("[MAIL] Erreur envoi mail :", e)
        return False


# =============================================================================
# SECTION 4 — LECTURE DU FICHIER DE CONFIGURATION
# =============================================================================

def charger_seuils() -> dict:
    """
    Charge et retourne le contenu de config_seuils.json sous forme de dictionnaire.

    Le fichier est relu à chaque appel (pas de cache), ce qui permet de modifier
    les seuils à chaud sans redémarrer le serveur.

    Returns:
        dict: configuration complète des seuils, ou {} en cas d'erreur
    """
    try:
        with CONFIG_SEUILS_FILE.open("r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        print("[SEUILS] Fichier config_seuils.json introuvable")
        return {}
    except Exception as e:
        print("[SEUILS] Erreur lecture config_seuils.json :", e)
        return {}


def charger_config_watchdog() -> dict:
    """
    Extrait et retourne la section "watchdog" du fichier de configuration.
    Des valeurs par défaut sont utilisées si la clé est absente ou mal formée.

    Valeurs par défaut :
      - delai_alerte_minutes         : 15  → premier mail après 15 min de silence
      - rappels_minutes              : [15, 30] → paliers intermédiaires
      - rappel_horaire_apres_minutes : 60  → rappels horaires dès 60 min de silence

    Returns:
        dict: configuration watchdog avec garantie de toutes les clés nécessaires
    """
    seuils = charger_seuils()
    cfg    = seuils.get("watchdog", {})
    return {
        "delai_alerte_minutes"        : cfg.get("delai_alerte_minutes", 15),
        "rappels_minutes"             : cfg.get("rappels_minutes", [15, 30]),
        "rappel_horaire_apres_minutes": cfg.get("rappel_horaire_apres_minutes", 60)
    }


# =============================================================================
# SECTION 5 — WATCHDOG : DÉTECTION DES MODULES SILENCIEUX
# =============================================================================
#
# Architecture du watchdog :
#   - Un dictionnaire _watchdog_state stocke l'état de chaque device
#   - Un thread daemon tourne en permanence et vérifie l'état toutes les 60 s
#   - Un verrou threading.Lock() protège les accès concurrents au dictionnaire
#     (le thread watchdog lit/écrit, main.py écrit via signaler_mesure_recue)
#
# Structure de _watchdog_state[device_id] :
# {
#   "derniere_mesure"                : datetime | None,  # timestamp de la dernière réception
#   "alerte_active"                  : bool,             # True si un mail de silence a été envoyé
#   "dernier_mail"                   : datetime | None,  # timestamp du dernier mail envoyé
#   "minutes_ecoules_au_dernier_mail": int               # durée de silence au moment du dernier mail
# }
#
# Le champ "minutes_ecoules_au_dernier_mail" est nécessaire pour recalculer
# la durée totale du silence dans le mail de reprise, car _watchdog_state est
# réinitialisé dès la réception d'une mesure (on perd l'historique).
# =============================================================================

_watchdog_state: dict = {}
_watchdog_lock = threading.Lock()


def _init_device(device_id: str):
    """
    Initialise l'entrée watchdog d'un device s'il n'est pas encore enregistré.
    Appelé de manière défensive avant tout accès à _watchdog_state[device_id].

    Args:
        device_id (str): identifiant du module Arduino (ex: "arduino_atmo")
    """
    if device_id not in _watchdog_state:
        _watchdog_state[device_id] = {
            "derniere_mesure"                : None,
            "alerte_active"                  : False,
            "dernier_mail"                   : None,
            "minutes_ecoules_au_dernier_mail": 0
        }


def signaler_mesure_recue(device_id: str):
    """
    À appeler dans main.py à chaque réception d'une mesure valide d'un device.

    Rôle :
      - Met à jour le timestamp de dernière mesure (réinitialise le watchdog)
      - Si une alerte de silence était active, envoie un mail de reprise
        indiquant la durée totale du silence, puis remet l'état à zéro

    Thread-safety : utilise _watchdog_lock pour protéger _watchdog_state.

    Args:
        device_id (str): identifiant du module qui vient d'envoyer une mesure
    """
    maintenant = heure_locale()

    with _watchdog_lock:
        _init_device(device_id)
        etat = _watchdog_state[device_id]

        if etat["alerte_active"]:
            # Calcul de la durée totale du silence pour le mail de reprise.
            # On additionne :
            #   - le temps depuis le dernier mail (non encore compté)
            #   - les minutes déjà comptées au moment du dernier mail
            dernier_mail_il_y_a = maintenant - etat["dernier_mail"]
            minutes_silence     = (
                int(dernier_mail_il_y_a.total_seconds() / 60)
                + etat["minutes_ecoules_au_dernier_mail"]
            )

            sujet   = f"REPRISE IRM - {device_id}"
            message = (
                f"Le module {device_id} a repris l'envoi de mesures.\n\n"
                f"Durée totale du silence : ~{minutes_silence} minutes\n"
                f"Heure de reprise : {formater_heure(maintenant)}\n\n"
                f"Aucune action supplémentaire requise."
            )
            envoyer_mail(sujet, message)
            print(f"[WATCHDOG] {device_id} a repris - alerte levée")

        # Réinitialisation complète de l'état du device
        etat["derniere_mesure"]                 = maintenant
        etat["alerte_active"]                   = False
        etat["dernier_mail"]                    = None
        etat["minutes_ecoules_au_dernier_mail"] = 0


def _verifier_silences():
    """
    Boucle infinie du thread watchdog. Vérifie l'état de chaque device toutes
    les 60 secondes et envoie des mails d'alerte selon la logique d'escalade.

    Logique d'escalade (valeurs par défaut de config_seuils.json) :
      - silence ≥ 15 min → 1er mail ("SILENCE IRM")
      - silence ≥ 30 min → 2e mail (rappel intermédiaire)
      - silence ≥ 60 min → 3e mail, puis rappel toutes les heures

    Les seuils sont relus depuis le JSON à chaque itération pour permettre
    une reconfiguration à chaud sans redémarrage.

    Note : cette fonction ne retourne jamais (thread daemon, tué à la fin du process).
    """
    while True:
        time.sleep(60)  # Pause d'une minute entre chaque cycle de vérification
        maintenant = heure_locale()
        cfg        = charger_config_watchdog()

        delai_alerte  = cfg["delai_alerte_minutes"]
        rappels       = sorted(cfg["rappels_minutes"])   # Tri croissant pour parcourir les paliers
        seuil_horaire = cfg["rappel_horaire_apres_minutes"]

        with _watchdog_lock:
            for device_id in DEVICES:
                _init_device(device_id)
                etat = _watchdog_state[device_id]

                # Device jamais vu depuis le démarrage : on ne génère pas de faux positif
                if etat["derniere_mesure"] is None:
                    continue

                # Durée du silence en minutes depuis la dernière mesure reçue
                silence_minutes = (maintenant - etat["derniere_mesure"]).total_seconds() / 60

                # Silence inférieur au seuil de déclenchement → rien à faire
                if silence_minutes < delai_alerte:
                    continue

                envoyer = False

                if not etat["alerte_active"]:
                    # Premier franchissement du seuil → déclenchement de l'alerte
                    envoyer               = True
                    etat["alerte_active"] = True

                else:
                    # Alerte déjà active : on vérifie si on doit envoyer un rappel
                    dernier_mail     = etat["dernier_mail"]
                    mins_depuis_mail = (maintenant - dernier_mail).total_seconds() / 60
                    mins_au_dernier  = etat["minutes_ecoules_au_dernier_mail"]

                    # Vérification des paliers de rappel intermédiaires (ex: 15, 30 min)
                    # On ne déclenche un palier que s'il est entre le précédent mail et maintenant
                    for palier in rappels:
                        if palier > delai_alerte:  # Ignore les paliers ≤ au premier déclenchement
                            if mins_au_dernier < palier <= silence_minutes:
                                envoyer = True
                                break

                    # Rappel horaire : une fois le seuil horaire dépassé, rappel toutes les 60 min
                    if not envoyer and silence_minutes >= seuil_horaire:
                        if mins_depuis_mail >= 60:
                            envoyer = True

                if envoyer:
                    mins_int = int(silence_minutes)
                    sujet    = f"SILENCE IRM - {device_id} ({mins_int} min)"
                    message  = (
                        f"Le module {device_id} n'envoie plus de mesures.\n\n"
                        f"Dernière mesure reçue : {formater_heure(etat['derniere_mesure'])}\n"
                        f"Durée du silence : {mins_int} minute(s)\n"
                        f"Heure de détection : {formater_heure(maintenant)}\n\n"
                        f"Veuillez vérifier le module et la connexion réseau."
                    )
                    envoyer_mail(sujet, message)

                    # Sauvegarde du contexte du mail pour calculer l'escalade suivante
                    etat["dernier_mail"]                    = maintenant
                    etat["minutes_ecoules_au_dernier_mail"] = mins_int
                    print(f"[WATCHDOG] Mail silence envoyé - {device_id} ({mins_int} min)")


def demarrer_watchdog():
    """
    Lance le thread watchdog en arrière-plan.

    Le thread est créé en mode daemon=True : il sera automatiquement tué
    lorsque le processus principal (FastAPI/Uvicorn) se termine, sans qu'il
    soit nécessaire de le gérer explicitement.

    À appeler une seule fois au démarrage de FastAPI, typiquement dans un
    handler @app.on_event("startup") dans main.py.
    """
    t = threading.Thread(target=_verifier_silences, daemon=True)
    t.start()
    print("[WATCHDOG] Démarré - surveillance de", DEVICES)


# =============================================================================
# SECTION 6 — VÉRIFICATION DES SEUILS DE MESURE
# =============================================================================

def verifier_seuils(point) -> list:
    """
    Compare les valeurs d'un DataPoint aux seuils définis dans config_seuils.json
    et retourne la liste des alertes déclenchées.

    Trois types de règles sont supportés (non exclusifs pour un même champ) :
      - "min" / "max"   : plage de valeurs autorisée (alertes "low" et "high")
      - "expected"      : liste de valeurs autorisées, typiquement [0] ou [1]
                          pour les signaux booléens (état compresseur, groupe froid)

    Chaque alerte retournée est un dictionnaire avec les champs :
      {
        "id"       : str,   # identifiant unique "<device_id>.<champ>.<low|high|expected>"
        "device_id": str,
        "champ"    : str,   # nom du champ dans le payload (ex: "temperature")
        "label"    : str,   # nom lisible pour les mails
        "valeur"   : float, # valeur mesurée ayant déclenché l'alerte
        "unite"    : str,   # unité de la mesure
        "message"  : str    # texte descriptif de l'alerte (issu du JSON)
      }

    Args:
        point: objet DataPoint (Pydantic) issu de main.py, avec attributs
               device_id, temperature, humidity, pressure, amplitude, etc.

    Returns:
        list[dict]: liste des alertes déclenchées (vide si tout est normal)
    """
    seuils  = charger_seuils()
    alertes = []

    # Récupère la configuration du device concerné
    config_device = seuils.get(point.device_id)
    if not config_device:
        return alertes  # Pas de seuils configurés pour ce device → rien à vérifier

    # Conversion du DataPoint Pydantic en dictionnaire pour accès générique par nom de champ
    data = point.dict()

    for champ, config in config_device.items():
        # Ignore les règles désactivées dans le JSON ("enabled": false)
        if not config.get("enabled", False):
            continue

        valeur = data.get(champ)
        if valeur is None:
            continue  # Champ absent dans ce payload (normal si optionnel)

        label = config.get("label", champ)
        unite = config.get("unit", "")

        # --- Règle MIN : valeur trop basse ---
        if "min" in config and valeur < config["min"]:
            alertes.append({
                "id"       : f"{point.device_id}.{champ}.low",
                "device_id": point.device_id,
                "champ"    : champ,
                "label"    : label,
                "valeur"   : valeur,
                "unite"    : unite,
                "message"  : config.get("message_low", f"{label} trop basse.")
            })

        # --- Règle MAX : valeur trop haute ---
        if "max" in config and valeur > config["max"]:
            alertes.append({
                "id"       : f"{point.device_id}.{champ}.high",
                "device_id": point.device_id,
                "champ"    : champ,
                "label"    : label,
                "valeur"   : valeur,
                "unite"    : unite,
                "message"  : config.get("message_high", f"{label} trop élevée.")
            })

        # --- Règle EXPECTED : valeur hors des états autorisés ---
        # Utilisé pour les signaux binaires : expected=[1] signifie "doit toujours être ON"
        if "expected" in config and valeur not in config["expected"]:
            alertes.append({
                "id"       : f"{point.device_id}.{champ}.expected",
                "device_id": point.device_id,
                "champ"    : champ,
                "label"    : label,
                "valeur"   : valeur,
                "unite"    : unite,
                "message"  : config.get("message_expected", f"{label} valeur incorrecte.")
            })

    return alertes


# =============================================================================
# SECTION 7 — GESTION DU CYCLE DE VIE DES ALERTES SEUILS
# =============================================================================

def traiter_alertes(point):
    """
    Orchestre le cycle de vie complet des alertes seuils pour un DataPoint reçu.

    Appelé dans main.py après chaque mesure valide (offset == 0 uniquement,
    pour ignorer les mesures en retard de la file d'attente Arduino).

    Logique en deux phases :

    Phase 1 — Alertes nouvelles ou persistantes :
      Pour chaque alerte détectée par verifier_seuils() :
        - Si l'alerte est nouvelle → mail immédiat + enregistrement dans alertes_actives
        - Si l'alerte était déjà active → rappel si DELAI_RAPPEL_ALERTE écoulé (1 h)
      La valeur courante est toujours mise à jour dans alertes_actives.

    Phase 2 — Retour à la normale :
      Pour chaque alerte précédemment active de ce device qui n'apparaît plus
      dans les alertes détectées → mail de "RETOUR NORMAL" + suppression de alertes_actives.

    Note sur le filtrage par device_id :
      On ne vérifie le "retour normal" que pour les alertes du device courant,
      car les autres devices n'ont pas de nouvelles mesures à ce moment.

    Args:
        point: objet DataPoint (Pydantic) issu de main.py
    """
    global alertes_actives

    maintenant            = heure_locale()
    alertes_detectees     = verifier_seuils(point)
    # Ensemble des IDs d'alertes encore actives après la mesure courante
    ids_alertes_detectees = {alerte["id"] for alerte in alertes_detectees}

    # ─── Phase 1 : Traitement des alertes nouvelles ou persistantes ───────────
    for alerte in alertes_detectees:
        alerte_id = alerte["id"]
        envoyer   = False

        if alerte_id not in alertes_actives:
            # Nouvelle alerte : enregistrement et envoi immédiat
            envoyer = True
            alertes_actives[alerte_id] = {
                "premiere_detection": maintenant,
                "dernier_mail"      : None,          # sera renseigné après l'envoi
                "derniere_valeur"   : alerte["valeur"],
                "label"             : alerte["label"],
                "device_id"         : alerte["device_id"],
                "champ"             : alerte["champ"],
                "unite"             : alerte["unite"]
            }
        else:
            # Alerte déjà connue : rappel si le délai est écoulé
            dernier_mail = alertes_actives[alerte_id]["dernier_mail"]
            if dernier_mail is None:
                envoyer = True  # Mail initial pas encore envoyé (ne devrait pas arriver)
            elif maintenant - dernier_mail >= DELAI_RAPPEL_ALERTE:
                envoyer = True  # Rappel après 1 heure
            # Mise à jour de la valeur même si on n'envoie pas de rappel
            alertes_actives[alerte_id]["derniere_valeur"] = alerte["valeur"]

        if envoyer:
            sujet   = f"ALERTE IRM - {alerte['label']}"
            message = (
                f"{alerte['message']}\n\n"
                f"Équipement : {alerte['device_id']}\n"
                f"Mesure : {alerte['label']}\n"
                f"Valeur reçue : {alerte['valeur']} {alerte['unite']}\n"
                f"Heure locale : {formater_heure(maintenant)}\n\n"
                f"Veuillez vérifier le système."
            )
            envoyer_mail(sujet, message)
            alertes_actives[alerte_id]["dernier_mail"] = maintenant

    # ─── Phase 2 : Retour à la normale ────────────────────────────────────────
    # On itère sur une copie de la liste des clés pour pouvoir supprimer
    # des entrées pendant la boucle sans provoquer d'erreur RuntimeError.
    for alerte_id in list(alertes_actives.keys()):

        # On ne traite que les alertes appartenant au device courant
        if not alerte_id.startswith(f"{point.device_id}."):
            continue

        # L'alerte n'est plus dans les alertes détectées → retour à la normale
        if alerte_id not in ids_alertes_detectees:
            alerte_memorisee = alertes_actives[alerte_id]
            label            = alerte_memorisee.get("label", alerte_id)
            derniere_valeur  = alerte_memorisee.get("derniere_valeur")
            unite            = alerte_memorisee.get("unite", "")

            sujet   = f"RETOUR NORMAL IRM - {label}"
            message = (
                f"La mesure suivante est revenue dans les valeurs normales :\n\n"
                f"{label}\n\n"
                f"Dernière valeur en alerte : {derniere_valeur} {unite}\n"
                f"Heure locale : {formater_heure(maintenant)}"
            )
            envoyer_mail(sujet, message)
            del alertes_actives[alerte_id]  # Suppression de l'alerte résolue
