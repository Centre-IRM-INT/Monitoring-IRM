"""
Module de gestion des alertes - Monitocrio

Gère l'envoi des mails d'alerte (seuils dépassés, retour à la normale)
et la surveillance de silence des modules Arduino (watchdog).

Version : 2.0
Auteur  : Romaiin
Date    : 25/08/2026
"""

import json
import smtplib
import threading
import time
from pathlib import Path
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo
from email.mime.text import MIMEText

# ==============================
# CONFIGURATION
# ==============================

# Chemin vers le fichier de seuils, lu à chaque appel pour prendre en compte les modifications à chaud
CONFIG_SEUILS_FILE = Path("/home/monitocrio/Documents/Test-com/seuils/config_seuils.json")

SMTP_SERVER = "smtp.gmail.com"
SMTP_PORT   = 587

EMAIL    = "automate.irm.2026@gmail.com"
PASSWORD = "nvkv jhfz fqco ogdu"

DESTINATAIRES = [
    "romain.poos@univ-amu.fr",
    "bruno.nazarian@univ-amu.fr",
    "jean-luc.anton@univ-amu.fr",
    "julien.sein@univ-amu.fr"
]

# Délai minimum entre deux mails de rappel pour une même alerte de seuil déjà active
DELAI_RAPPEL_ALERTE = timedelta(hours=1)

# Liste des modules surveillés par le watchdog de silence
DEVICES = [
    "arduino_atmo",
    "arduino_ntc_01",
    "arduino_cta"
]

# Dictionnaire des alertes de seuils actuellement actives, indexé par id d'alerte
alertes_actives = {}


# ==============================
# OUTILS DATE / HEURE
# ==============================

def heure_locale():
    """
    Retourne l'heure actuelle dans le fuseau Europe/Paris.
    Utilisée partout dans le module pour garder une cohérence horaire dans les mails.
    """
    return datetime.now(ZoneInfo("Europe/Paris"))


def formater_heure(dt: datetime):
    """
    Formate une date au format JJ/MM/AAAA à HH:MM pour l'affichage dans les mails.
    """
    return dt.strftime("%d/%m/%Y à %H:%M")


# ==============================
# ENVOI MAIL
# ==============================

def envoyer_mail(sujet: str, message: str):
    """
    Envoie un mail texte brut à la liste des destinataires via SMTP (Gmail).
    Retourne True si l'envoi a réussi, False sinon.
    """
    msg = MIMEText(message, "plain", "utf-8")
    msg["Subject"] = sujet
    msg["From"]    = EMAIL
    msg["To"]      = ", ".join(DESTINATAIRES)

    try:
        with smtplib.SMTP(SMTP_SERVER, SMTP_PORT, timeout=10) as server:
            server.starttls()
            server.login(EMAIL, PASSWORD)
            server.send_message(msg, from_addr=EMAIL, to_addrs=DESTINATAIRES)
        print("[MAIL] Mail envoyé :", sujet)
        return True

    except Exception as e:
        print("[MAIL] Erreur envoi mail :", e)
        return False


# ==============================
# LECTURE DES SEUILS
# ==============================

