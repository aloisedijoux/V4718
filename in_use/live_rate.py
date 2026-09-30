#!/usr/bin/env python3
"""
live_rate.py

Affiche en temps reel le taux de coups (hits/s) recus sur stdin,
depuis un flux CSV "numero_trigger,delai_ns" (ou n'importe quel flux
d'une ligne par hit) produit par read_output_buffer_blt en mode
stream. Concu pour observer en direct l'effet d'un reglage physique
(ex: disc level d'un CFD) sur le taux de comptage d'un channel, en
Continuous Storage (pas de notion de trigger ici, on compte juste les
coups qui arrivent).

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s 0 | python3 live_rate.py

Variables d'environnement :
  RATE_BIN_S       largeur d'un bin de comptage, en secondes (defaut 0.5)
  RATE_WINDOW_S    duree affichee a l'ecran, en secondes (defaut 30)
  RATE_INTERVAL_MS periode de rafraichissement de la figure, en ms (defaut 300)
  RATE_CSV_OUT     si defini, chemin d'un fichier CSV ou chaque hit recu est
                    enregistre au fur et a mesure : "temps_ecoule_s,numero_trigger,delai_ns"
                    (temps_ecoule_s = temps reel depuis le lancement du script,
                    utile pour recouper avec un changement manuel, ex: disc
                    level d'un CFD, fait pendant l'acquisition)
"""

import csv
import os
import sys
import time
import threading

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

BIN_S = float(os.environ.get("RATE_BIN_S", "0.5"))
WINDOW_S = float(os.environ.get("RATE_WINDOW_S", "30"))
INTERVAL_MS = int(os.environ.get("RATE_INTERVAL_MS", "300"))
CSV_OUT = os.environ.get("RATE_CSV_OUT")

lock = threading.Lock()
stdin_closed = threading.Event()
t_start = time.monotonic()
counts_by_bin = {}
total_hits = 0


def reader():
    global total_hits
    csv_file = open(CSV_OUT, "w", newline="", encoding="utf-8") if CSV_OUT else None
    csv_writer = csv.writer(csv_file) if csv_file else None
    if csv_writer:
        csv_writer.writerow(["temps_ecoule_s", "numero_trigger", "delai_ns"])

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        now = time.monotonic() - t_start
        bin_idx = int(now // BIN_S)
        with lock:
            counts_by_bin[bin_idx] = counts_by_bin.get(bin_idx, 0) + 1
            total_hits += 1
        if csv_writer:
            parts = line.split(",")
            trigger_num = parts[0] if len(parts) >= 2 else ""
            delay = parts[-1]
            csv_writer.writerow([f"{now:.6f}", trigger_num, delay])
            if total_hits % 200 == 0:
                csv_file.flush()
    stdin_closed.set()
    if csv_file:
        csv_file.close()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

fig, ax = plt.subplots(figsize=(9, 5))


def update(_frame):
    now = time.monotonic() - t_start
    current_bin = int(now // BIN_S)
    first_bin = max(0, current_bin - int(WINDOW_S / BIN_S))

    with lock:
        bins = list(range(first_bin, current_bin + 1))
        rates = [counts_by_bin.get(b, 0) / BIN_S for b in bins]
        total = total_hits

    times = [b * BIN_S for b in bins]

    ax.clear()
    ax.plot(times, rates, color="#4c72b0", linewidth=1.5, marker=".", markersize=3)
    if rates:
        ax.axhline(rates[-1], color="#c44e52", linestyle="--", linewidth=1,
                    label=f"taux actuel = {rates[-1]:.0f} hits/s")
        ax.legend(loc="upper left", fontsize=9)

    status = "stdin fermé" if stdin_closed.is_set() else "en direct"
    ax.set_xlabel("Temps écoulé (s)")
    ax.set_ylabel(f"Taux de coups (hits/s, bin={BIN_S:.1f}s)")
    ax.set_title(f"Taux de coups channel -- N total={total} ({status})")
    ax.set_ylim(bottom=0)
    fig.tight_layout()


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
