#!/usr/bin/env python3
"""
live_histogram.py

Trace un histogramme en temps reel a partir d'un flux CSV
"numero_trigger,delai_ns" recu sur stdin -- concu pour etre pipe depuis
read_output_buffer_blt en mode stream (-s offset_ns), qui emet une
ligne par TDC MEASUREMENT : le numero de trigger (event_count) et le
delai (ns) hit<->trigger. Seul le delai (2e champ) est trace ici.

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -1000 | python3 live_histogram.py

Options (variables d'environnement, pour rester simple en ligne de
commande) :
  LIVEHIST_BINS       nombre de bins de l'histogramme (defaut 60)
  LIVEHIST_INTERVAL_MS periode de rafraichissement en ms (defaut 500)
  LIVEHIST_MAXPOINTS   nombre max de points gardes en memoire, les plus
                       anciens sont ensuite abandonnes (defaut 200000,
                       0 = illimite)
  LIVEHIST_MIN        coupure basse (ns) : valeurs < MIN ignorees (defaut
                       aucune)
  LIVEHIST_MAX        coupure haute (ns) : valeurs > MAX ignorees (defaut
                       aucune)

Exemple pour exclure la queue basse vers -700ns :
  LIVEHIST_MIN=-690 ./read_output_buffer_blt ... -c -s -2000 | LIVEHIST_MIN=-690 python3 live_histogram.py
"""

import os
import sys
import threading

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

BINS = int(os.environ.get("LIVEHIST_BINS", "60"))
INTERVAL_MS = int(os.environ.get("LIVEHIST_INTERVAL_MS", "500"))
MAXPOINTS = int(os.environ.get("LIVEHIST_MAXPOINTS", "200000"))
CUT_MIN = os.environ.get("LIVEHIST_MIN")
CUT_MIN = float(CUT_MIN) if CUT_MIN is not None else None
CUT_MAX = os.environ.get("LIVEHIST_MAX")
CUT_MAX = float(CUT_MAX) if CUT_MAX is not None else None

data = []
lock = threading.Lock()
stdin_closed = threading.Event()


def reader():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        # format CSV "numero_trigger,delai_ns" -- on ne garde que le
        # dernier champ (reste compatible avec un flux a une seule
        # colonne, si jamais).
        try:
            value = float(line.rsplit(",", 1)[-1])
        except ValueError:
            continue
        if CUT_MIN is not None and value < CUT_MIN:
            continue
        if CUT_MAX is not None and value > CUT_MAX:
            continue
        with lock:
            data.append(value)
            if MAXPOINTS > 0 and len(data) > MAXPOINTS:
                del data[: len(data) - MAXPOINTS]
    stdin_closed.set()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

fig, ax = plt.subplots(figsize=(8, 5))


def update(_frame):
    with lock:
        snapshot = list(data)

    ax.clear()
    if snapshot:
        ax.hist(snapshot, bins=BINS, color="#4c72b0", edgecolor="black", linewidth=0.3)
        mean = sum(snapshot) / len(snapshot)
        ax.axvline(mean, color="#c44e52", linestyle="--", linewidth=1,
                   label=f"moyenne = {mean:.2f}")
        ax.legend(loc="upper right")

    status = "stdin ferme" if stdin_closed.is_set() else "en direct"
    ax.set_xlabel("Delai par rapport au trigger (ns)")
    ax.set_ylabel("Coups")
    ax.set_title(f"Histogramme temps reel -- N={len(snapshot)} ({status})")
    fig.tight_layout()


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
