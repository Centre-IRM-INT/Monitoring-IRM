"""
Module de rapport quotidien - Monitocrio

Genere et envoie chaque nuit un mail recapitulatif de la journee ecoulee :
statistiques sur l'eau glacee et liste des depassements de seuil detectes
dans les CSV locaux.

Version : 1.0
Auteur  : Romaiin
Date    : 25/08/2026
"""

import csv
import threading
import time
from pathlib import Path
from datetime import timedelta
from statistics import mean

from mail.alerte_manager import envoyer_mail, heure_locale, charger_seuils

# ==============================
# CONFIGURATION
# ==============================

DATA_DIR = Path("/home/monitocrio/Documents/Test-com/data")

# Mesure eau glacee mise en avant dans le rapport (demande explicite)
DEVICE_EAU_GLACEE = "arduino_ntc_01"
CHAMP_EAU_GLACEE  = "temperature"

HEURE_RAPPORT = 0  # 0 = minuit, juste apres le changement de jour


# ==============================
# LECTURE DU CSV DU JOUR
# ==============================

def convertir_ligne(row: dict) -> dict:
    """
    Convertit les champs numeriques du CSV (tout est string a la lecture).
    Les champs non convertibles ou vides sont mis a None plutot que de faire
    echouer la lecture de toute la ligne.
    """
    ligne = {"timestamp": row.get("timestamp"), "device_id": row.get("device_id")}
    for champ, valeur in row.items():
        if champ in ("timestamp", "device_id"):
            continue
        if valeur is None or valeur == "":
            ligne[champ] = None
        else:
            try:
                ligne[champ] = float(valeur)
            except ValueError:
                ligne[champ] = None
    return ligne


def lire_csv_jour(date_str: str) -> list:
    """
    Lit le CSV correspondant a date_str (format YYYY-MM-DD), comme les
    fichiers ecrits par log_csv() dans main.py. Retourne une liste vide
    si le fichier n'existe pas encore.
    """
    filepath = DATA_DIR / f"{date_str}.csv"
    if not filepath.exists():
        print(f"[RAPPORT] Pas de CSV trouve pour {date_str} ({filepath})")
        return []

    lignes = []
    with filepath.open("r", encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter=";")
        for row in reader:
            lignes.append(convertir_ligne(row))
    return lignes


# ==============================
# ANALYSE EAU GLACEE
# ==============================

def analyser_eau_glacee(mesures: list):
    """
    Calcule la moyenne, le min et le max de temperature de l'eau glacee
    sur la journee, avec l'heure a laquelle chaque extreme a ete releve.
    Retourne None si aucune mesure n'est disponible.
    """
    valeurs = [
        (m["timestamp"], m[CHAMP_EAU_GLACEE])
        for m in mesures
        if m["device_id"] == DEVICE_EAU_GLACEE and m.get(CHAMP_EAU_GLACEE) is not None
    ]
    if not valeurs:
        return None

    temperatures = [v for _, v in valeurs]
    ts_min, val_min = min(valeurs, key=lambda x: x[1])
    ts_max, val_max = max(valeurs, key=lambda x: x[1])

    return {
        "moyenne":    mean(temperatures),
        "min":        val_min,
        "min_ts":     ts_min,
        "max":        val_max,
        "max_ts":     ts_max,
        "nb_mesures": len(temperatures),
    }


# ==============================
# ANALYSE DES DEPASSEMENTS DE SEUIL
# ==============================

def analyser_depassements(device_id: str, mesures: list, config_device: dict):
    """
    Meme logique que verifier_seuils() dans alerte_manager.py, appliquee
    a toutes les lignes de la journee au lieu d'un seul point. Retourne
    une liste : un element par mesure/type de depassement, avec le nombre
    d'occurrences et la valeur la plus extreme relevee.
    """
    mesures_device = [m for m in mesures if m["device_id"] == device_id]
    depassements = []

    for champ, config in config_device.items():
        if not config.get("enabled", False):
            continue

        label = config.get("label", champ)
        unite = config.get("unit", "")

        occurrences_basses = []
        occurrences_hautes = []
        occurrences_etat   = []

        for m in mesures_device:
            valeur = m.get(champ)
            if valeur is None:
                continue

            if "min" in config and valeur < config["min"]:
                occurrences_basses.append((valeur, m["timestamp"]))

            if "max" in config and valeur > config["max"]:
                occurrences_hautes.append((valeur, m["timestamp"]))

            if "expected" in config and valeur not in config["expected"]:
                occurrences_etat.append((valeur, m["timestamp"]))

        if occurrences_basses:
            # On ne garde que l'occurrence la plus basse de la journee pour ce champ
            pire = min(occurrences_basses, key=lambda o: o[0])
            depassements.append({
                "device_id":      device_id,
                "champ":          champ,
                "label":          label,
                "unite":          unite,
                "description":    "trop basse",
                "nb_occurrences": len(occurrences_basses),
                "valeur_extreme": pire[0],
                "heure_extreme":  pire[1],
            })

        if occurrences_hautes:
            # On ne garde que l'occurrence la plus haute de la journee pour ce champ
            pire = max(occurrences_hautes, key=lambda o: o[0])
            depassements.append({
                "device_id":      device_id,
                "champ":          champ,
                "label":          label,
                "unite":          unite,
                "description":    "trop elevee",
                "nb_occurrences": len(occurrences_hautes),
                "valeur_extreme": pire[0],
                "heure_extreme":  pire[1],
            })

        if occurrences_etat:
            # Pas de notion d'extreme pour un etat discret, on garde la premiere occurrence
            pire = occurrences_etat[0]
            depassements.append({
                "device_id":      device_id,
                "champ":          champ,
                "label":          label,
                "unite":          unite,
                "description":    "valeur incorrecte",
                "nb_occurrences": len(occurrences_etat),
                "valeur_extreme": pire[0],
                "heure_extreme":  pire[1],
            })

    return depassements