def charger_seuils():
    """
    Charge et retourne le contenu du fichier config_seuils.json.
    Retourne un dictionnaire vide en cas de fichier absent ou de JSON invalide,
    ce qui désactive silencieusement les alertes concernées.
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


def charger_config_watchdog():
    """
    Extrait la configuration du watchdog de silence depuis config_seuils.json,
    avec des valeurs par défaut si la section "watchdog" est absente.
    """
    seuils = charger_seuils()
    cfg    = seuils.get("watchdog", {})
    return {
        "delai_alerte_minutes":         cfg.get("delai_alerte_minutes", 15),
        "rappels_minutes":              cfg.get("rappels_minutes", [15, 30]),
        "rappel_horaire_apres_minutes": cfg.get("rappel_horaire_apres_minutes", 60)
    }


# ==============================
# WATCHDOG — ETAT PAR DEVICE
# ==============================

# Etat interne du watchdog pour chaque device, protégé par _watchdog_lock
_watchdog_state: dict = {}
_watchdog_lock = threading.Lock()


def _init_device(device_id: str):
    """
    Initialise l'état watchdog d'un device s'il n'existe pas encore
    dans _watchdog_state.
    """
    if device_id not in _watchdog_state:
        _watchdog_state[device_id] = {
            "derniere_mesure":                  None,
            "alerte_active":                    False,
            "dernier_mail":                     None,
            "minutes_ecoules_au_dernier_mail":  0
        }


def signaler_mesure_recue(device_id: str):
    """
    À appeler dans main.py à chaque mesure reçue d'un device.
    Remet à zéro le watchdog et envoie un mail de confirmation
    si une alerte de silence était active.
    """
    maintenant = heure_locale()

    with _watchdog_lock:
        _init_device(device_id)
        etat = _watchdog_state[device_id]

        if etat["alerte_active"]:
            # Calcul de la durée totale du silence en tenant compte des minutes
            # déjà écoulées au moment du dernier mail de rappel envoyé
            dernier_mail_il_y_a = maintenant - etat["dernier_mail"]
            minutes_silence     = int(dernier_mail_il_y_a.total_seconds() / 60) \
                                  + etat["minutes_ecoules_au_dernier_mail"]

            sujet   = f"REPRISE IRM - {device_id}"
            message = (
                f"Le module {device_id} a repris l'envoi de mesures.\n\n"
                f"Durée totale du silence : ~{minutes_silence} minutes\n"
                f"Heure de reprise : {formater_heure(maintenant)}\n\n"
                f"Aucune action supplémentaire requise."
            )
            envoyer_mail(sujet, message)
            print(f"[WATCHDOG] {device_id} a repris - alerte levée")

        etat["derniere_mesure"]                 = maintenant
        etat["alerte_active"]                   = False
        etat["dernier_mail"]                    = None
        etat["minutes_ecoules_au_dernier_mail"] = 0


def _verifier_silences():
    """
    Boucle de fond exécutée dans un thread dédié.
    Vérifie toutes les minutes si un device n'a plus envoyé de mesure
    depuis trop longtemps et déclenche les mails d'alerte / de rappel.
    """
    while True:
        time.sleep(60)
        maintenant = heure_locale()
        cfg        = charger_config_watchdog()

        delai_alerte   = cfg["delai_alerte_minutes"]
        rappels        = sorted(cfg["rappels_minutes"])
        seuil_horaire  = cfg["rappel_horaire_apres_minutes"]

        with _watchdog_lock:
            for device_id in DEVICES:
                _init_device(device_id)
                etat = _watchdog_state[device_id]

                if etat["derniere_mesure"] is None:
                    # Aucune mesure reçue depuis le démarrage, rien à surveiller
                    continue

                silence_minutes = (maintenant - etat["derniere_mesure"]).total_seconds() / 60

                if silence_minutes < delai_alerte:
                    continue

                envoyer = False

                if not etat["alerte_active"]:
                    # Premier dépassement du délai d'alerte, premier mail
                    envoyer             = True
                    etat["alerte_active"] = True

                else:
                    dernier_mail      = etat["dernier_mail"]
                    mins_depuis_mail  = (maintenant - dernier_mail).total_seconds() / 60
                    mins_au_dernier   = etat["minutes_ecoules_au_dernier_mail"]

                    # Rappels rapprochés définis dans "rappels_minutes" (ex : 15, 30 min)
                    for palier in rappels:
                        if palier > delai_alerte:
                            if mins_au_dernier < palier <= silence_minutes:
                                envoyer = True
                                break

                    # Au-delà du seuil horaire, on bascule sur un rappel toutes les heures
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
                    etat["dernier_mail"]                    = maintenant
                    etat["minutes_ecoules_au_dernier_mail"] = mins_int
                    print(f"[WATCHDOG] Mail silence envoyé - {device_id} ({mins_int} min)")


def demarrer_watchdog():
    """
    Lance le thread watchdog en arrière-plan.
    À appeler au démarrage de FastAPI (startup event).
    """
    t = threading.Thread(target=_verifier_silences, daemon=True)
    t.start()
    print("[WATCHDOG] Démarré - surveillance de", DEVICES)


# ==============================
# COMPARAISON AUX SEUILS
# ==============================

def verifier_seuils(point):
    """
    Compare les valeurs d'un point de mesure aux seuils définis pour son device
    dans config_seuils.json et retourne la liste des alertes détectées.
    """
    seuils  = charger_seuils()
    alertes = []

    config_device = seuils.get(point.device_id)
    if not config_device:
        # Aucun seuil défini pour ce device
        return alertes

    data = point.dict()

    for champ, config in config_device.items():
        if not config.get("enabled", False):
            continue

        valeur = data.get(champ)
        if valeur is None:
            continue

        label = config.get("label", champ)
        unite = config.get("unit", "")
        seuil_min = config.get("min")
        seuil_max = config.get("max")

        if "min" in config and valeur < config["min"]:
            alertes.append({
                "id":        f"{point.device_id}.{champ}.low",
                "device_id": point.device_id,
                "champ":     champ,
                "label":     label,
                "valeur":    valeur,
                "unite":     unite,
                "min":       seuil_min,
                "max":       seuil_max,
                "message":   config.get("message_low", f"{label} trop basse.")
            })

        if "max" in config and valeur > config["max"]:
            alertes.append({
                "id":        f"{point.device_id}.{champ}.high",
                "device_id": point.device_id,
                "champ":     champ,
                "label":     label,
                "valeur":    valeur,
                "unite":     unite,
                "min":       seuil_min,
                "max":       seuil_max,
                "message":   config.get("message_high", f"{label} trop élevée.")
            })

        # Cas des valeurs discrètes attendues (ex : état marche/défaut), sans bornes min/max
        if "expected" in config and valeur not in config["expected"]:
            alertes.append({
                "id":        f"{point.device_id}.{champ}.expected",
                "device_id": point.device_id,
                "champ":     champ,
                "label":     label,
                "valeur":    valeur,
                "unite":     unite,
                "min":       seuil_min,
                "max":       seuil_max,
                "message":   config.get("message_expected", f"{label} valeur incorrecte.")
            })

    return alertes


# ==============================
# TEXTE DES SEUILS POUR LES MAILS
# ==============================

def formater_seuils(seuil_min, seuil_max, unite):
    """
    Construit la phrase décrivant les seuils définis, insérée dans les mails
    d'alerte et de retour à la normale.
    Retourne "non spécifiés" si aucune borne min/max n'est définie
    (cas des alertes sur valeur "expected").
    """
    if seuil_min is not None and seuil_max is not None:
        return f"Seuils définis : min {seuil_min} {unite} / max {seuil_max} {unite}"
    elif seuil_min is not None:
        return f"Seuil défini : min {seuil_min} {unite}"
    elif seuil_max is not None:
        return f"Seuil défini : max {seuil_max} {unite}"
    else:
        return "Seuils définis : non spécifiés"


# ==============================
# GESTION DES ALERTES SEUILS
# ==============================

def traiter_alertes(point):
    """
    À appeler pour chaque point de mesure reçu.
    Compare le point aux seuils, envoie les mails d'alerte (nouvelles alertes
    ou rappels après DELAI_RAPPEL_ALERTE) et détecte les retours à la normale
    pour les alertes qui ne sont plus détectées.
    """
    global alertes_actives

    maintenant            = heure_locale()
    alertes_detectees     = verifier_seuils(point)
    ids_alertes_detectees = {alerte["id"] for alerte in alertes_detectees}
    data_point             = point.dict()

    # 1. Alertes nouvelles ou toujours actives
    for alerte in alertes_detectees:
        alerte_id = alerte["id"]
        envoyer   = False

        if alerte_id not in alertes_actives:
            # Nouvelle alerte, premier mail immédiat
            envoyer = True
            alertes_actives[alerte_id] = {
                "premiere_detection": maintenant,
                "dernier_mail":       None,
                "derniere_valeur":    alerte["valeur"],
                "label":              alerte["label"],
                "device_id":          alerte["device_id"],
                "champ":              alerte["champ"],
                "unite":              alerte["unite"],
                "min":                alerte["min"],
                "max":                alerte["max"]
            }
        else:
            # Alerte déjà active, on renvoie un mail seulement après le délai de rappel
            dernier_mail = alertes_actives[alerte_id]["dernier_mail"]
            if dernier_mail is None:
                envoyer = True
            elif maintenant - dernier_mail >= DELAI_RAPPEL_ALERTE:
                envoyer = True
            alertes_actives[alerte_id]["derniere_valeur"] = alerte["valeur"]
            alertes_actives[alerte_id]["min"]              = alerte["min"]
            alertes_actives[alerte_id]["max"]              = alerte["max"]

        if envoyer:
            seuils_txt = formater_seuils(alerte["min"], alerte["max"], alerte["unite"])
            sujet      = f"ALERTE IRM - {alerte['label']}"
            message    = (
                f"{alerte['message']}\n\n"
                f"Équipement : {alerte['device_id']}\n"
                f"Mesure : {alerte['label']}\n"
                f"Valeur reçue : {alerte['valeur']} {alerte['unite']}\n"
                f"{seuils_txt}\n"
                f"Heure locale : {formater_heure(maintenant)}\n\n"
                f"Veuillez vérifier le système."
            )
            envoyer_mail(sujet, message)
            alertes_actives[alerte_id]["dernier_mail"] = maintenant

    # 2. Retour à la normale : on parcourt les alertes actives de ce device
    # et on ferme celles qui ne sont plus détectées dans ce point de mesure
    for alerte_id in list(alertes_actives.keys()):
        if not alerte_id.startswith(f"{point.device_id}."):
            continue
        if alerte_id not in ids_alertes_detectees:
            alerte_memorisee = alertes_actives[alerte_id]
            label            = alerte_memorisee.get("label", alerte_id)
            derniere_valeur  = alerte_memorisee.get("derniere_valeur")
            unite            = alerte_memorisee.get("unite", "")
            seuil_min        = alerte_memorisee.get("min")
            seuil_max        = alerte_memorisee.get("max")
            champ            = alerte_memorisee.get("champ")
            nouvelle_valeur  = data_point.get(champ)

            seuils_txt = formater_seuils(seuil_min, seuil_max, unite)
            sujet      = f"RETOUR NORMAL IRM - {label}"
            message    = (
                f"La mesure suivante est revenue dans les valeurs normales :\n\n"
                f"{label}\n\n"
                f"Dernière valeur en alerte : {derniere_valeur} {unite}\n"
                f"Nouvelle valeur : {nouvelle_valeur} {unite}\n"
                f"{seuils_txt}\n"
                f"Heure locale : {formater_heure(maintenant)}"
            )
            envoyer_mail(sujet, message)
            del alertes_actives[alerte_id]
