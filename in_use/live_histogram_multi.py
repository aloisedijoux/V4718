#!/usr/bin/env python3
"""
live_histogram_multi.py

Histogramme en temps reel, un jeu de couleur par channel, a partir du
flux CSV "numero_trigger,channel,delai_ns" produit par
read_output_buffer_blt en mode stream (-s). Sert a comparer plusieurs
channels actifs en meme temps (ex: montage a deux scintillateurs sur
channel 13 et channel 4) : superposition des distributions de delai
+ frequence (hits/s) de chaque channel affichee dans la legende.

Les comptages par bin sont accumules directement dans le thread lecteur
(bins fixes, calcules une seule fois au demarrage) -- chaque frame ne fait
que mettre a jour les courbes deja crees avec les comptages courants, au
lieu de refaire ax.clear() + ax.hist() sur l'historique complet a chaque
fois. L'ancienne version recalculait tout depuis le debut (jusqu'a 200000
points/channel) a chaque rafraichissement, ce qui ralentit de plus en plus
a mesure que l'acquisition dure -- symptome rapporte : "l'histogramme
s'actualise mal" meme avec un seul channel actif.

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -31350 \
      | python3 live_histogram_multi.py

Variables d'environnement :
  HISTM_BINS         nombre de bins (defaut 60)
  HISTM_RANGE_NS      plage fixe de l'histogramme "MIN,MAX" en ns (defaut
                      "-2500,2500") -- a adapter a la fenetre de trigger
                      reellement utilisee (offset .. offset+largeur) ; les
                      hits hors plage sont comptes dans le total/taux mais
                      pas affiches dans l'histogramme.
  HISTM_INTERVAL_MS  periode de rafraichissement en ms (defaut 500)
"""

import os
import sys
import threading
import time

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

BINS = int(os.environ.get("HISTM_BINS", "60"))
INTERVAL_MS = int(os.environ.get("HISTM_INTERVAL_MS", "500"))
_range_raw = os.environ.get("HISTM_RANGE_NS", "-2500,2500")
try:
    RANGE_MIN, RANGE_MAX = (float(x) for x in _range_raw.split(","))
except ValueError:
    sys.exit(f"ERREUR : HISTM_RANGE_NS invalide ({_range_raw!r}), attendu 'MIN,MAX' ex: -2500,2500")

EDGES = np.linspace(RANGE_MIN, RANGE_MAX, BINS + 1)
CENTERS = 0.5 * (EDGES[:-1] + EDGES[1:])

COLORS = ["#4c72b0", "#c44e52", "#55a868", "#8172b2", "#ccb974", "#64b5cd"]

lock = threading.Lock()
stdin_closed = threading.Event()
t_start = time.monotonic()
counts_by_channel = {}   # channel -> np.array(BINS,) cumulative bin counts
first_seen = {}          # channel -> temps (s) du premier hit
last_seen = {}           # channel -> temps (s) du dernier hit
n_by_channel = {}        # channel -> total recu (y compris hors plage)


def reader():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            channel = int(float(parts[1]))
            delay = float(parts[2])
        except ValueError:
            continue
        now = time.monotonic() - t_start
        with lock:
            if channel not in counts_by_channel:
                counts_by_channel[channel] = np.zeros(BINS, dtype=np.int64)
                first_seen[channel] = now
                n_by_channel[channel] = 0
            if RANGE_MIN <= delay < RANGE_MAX:
                bin_idx = int((delay - RANGE_MIN) / (RANGE_MAX - RANGE_MIN) * BINS)
                bin_idx = min(bin_idx, BINS - 1)
                counts_by_channel[channel][bin_idx] += 1
            n_by_channel[channel] += 1
            last_seen[channel] = now
    stdin_closed.set()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

fig, ax = plt.subplots(figsize=(9, 5.5))
ax.set_xlim(RANGE_MIN, RANGE_MAX)
ax.set_xlabel("Délai par rapport au trigger (ns)")
ax.set_ylabel("Coups")
title = ax.set_title("Histogramme multi-canal -- N total=0 (en direct)")
legend = None
lines_by_channel = {}  # channel -> Line2D (step plot), created lazily
fig.tight_layout()


def update(_frame):
    global legend
    with lock:
        channels = sorted(counts_by_channel.keys())
        snapshot = {ch: counts_by_channel[ch].copy() for ch in channels}
        n_total_by_ch = {ch: n_by_channel[ch] for ch in channels}
        t0 = dict(first_seen)
        t1 = dict(last_seen)

    ymax = 1
    for idx, ch in enumerate(channels):
        counts = snapshot[ch]
        ymax = max(ymax, int(counts.max()))
        span = max(t1[ch] - t0[ch], 1e-6)
        rate = n_total_by_ch[ch] / span
        label = f"channel {ch} (N={n_total_by_ch[ch]}, {rate:.0f} hits/s)"

        if ch not in lines_by_channel:
            color = COLORS[idx % len(COLORS)]
            (line,) = ax.step(CENTERS, counts, where="mid", linewidth=1.8, color=color, label=label)
            lines_by_channel[ch] = line
        else:
            lines_by_channel[ch].set_data(CENTERS, counts)
            lines_by_channel[ch].set_label(label)

    ax.set_ylim(0, ymax * 1.1)

    status = "stdin fermé" if stdin_closed.is_set() else "en direct"
    total_n = sum(n_total_by_ch.values())
    title.set_text(f"Histogramme multi-canal -- N total={total_n} ({status})")
    if channels:
        legend = ax.legend(loc="upper right", fontsize=9)


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
