#!/usr/bin/env python3
"""
live_histogram_multi.py

Histogramme en temps reel, un jeu de couleur par channel, a partir du
flux CSV "numero_trigger,channel,delai_ns" produit par
read_output_buffer_blt en mode stream (-s). Sert a comparer plusieurs
channels actifs en meme temps (ex: montage a deux scintillateurs sur
channel 13 et channel 4) : superposition des distributions de delai
+ frequence (hits/s) de chaque channel affichee dans la legende.

Usage :
  ./read_output_buffer_blt 64324 0x03000000 4096 -c -s -31350 \
      | python3 live_histogram_multi.py

Variables d'environnement :
  HISTM_BINS         nombre de bins (defaut 60)
  HISTM_INTERVAL_MS  periode de rafraichissement en ms (defaut 500)
  HISTM_MAXPOINTS     points gardes par channel (defaut 200000, 0=illimite)
"""

import os
import sys
import threading
import time

import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation

BINS = int(os.environ.get("HISTM_BINS", "60"))
INTERVAL_MS = int(os.environ.get("HISTM_INTERVAL_MS", "500"))
MAXPOINTS = int(os.environ.get("HISTM_MAXPOINTS", "200000"))

COLORS = ["#4c72b0", "#c44e52", "#55a868", "#8172b2", "#ccb974", "#64b5cd"]

lock = threading.Lock()
stdin_closed = threading.Event()
t_start = time.monotonic()
data_by_channel = {}     # channel -> list of delay_ns
first_seen = {}          # channel -> temps (s) du premier hit
last_seen = {}           # channel -> temps (s) du dernier hit
n_by_channel = {}        # channel -> total recu (avant troncature MAXPOINTS)


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
            if channel not in data_by_channel:
                data_by_channel[channel] = []
                first_seen[channel] = now
                n_by_channel[channel] = 0
            data_by_channel[channel].append(delay)
            n_by_channel[channel] += 1
            last_seen[channel] = now
            if MAXPOINTS > 0 and len(data_by_channel[channel]) > MAXPOINTS:
                del data_by_channel[channel][: len(data_by_channel[channel]) - MAXPOINTS]
    stdin_closed.set()


thread = threading.Thread(target=reader, daemon=True)
thread.start()

fig, ax = plt.subplots(figsize=(9, 5.5))


def update(_frame):
    with lock:
        channels = sorted(data_by_channel.keys())
        snapshot = {ch: list(data_by_channel[ch]) for ch in channels}
        n_total = {ch: n_by_channel[ch] for ch in channels}
        t0 = dict(first_seen)
        t1 = dict(last_seen)

    ax.clear()
    for idx, ch in enumerate(channels):
        values = snapshot[ch]
        if not values:
            continue
        color = COLORS[idx % len(COLORS)]
        span = max(t1[ch] - t0[ch], 1e-6)
        rate = n_total[ch] / span
        ax.hist(values, bins=BINS, histtype="step", linewidth=1.8, color=color,
                label=f"channel {ch} (N={n_total[ch]}, {rate:.0f} hits/s)")

    status = "stdin fermé" if stdin_closed.is_set() else "en direct"
    total_n = sum(n_total.values())
    ax.set_xlabel("Délai par rapport au trigger (ns)")
    ax.set_ylabel("Coups")
    ax.set_title(f"Histogramme multi-canal -- N total={total_n} ({status})")
    if channels:
        ax.legend(loc="upper right", fontsize=9)
    fig.tight_layout()


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
