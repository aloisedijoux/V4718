#!/usr/bin/env python3
"""
live_histogram_multi.py

Histogramme en temps reel, un jeu de couleur par channel, a partir du
flux CSV "numero_trigger,channel,delai_ns" produit par
read_output_buffer_blt en mode stream (-s). Sert a comparer plusieurs
channels actifs en meme temps (ex: montage a deux scintillateurs sur
channel 13 et channel 4) : superposition des distributions de delai
+ frequence (hits/s) de chaque channel affichee dans la legende.

Trois modes d'affichage (HISTM_MODE), AUCUN des trois ne lisse/moyenne les
valeurs des bins entre eux -- chaque bin reste un comptage brut, ce qui
change c'est juste QUELS hits sont comptes dans ce comptage :

  cumulative (defaut)  Comptage cumule depuis le lancement du script (ou
                        depuis le dernier reset manuel, touche 'r'). Peut
                        finir par melanger des periodes tres differentes
                        sur une longue acquisition.
  frame                Ne montre que les hits recus depuis le CE frame-ci
                        (depuis le dernier rafraichissement) -- tres
                        reactif, mais peut etre tres eparse si le taux est
                        bas (peu de hits entre deux refresh).
  window                Fenetre glissante des HISTM_WINDOW_S dernieres
                        secondes (defaut 10s) -- les hits plus vieux sont
                        retires du comptage au fur et a mesure. Compromis
                        entre les deux : montre l'etat recent sans etre
                        trop eparse.

Touche 'r' dans la fenetre du graphe : remet tous les comptages (et N
total/taux) a zero, quel que soit le mode -- utile pour repartir de zero
sans relancer le script.

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

  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -31350 \
      | HISTM_MODE=window HISTM_WINDOW_S=5 python3 live_histogram_multi.py

Variables d'environnement :
  HISTM_BINS         nombre de bins (defaut 60)
  HISTM_RANGE_NS      plage fixe de l'histogramme "MIN,MAX" en ns (defaut
                      "-2500,2500") -- a adapter a la fenetre de trigger
                      reellement utilisee (offset .. offset+largeur) ; les
                      hits hors plage sont comptes dans le total/taux mais
                      pas affiches dans l'histogramme.
  HISTM_INTERVAL_MS  periode de rafraichissement en ms (defaut 500)
  HISTM_MODE         "cumulative" (defaut) / "frame" / "window"
  HISTM_WINDOW_S     largeur de la fenetre glissante en s (defaut 10),
                      utilise seulement si HISTM_MODE=window
"""

import os
import sys
import threading
import time
from collections import deque

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

BINS = int(os.environ.get("HISTM_BINS", "60"))
INTERVAL_MS = int(os.environ.get("HISTM_INTERVAL_MS", "500"))
MODE = os.environ.get("HISTM_MODE", "cumulative")
WINDOW_S = float(os.environ.get("HISTM_WINDOW_S", "10"))
if MODE not in ("cumulative", "frame", "window"):
    sys.exit(f"ERREUR : HISTM_MODE invalide ({MODE!r}), attendu cumulative/frame/window")

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
counts_by_channel = {}      # channel -> np.array(BINS,) comptage affiche (semantique selon MODE)
n_by_channel = {}           # channel -> total recu depuis le dernier reset (y compris hors plage)
n_in_range_by_channel = {}  # channel -> total dans la plage affichee depuis le dernier reset
window_by_channel = {}      # channel -> deque[(temps, bin_idx)], utilise seulement en mode "window"


def _ensure_channel(channel):
    if channel not in counts_by_channel:
        counts_by_channel[channel] = np.zeros(BINS, dtype=np.int64)
        n_by_channel[channel] = 0
        n_in_range_by_channel[channel] = 0
        if MODE == "window":
            window_by_channel[channel] = deque()


def reset_all():
    with lock:
        for ch in counts_by_channel:
            counts_by_channel[ch][:] = 0
            n_by_channel[ch] = 0
            n_in_range_by_channel[ch] = 0
            if MODE == "window":
                window_by_channel[ch].clear()
        _prev_n_by_channel.clear()
        _prev_counts_by_channel.clear()


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
                if MODE == "window":
                    window_by_channel[channel].append((time.monotonic() - t_start, bin_idx))
            n_by_channel[channel] += 1
    stdin_closed.set()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

fig, ax = plt.subplots(figsize=(9, 5.5))
ax.set_xlim(RANGE_MIN, RANGE_MAX)
ax.set_xlabel("Délai par rapport au trigger (ns)")
ax.set_ylabel("Coups")
mode_label = {"cumulative": "cumul depuis le debut", "frame": "ce rafraichissement seulement",
              "window": f"fenetre glissante {WINDOW_S:.0f}s"}[MODE]
title = ax.set_title(f"Histogramme multi-canal [{mode_label}] -- N total=0 (en direct)")
legend = None
lines_by_channel = {}  # channel -> Line2D (step plot), created lazily
fig.canvas.mpl_connect("key_press_event", lambda event: reset_all() if event.key == "r" else None)
fig.tight_layout()

_prev_n_by_channel = {}       # channel -> n_by_channel au frame precedent (pour le taux instantane)
_prev_counts_by_channel = {}  # channel -> comptage au frame precedent (pour le mode "frame")
_prev_time = 0.0


def update(_frame):
    global legend, _prev_time
    now = time.monotonic() - t_start

    with lock:
        channels = sorted(counts_by_channel.keys())

        if MODE == "window":
            # Purge les entrees sorties de la fenetre glissante avant de lire les comptages.
            cutoff = now - WINDOW_S
            for ch in channels:
                dq = window_by_channel[ch]
                while dq and dq[0][0] < cutoff:
                    _, old_bin = dq.popleft()
                    counts_by_channel[ch][old_bin] -= 1

        snapshot = {ch: counts_by_channel[ch].copy() for ch in channels}
        n_total_by_ch = {ch: n_by_channel[ch] for ch in channels}
        n_shown_by_ch = {ch: n_in_range_by_channel[ch] for ch in channels}

    window_s = max(now - _prev_time, 1e-6)  # temps reel depuis la frame precedente

    ymax = 1
    for idx, ch in enumerate(channels):
        if MODE == "frame":
            prev = _prev_counts_by_channel.get(ch)
            counts = snapshot[ch] if prev is None else snapshot[ch] - prev
            _prev_counts_by_channel[ch] = snapshot[ch]
        else:
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
    title.set_text(f"Histogramme multi-canal [{mode_label}] -- N total={total_n} ({status}){out_note}")
    if channels:
        legend = ax.legend(loc="upper right", fontsize=9)

    _prev_n_by_channel.update(n_total_by_ch)
    _prev_time = now


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