# ==============================
# CONSTRUCTION DU RAPPORT
# ==============================

def formater_heure_courte(iso_ts: str) -> str:
    """
    Extrait l'heure HH:MM d'un timestamp ISO du type "2026-07-01T14:32:05+02:00".
    Retourne le timestamp brut si le format ne correspond pas.
    """
    try:
        return iso_ts.split("T")[1][:5]
    except Exception:
        return iso_ts


def construire_rapport(date_str: str, stats_eau: dict, depassements: list) -> str:
    """
    Assemble le texte final du mail de rapport quotidien a partir des
    statistiques eau glacee et de la liste des depassements de seuil.
    """
    lignes = []
    lignes.append(f"RAPPORT QUOTIDIEN IRM - {date_str}")
    lignes.append("=" * 40)
    lignes.append("")

    lignes.append("EAU GLACEE")
    lignes.append("-" * 45)
    if stats_eau:
        lignes.append(f"Temperature moyenne : {stats_eau['moyenne']:.1f} C  ({stats_eau['nb_mesures']} mesures)")
        lignes.append(f"Temperature min     : {stats_eau['min']:.1f} C  a {formater_heure_courte(stats_eau['min_ts'])}")
        lignes.append(f"Temperature max     : {stats_eau['max']:.1f} C  a {formater_heure_courte(stats_eau['max_ts'])}")
    else:
        lignes.append("Aucune mesure d'eau glacee disponible pour cette journee.")
    lignes.append("")

    lignes.append("DEPASSEMENTS DE SEUIL")
    lignes.append("-" * 45)
    if not depassements:
        lignes.append("0 depassement de seuil.")
    else:
        lignes.append(f"{len(depassements)} depassement(s) de seuil :")
        lignes.append("")
        for i, d in enumerate(depassements, start=1):
            lignes.append(
                f"{i}. {d['label']} ({d['device_id']}) - {d['description']} "
                f"- {d['nb_occurrences']} fois"
            )
            lignes.append(
                f"   Valeur la plus extreme : {d['valeur_extreme']} {d['unite']} "
                f"a {formater_heure_courte(d['heure_extreme'])}"
            )
    lignes.append("")

    return "\n".join(lignes)


# ==============================
# GENERATION + ENVOI
# ==============================

def envoyer_rapport_quotidien():
    """
    Point d'entree principal : lit le CSV de la veille, calcule les stats
    et les depassements, construit le rapport et l'envoie par mail.
    """
    hier = heure_locale() - timedelta(days=1)
    date_str_fichier  = hier.strftime("%Y-%m-%d")
    date_str_affiche  = hier.strftime("%d/%m/%Y")

    seuils = charger_seuils()
    if not seuils:
        # ATTENTION : ce cas couvre aussi bien l'absence de config_seuils.json
        # qu'un JSON malforme (charger_seuils() retourne {} dans les deux cas),
        # ce qui annule le rapport sans notification. A corriger si besoin.
        print("[RAPPORT] Pas de config_seuils.json, rapport annule")
        return

    mesures = lire_csv_jour(date_str_fichier)

    if not mesures:
        envoyer_mail(
            f"Rapport quotidien IRM - {date_str_affiche}",
            f"RAPPORT QUOTIDIEN IRM - {date_str_affiche}\n\n"
            f"Aucune donnee disponible pour cette journee (CSV introuvable ou vide)."
        )
        print(f"[RAPPORT] Aucun CSV pour {date_str_fichier}, mail d'absence de donnees envoye")
        return

    depassements_total = []
    for device_id, config_device in seuils.items():
        if device_id == "watchdog":
            # La section "watchdog" n'est pas un device, on l'exclut de l'analyse
            continue
        depassements_total.extend(analyser_depassements(device_id, mesures, config_device))

    stats_eau = analyser_eau_glacee(mesures)
    rapport   = construire_rapport(date_str_affiche, stats_eau, depassements_total)

    envoyer_mail(f"Rapport quotidien IRM - {date_str_affiche}", rapport)
    print(f"[RAPPORT] Rapport quotidien envoye pour le {date_str_affiche}")


# ==============================
# PLANIFICATEUR
# ==============================

def _scheduler_rapport():
    """
    Boucle de fond qui calcule l'attente jusqu'a la prochaine occurrence
    de HEURE_RAPPORT puis declenche l'envoi du rapport, en boucle infinie.
    """
    while True:
        maintenant = heure_locale()
        prochain = maintenant.replace(hour=HEURE_RAPPORT, minute=0, second=0, microsecond=0)
        if prochain <= maintenant:
            prochain += timedelta(days=1)
        attente = (prochain - maintenant).total_seconds()
        print(f"[RAPPORT] Prochain rapport dans {attente / 3600:.1f}h")
        time.sleep(attente)
        envoyer_rapport_quotidien()


def demarrer_rapport_quotidien():
    """A appeler au startup FastAPI, comme demarrer_watchdog()."""
    t = threading.Thread(target=_scheduler_rapport, daemon=True)
    t.start()
    print("[RAPPORT] Planificateur de rapport quotidien demarre")
