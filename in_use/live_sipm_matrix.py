#!/usr/bin/env python3
"""
live_sipm_matrix.py

Carte de couverture faisceau EN DIRECT sur une grille de barres SiPM
(voir toodo.txt, Todo1), a partir du flux CSV "numero_trigger,channel,
delai_ns" produit par read_output_buffer_blt en mode stream (-s). Se
branche comme live_histogram_multi.py (meme flux d'entree), mais affiche
une matrice C x D au lieu d'un histogramme, avec en plus un panneau lateral
montrant le taux instantane (hits/s) de chaque channel individuel (L/R ou
T/B separement, pas somme par barre comme dans la matrice).

Chaque case affiche un DIFFERENTIEL, pas un cumul : le nombre de coups
recus sur la barre C (L+R) et sur la barre D (T+B) depuis le rafraichissement
PRECEDENT (une fenetre d'integration = l'intervalle entre deux frames,
SIPM_INTERVAL_MS). Comme ca, la carte montre l'activite ACTUELLE (ou est le
faisceau maintenant), pas une somme qui grossit indefiniment depuis le debut
de l'acquisition et finit par etre dominee par l'historique.

Par defaut, utilise le mapping FIXE channel V1290A -> barre defini dans
sipm_channel_map.py (cablage MCFD modules #6 et #7) : 9 barres C (C5-C13,
L+R) en lignes, 7 barres D (D4-D10, T+B) en colonnes -- voir ce fichier si
le cablage change. SIPM_C_CHANNELS/SIPM_D_CHANNELS permet de court-circuiter
ce mapping avec des channels bruts (un par ligne/colonne).

Usage :
  ./read_output_buffer_blt 64324 0x03E00000 4096 -c -s -2000 \
      | python3 live_sipm_matrix.py

  ./read_output_buffer_blt 64324 0x03E00000 4096 -c -s -2000 \
      | SIPM_C_BARS=C5,C6,C7 SIPM_D_BARS=D4,D5 python3 live_sipm_matrix.py

  ./read_output_buffer_blt 64324 0x03E00000 4096 -c -s -2000 \
      | SIPM_C_CHANNELS=0,1,2,3 SIPM_D_CHANNELS=8,9,10,11 python3 live_sipm_matrix.py

Variables d'environnement :
  SIPM_C_BARS       sous-ensemble de barres C, ex: C5,C6,C7 (defaut: toutes, C5-C13)
  SIPM_D_BARS       sous-ensemble de barres D, ex: D4,D5 (defaut: toutes, D4-D10)
  SIPM_C_CHANNELS   mode manuel : channels TDC bruts pour les lignes (ignore le mapping)
  SIPM_D_CHANNELS   mode manuel : channels TDC bruts pour les colonnes (ignore le mapping)
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

import sipm_channel_map as chmap


def _parse_list(env_name):
    raw = os.environ.get(env_name, "")
    return [c.strip() for c in raw.split(",") if c.strip() != ""]


def _build_axes():
    """Retourne (row_labels, row_channel_groups, col_labels, col_channel_groups),
    en respectant SIPM_C_CHANNELS/SIPM_D_CHANNELS (mode manuel, prioritaire) ou
    SIPM_C_BARS/SIPM_D_BARS (sous-ensemble du mapping fixe), sinon le mapping
    fixe complet."""
    c_chan_raw = _parse_list("SIPM_C_CHANNELS")
    d_chan_raw = _parse_list("SIPM_D_CHANNELS")
    if c_chan_raw or d_chan_raw:
        if not (c_chan_raw and d_chan_raw):
            sys.exit("ERREUR : SIPM_C_CHANNELS et SIPM_D_CHANNELS doivent etre donnes ensemble.")
        try:
            c_channels = [int(c) for c in c_chan_raw]
            d_channels = [int(c) for c in d_chan_raw]
        except ValueError:
            sys.exit("ERREUR : SIPM_C_CHANNELS/SIPM_D_CHANNELS doivent etre des entiers separes par des virgules.")
        row_labels = [f"C ch{c}" for c in c_channels]
        col_labels = [f"D ch{d}" for d in d_channels]
        return row_labels, [[c] for c in c_channels], col_labels, [[d] for d in d_channels]

    c_bars_raw = _parse_list("SIPM_C_BARS")
    d_bars_raw = _parse_list("SIPM_D_BARS")
    c_bars = [b.upper() for b in c_bars_raw] if c_bars_raw else chmap.C_BAR_ORDER
    d_bars = [b.upper() for b in d_bars_raw] if d_bars_raw else chmap.D_BAR_ORDER
    unknown_c = [b for b in c_bars if b not in chmap.C_BAR_ORDER]
    unknown_d = [b for b in d_bars if b not in chmap.D_BAR_ORDER]
    if unknown_c or unknown_d:
        sys.exit(f"ERREUR : barre(s) inconnue(s) {unknown_c + unknown_d}, "
                  f"attendu parmi C: {chmap.C_BAR_ORDER} / D: {chmap.D_BAR_ORDER}")
    row_labels = [f"{b} (ch{','.join(map(str, chmap.C_BAR_CHANNELS[b]))})" for b in c_bars]
    col_labels = [f"{b} (ch{','.join(map(str, chmap.D_BAR_CHANNELS[b]))})" for b in d_bars]
    return row_labels, [chmap.C_BAR_CHANNELS[b] for b in c_bars], col_labels, [chmap.D_BAR_CHANNELS[b] for b in d_bars]


ROW_LABELS, ROW_GROUPS, COL_LABELS, COL_GROUPS = _build_axes()
INTERVAL_MS = int(os.environ.get("SIPM_INTERVAL_MS", "500"))
CMAP = os.environ.get("SIPM_CMAP", "Blues")

_row_channels = {ch for grp in ROW_GROUPS for ch in grp}
_col_channels = {ch for grp in COL_GROUPS for ch in grp}
overlap = _row_channels & _col_channels
if overlap:
    print(f"NOTE : channel(s) {sorted(overlap)} present(s) a la fois cote lignes et colonnes "
          f"-- leur(s) case(s) affichera(ont) un comptage double.", file=sys.stderr)

lock = threading.Lock()
stdin_closed = threading.Event()
t_start = time.monotonic()
watched = _row_channels | _col_channels
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

N_ROWS = len(ROW_GROUPS)
N_COLS = len(COL_GROUPS)


def _channel_label(ch):
    """Etiquette lisible pour un channel : 'C5-L'/'D9-T' via le mapping fixe
    (sipm_channel_map.CHANNEL_TO_BAR) si connu, sinon juste 'ch<N>' (mode
    manuel / channel hors mapping)."""
    entry = chmap.CHANNEL_TO_BAR.get(ch)
    return f"{entry[1]}-{entry[2]}" if entry else f"ch{ch}"


# Ordre d'affichage du panneau par-channel : toutes les barres C (dans l'ordre
# des lignes de la matrice), puis toutes les barres D (ordre des colonnes),
# sans doublon (un channel utilise a la fois en C et en D -- overlap manuel --
# n'apparait qu'une fois, son taux est le meme quel que soit le contexte).
_CHANNEL_ORDER = []
_seen_ch = set()
for _grp in ROW_GROUPS + COL_GROUPS:
    for _ch in _grp:
        if _ch not in _seen_ch:
            _seen_ch.add(_ch)
            _CHANNEL_ORDER.append(_ch)
_CHANNEL_LABELS = [_channel_label(ch) for ch in _CHANNEL_ORDER]
_CHANNEL_COLORS = ["#4c72b0" if ch in _row_channels else "#c44e52" for ch in _CHANNEL_ORDER]
N_CHANNELS = len(_CHANNEL_ORDER)

fig_w = max(8.0, 1.1 * N_COLS + 6.5)
fig_h = max(4.0, 1.1 * N_ROWS + 2.0, 0.28 * N_CHANNELS + 1.5)
fig, (ax, ax2) = plt.subplots(1, 2, figsize=(fig_w, fig_h),
                               gridspec_kw={"width_ratios": [max(N_COLS, 2), 2.2]})

# Build all artists (image, ticks, colorbar, per-cell text, per-channel bars) ONCE.
# Animating by mutating them in place (im.set_data/set_clim, cbar.update_normal,
# text.set_text, bar.set_width) instead of ax.clear() + re-creating everything
# each frame -- repeatedly removing/recreating a colorbar's axes on a live figure
# is fragile in matplotlib and eventually raises a KeyError from deep inside
# figure.delaxes().
im = ax.imshow(np.zeros((N_ROWS, N_COLS)), cmap=CMAP, aspect="equal", origin="upper", vmin=0, vmax=1)
ax.set_xticks(range(N_COLS))
ax.set_xticklabels(COL_LABELS, rotation=45, ha="right")
ax.set_yticks(range(N_ROWS))
ax.set_yticklabels(ROW_LABELS)
ax.set_xlabel("Barres D")
ax.set_ylabel("Barres C")
cbar = fig.colorbar(im, ax=ax, label="Comptage (barre C + barre D) depuis la frame precedente")
texts = [[ax.text(j, i, "0", ha="center", va="center", color="black", fontsize=9)
          for j in range(N_COLS)] for i in range(N_ROWS)]

# Panneau de droite : taux instantane (hits/s) par channel individuel (une barre
# horizontale par channel -- L/R ou T/B affiches separement, pas sommes comme
# dans la matrice) -- bleu = channel cote C, rouge = channel cote D.
bars = ax2.barh(range(N_CHANNELS), [0.0] * N_CHANNELS, color=_CHANNEL_COLORS)
ax2.set_yticks(range(N_CHANNELS))
ax2.set_yticklabels(_CHANNEL_LABELS, fontsize=7)
ax2.invert_yaxis()
ax2.set_xlabel("hits/s (instantane)")
ax2.set_title("Taux par channel", fontsize=10)
ax2.set_xlim(0, 1)

fig.tight_layout()

_prev_counts = dict(counts_by_channel)
_prev_time = t_start


def update(_frame):
    global _prev_counts, _prev_time
    with lock:
        counts = dict(counts_by_channel)
    now = time.monotonic()
    elapsed_s = max(now - t_start, 1e-6)
    window_s = max(now - _prev_time, 1e-6)  # real time since the last frame (integration window)

    delta = {ch: counts[ch] - _prev_counts.get(ch, 0) for ch in counts}
    _prev_counts, _prev_time = counts, now

    row_totals = [sum(delta[ch] for ch in grp) for grp in ROW_GROUPS]
    col_totals = [sum(delta[ch] for ch in grp) for grp in COL_GROUPS]
    matrix = np.array([[r + c for c in col_totals] for r in row_totals], dtype=np.int64)

    total_hits = sum(counts.values())      # cumulatif, pour info seulement
    window_hits = sum(delta.values())      # coups dans cette fenetre -> ce que montre la matrice
    rate_hz = window_hits / window_s
    vmax = max(int(matrix.max()), 1)

    im.set_data(matrix)
    im.set_clim(0, vmax)
    cbar.update_normal(im)

    for i in range(N_ROWS):
        for j in range(N_COLS):
            value = int(matrix[i, j])
            texts[i][j].set_text(str(value))
            texts[i][j].set_color("white" if value > 0.5 * vmax else "black")

    chan_rates = [delta[ch] / window_s for ch in _CHANNEL_ORDER]
    for bar, rate in zip(bars, chan_rates):
        bar.set_width(rate)
    ax2.set_xlim(0, max(chan_rates, default=0) * 1.15 or 1)

    status = "stopped" if stdin_closed.is_set() else "live"
    ax.set_title(f"SiPM matrix ({status}) -- {elapsed_s:.1f}s ecoules -- "
                 f"fenetre={window_s * 1000:.0f}ms -- {rate_hz:.1f} hits/s -- "
                 f"cumul total={total_hits}", fontsize=10)


ani = FuncAnimation(fig, update, interval=INTERVAL_MS, cache_frame_data=False)
plt.show()
