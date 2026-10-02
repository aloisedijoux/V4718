#!/usr/bin/env python3
"""
live_histogram_multi.py

Histogramme en temps reel, un jeu de couleur par channel, a partir du
flux CSV "numero_trigger,channel,delai_ns" produit par
read_output_buffer_blt en mode stream (-s). Sert a comparer plusieurs
channels actifs en meme temps (ex: montage a deux scintillateurs sur
channel 13 et channel 4) : superposition des distributions de delai
+ frequence (hits/s) de chaque channel affichee dans la legende.

Le comptage est un cumul BRUT depuis le lancement du script : chaque hit
recu incremente son bin, rien n'est jamais moyenne, lisse ou retire tant
qu'on ne demande pas explicitement un reset (touche 'r' dans la fenetre du
graphe, remet tous les comptages/N/taux a zero pour repartir sans
relancer le script).

Les comptages par bin sont accumules directement dans le thread lecteur
(bins fixes, calcules une seule fois au demarrage) -- chaque frame ne fait
que mettre a jour les courbes deja crees avec les comptages courants, au
lieu de refaire ax.clear() + ax.hist() sur l'historique complet a chaque
fois (l'ancienne version recalculait tout depuis le debut, jusqu'a 200000
points/channel, a chaque rafraichissement -- symptome rapporte :
"l'histogramme s'actualise mal" meme avec un seul channel actif).

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -31350 \
      | python3 live_histogram_multi.py

Variables d'environnement :
  HISTM_BINS         nombre de bins (defaut 60)
  HISTM_RANGE_NS      plage fixe de l'histogramme "MIN,MAX" en ns (defaut
                      "-2500,2500") -- a adapter a la fenetre de trigger
                      reellement utilisee (offset .. offset+largeur) ; les
                      hits hors plage sont comptes dans le total/taux mais
                      pas affiches dans l'histogramme (indique dans le
                      titre si ca arrive).
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
counts_by_channel = {}      # channel -> np.array(BINS,) cumul brut depuis le debut/dernier reset
n_by_channel = {}           # channel -> total recu (y compris hors plage) depuis le debut/dernier reset
n_in_range_by_channel = {}  # channel -> total dans la plage affichee (= somme des bins)


def _ensure_channel(channel):
    if channel not in counts_by_channel:
        counts_by_channel[channel] = np.zeros(BINS, dtype=np.int64)
        n_by_channel[channel] = 0
        n_in_range_by_channel[channel] = 0


def reset_all():
    with lock:
        for ch in counts_by_channel:
            counts_by_channel[ch][:] = 0
            n_by_channel[ch] = 0
            n_in_range_by_channel[ch] = 0
        _prev_n_by_channel.clear()


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
        with lock:
            _ensure_channel(channel)
            if RANGE_MIN <= delay < RANGE_MAX:
                bin_idx = int((delay - RANGE_MIN) / (RANGE_MAX - RANGE_MIN) * BINS)
                bin_idx = min(bin_idx, BINS - 1)
                counts_by_channel[channel][bin_idx] += 1
                n_in_range_by_channel[channel] += 1
            n_by_channel[channel] += 1
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
fig.canvas.mpl_connect("key_press_event", lambda event: reset_all() if event.key == "r" else None)
fig.tight_layout()

_prev_n_by_channel = {}  # channel -> n_by_channel au frame precedent (pour le taux instantane)
_prev_time = 0.0


def update(_frame):
    global legend, _prev_time
    with lock:
        channels = sorted(counts_by_channel.keys())
        snapshot = {ch: counts_by_channel[ch].copy() for ch in channels}
        n_total_by_ch = {ch: n_by_channel[ch] for ch in channels}
        n_shown_by_ch = {ch: n_in_range_by_channel[ch] for ch in channels}

    now = time.monotonic() - t_start
    window_s = max(now - _prev_time, 1e-6)  # temps reel depuis la frame precedente

    ymax = 1
    for idx, ch in enumerate(channels):
        counts = snapshot[ch]
        ymax = max(ymax, int(counts.max()))
        # Taux INSTANTANE (coups recus depuis la derniere frame / temps ecoule depuis
        # la derniere frame) -- PAS une moyenne depuis le tout premier hit du channel,
        # qui peut rester tiree vers un ancien taux plus eleve/plus bas et ne plus du
        # tout refleter ce qui se passe maintenant.
        delta_n = n_total_by_ch[ch] - _prev_n_by_channel.get(ch, 0)
        rate = delta_n / window_s
        out_of_range = n_total_by_ch[ch] - n_shown_by_ch[ch]
        extra = f", {out_of_range} hors-plage" if out_of_range else ""
        label = f"channel {ch} (N={n_total_by_ch[ch]}{extra}, {rate:.0f} hits/s)"

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
    total_shown = sum(n_shown_by_ch.values())
    out_note = f" -- {total_n - total_shown} hors de la plage [{RANGE_MIN:.0f},{RANGE_MAX:.0f}]ns, pas affiches" \
        if total_n > total_shown else ""
    title.set_text(f"Histogramme multi-canal -- N total={total_n} ({status}){out_note}")
    if channels:
        legend = ax.legend(loc="upper right", fontsize=9)

    _prev_n_by_channel.update(n_total_by_ch)
    _prev_time = now


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
