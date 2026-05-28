# Monitoring-IRM

Real-time monitoring system for critical MRI room equipment — Raspberry Pi (FastAPI, WebSocket) + Arduino MKR WiFi + Supabase. Local supervision, email alerts, CSV datalogging and remote consultation.

**Final year engineering project (PFE) — INT / CNRS 2026**  
*Author: Romain | Supervisors: Bruno Nazarian, Jean-Luc Anton, Julien Sein*

---

## What it does

The system continuously monitors the critical equipment of an MRI platform:

- **Atmospheric data** — temperature, humidity, pressure (salle IRM)
- **Cold water circuit** — water temperature via NTC probe
- **Helium compressor** — vibration amplitude + logical states
- **Cold group** — on/off state

Each Arduino sends measurements every 60s to the Raspberry Pi, which logs them locally (CSV), pushes them to Supabase, and broadcasts them via WebSocket to the local dashboard. Email alerts are triggered when a threshold is exceeded or a device stops transmitting.

---

## Architecture

```
Arduinos (local network)
    ↓  HTTP POST /data  (every 60s)
Raspberry Pi — FastAPI server (192.168.0.101:8000)
    ├── WebSocket → Local dashboard (real-time)
    ├── CSV       → data/YYYY-MM-DD.csv (local archiving)
    ├── Email     → Threshold alerts + watchdog
    └── Supabase  → Remote dashboard (read-only)
```

---

## Repository structure

```
Monitoring-IRM/
├── app/                        ← FastAPI server (main.py + local dashboard)
├── mail/                       ← Email alert manager
├── seuils/                     ← Alert thresholds config (config_seuils.json)
├── frontend/                   ← Remote dashboard (static HTML — GitHub Pages)
├── arduino/                    ← Arduino sketches (arduino_atmo, arduino_ntc_01)
├── systemd/                    ← systemd service file for auto-start
├── docs/                       ← Full deployment guide + datasheets
├── requirements.txt
├── .env.example
└── .gitignore
```

---

## Deployment

A complete step-by-step deployment guide is available in:

📄 **`docs/Guide_Deploiement.docx`**

It covers in order:
1. Raspberry Pi setup (venv, dependencies, email config, thresholds, systemd)
2. Arduino configuration and upload
3. Supabase setup (table, RLS, auto-delete)
4. Remote dashboard deployment (GitHub Pages)

---

## Remote dashboard

The remote dashboard reads data from Supabase and is hosted on GitHub Pages.  
To deploy your own instance, follow Part 4 of the deployment guide.

---

## Stack

| Component | Technology |
|---|---|
| Server | Python — FastAPI, Uvicorn, WebSocket |
| Acquisition | Arduino MKR WiFi 1010 + MKR ENV Shield |
| Cloud database | Supabase (PostgreSQL + REST API) |
| Local storage | CSV files |
| Remote dashboard | HTML / CSS / JavaScript — GitHub Pages |
| Alerts | SMTP Gmail |
