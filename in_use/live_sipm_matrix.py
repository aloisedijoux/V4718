#!/usr/bin/env python3
"""
live_sipm_matrix.py

Carte de couverture faisceau EN DIRECT sur une grille de barres SiPM
(voir toodo.txt, Todo1), a partir du flux CSV "numero_trigger,channel,
delai_ns" produit par read_output_buffer_blt en mode stream (-s). Se
branche comme live_histogram_multi.py (meme flux d'entree), mais affiche
une matrice C x D (comptage cumule par channel depuis le debut de
l'acquisition) au lieu d'un histogramme, avec le taux de comptage global
mis a jour en direct -- pense pour les acquisitions courtes de 2-3s
decrites dans le Todo1.

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -2000 \
      | SIPM_C_CHANNELS=0,1,2,3 SIPM_D_CHANNELS=8,9,10,11 \
        python3 live_sipm_matrix.py

Variables d'environnement :
  SIPM_C_CHANNELS   channels TDC des barres C (lignes), obligatoire, ex: 0,1,2,3
  SIPM_D_CHANNELS   channels TDC des barres D (colonnes), obligatoire, ex: 8,9,10,11
  SIPM_INTERVAL_MS  periode de rafraichissement en ms (defaut 500)
  SIPM_CMAP         colormap matplotlib (defaut Blues)
"""

import os
import sys
import threading
import time

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation


def _parse_channels(env_name):
    raw = os.environ.get(env_name, "")
    try:
        chans = [int(c.strip()) for c in raw.split(",") if c.strip() != ""]
    except ValueError:
        chans = []
    if not chans:
        sys.exit(f"ERREUR : variable d'environnement {env_name} manquante ou invalide "
                  f"(attendu une liste de channels separes par des virgules, ex: {env_name}=0,1,2,3)")
    return chans


C_CHANNELS = _parse_channels("SIPM_C_CHANNELS")
D_CHANNELS = _parse_channels("SIPM_D_CHANNELS")
INTERVAL_MS = int(os.environ.get("SIPM_INTERVAL_MS", "500"))
CMAP = os.environ.get("SIPM_CMAP", "Blues")

overlap = set(C_CHANNELS) & set(D_CHANNELS)
if overlap:
    print(f"NOTE : channel(s) {sorted(overlap)} present(s) a la fois dans SIPM_C_CHANNELS "
          f"et SIPM_D_CHANNELS -- leur(s) case(s) affichera(ont) un comptage double (C+D du "
          f"meme channel).", file=sys.stderr)

lock = threading.Lock()
stdin_closed = threading.Event()
t_start = time.monotonic()
watched = set(C_CHANNELS) | set(D_CHANNELS)
counts_by_channel = {ch: 0 for ch in watched}


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
        except ValueError:
            continue
        if channel not in watched:
            continue
        with lock:
            counts_by_channel[channel] += 1
    stdin_closed.set()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

N_C = len(C_CHANNELS)
N_D = len(D_CHANNELS)

fig_w = max(4.0, 1.1 * N_D + 2.0)
fig_h = max(4.0, 1.1 * N_C + 2.0)
fig, ax = plt.subplots(figsize=(fig_w, fig_h))

# Build all artists (image, ticks, colorbar, per-cell text) ONCE. Animating by
# mutating them in place (im.set_data/set_clim, cbar.update_normal, text.set_text)
# instead of ax.clear() + re-creating everything each frame -- repeatedly
# removing/recreating a colorbar's axes on a live figure is fragile in
# matplotlib and eventually raises a KeyError from deep inside figure.delaxes().
im = ax.imshow(np.zeros((N_C, N_D)), cmap=CMAP, aspect="equal", origin="upper", vmin=0, vmax=1)
ax.set_xticks(range(N_D))
ax.set_xticklabels([f"D ch{ch}" for ch in D_CHANNELS], rotation=45, ha="right")
ax.set_yticks(range(N_C))
ax.set_yticklabels([f"C ch{ch}" for ch in C_CHANNELS])
ax.set_xlabel("Barres D")
ax.set_ylabel("Barres C")
cbar = fig.colorbar(im, ax=ax, label="Comptage (barre C + barre D)")
texts = [[ax.text(j, i, "0", ha="center", va="center", color="black", fontsize=9)
          for j in range(N_D)] for i in range(N_C)]
fig.tight_layout()


def update(_frame):
    with lock:
        counts = dict(counts_by_channel)
    elapsed_s = max(time.monotonic() - t_start, 1e-6)

    matrix = np.zeros((N_C, N_D), dtype=np.int64)
    for i, cch in enumerate(C_CHANNELS):
        for j, dch in enumerate(D_CHANNELS):
            matrix[i, j] = counts[cch] + counts[dch]

    total_hits = sum(counts.values())
    rate_hz = total_hits / elapsed_s
    vmax = max(int(matrix.max()), 1)

    im.set_data(matrix)
    im.set_clim(0, vmax)
    cbar.update_normal(im)

    for i in range(N_C):
        for j in range(N_D):
            value = int(matrix[i, j])
            texts[i][j].set_text(str(value))
            texts[i][j].set_color("white" if value > 0.5 * vmax else "black")

    status = "stopped" if stdin_closed.is_set() else "live"
    ax.set_title(f"SiPM matrix ({status}) -- {elapsed_s:.1f}s -- "
                 f"total={total_hits} -- {rate_hz:.1f} hits/s", fontsize=10)


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
