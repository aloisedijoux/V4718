#!/usr/bin/env python3
"""
analyze_run.py

Analyse rapide (vectorisee numpy) d'un enregistrement binaire brut
produit par read_output_buffer_blt (-b fichier.bin) : decode tous les
mots BLT32, reconstruit pour chaque TDC MEASUREMENT le numero de
trigger (event_count du dernier GLOBAL HEADER), le channel et le
delai. Identifie ensuite les vraies coincidences (meme trigger, deux
channels differents matches) et trace la distribution du delai relatif
entre les deux channels (TOF/decalage) pour ces coincidences.

Usage :
  python3 analyze_run.py <fichier.bin> [--offset-ns N] [--ch-a 4] [--ch-b 13]
"""

import argparse
import sys
import numpy as np
import matplotlib.pyplot as plt

ASSUMED_RESOLUTION_PS = 25

ap = argparse.ArgumentParser()
ap.add_argument("bin_path")
ap.add_argument("--offset-ns", type=float, default=-2000.0)
ap.add_argument("--ch-a", type=int, default=4, help="channel de reference (ex: generateur)")
ap.add_argument("--ch-b", type=int, default=13, help="channel d'interet (ex: scintillateur)")
args = ap.parse_args()

print(f"Lecture de {args.bin_path} ...")
words = np.fromfile(args.bin_path, dtype="<u4")
n = len(words)
print(f"{n} mots lus ({n * 4 / 1e6:.1f} Mo)")

word_type = (words >> 27) & 0x1F

TYPE_GLOBAL_HEADER = 0x08
TYPE_TDC_MEASUREMENT = 0x00

is_header = word_type == TYPE_GLOBAL_HEADER
is_meas = word_type == TYPE_TDC_MEASUREMENT

# --- reconstruction du numero de trigger courant pour chaque mot (equivalent
# de g_current_event du C : la valeur du dernier GLOBAL HEADER vu) ---
header_positions = np.nonzero(is_header)[0]
header_events = (words[header_positions] >> 5) & 0x3FFFFF
print(f"{len(header_positions)} GLOBAL HEADER, {is_meas.sum()} TDC MEASUREMENT")

if len(header_positions) == 0:
    print("Aucun GLOBAL HEADER trouve, abandon.", file=sys.stderr)
    sys.exit(1)

# pour chaque position, index du dernier header <= position
idx_all = np.arange(n)
last_header_idx = np.searchsorted(header_positions, idx_all, side="right") - 1
last_header_idx = np.clip(last_header_idx, 0, len(header_positions) - 1)
current_event_per_word = header_events[last_header_idx]

# --- extraction des mesures ---
meas_positions = np.nonzero(is_meas)[0]
meas_words = words[meas_positions]
meas_channel = (meas_words >> 21) & 0x1F
meas_raw = meas_words & 0x1FFFFF
meas_delay_ns = args.offset_ns + meas_raw.astype(np.float64) * ASSUMED_RESOLUTION_PS / 1000.0
meas_trigger = current_event_per_word[meas_positions]

print(f"\nRepartition par channel :")
channels, counts = np.unique(meas_channel, return_counts=True)
for ch, c in zip(channels, counts):
    print(f"  channel {ch:2d} : {c} hits")

# --- recherche des coincidences (meme trigger, channel A et channel B) ---
mask_a = meas_channel == args.ch_a
mask_b = meas_channel == args.ch_b

trig_a = meas_trigger[mask_a]
delay_a = meas_delay_ns[mask_a]
trig_b = meas_trigger[mask_b]
delay_b = meas_delay_ns[mask_b]

print(f"\nChannel {args.ch_a} (reference) : {len(trig_a)} hits")
print(f"Channel {args.ch_b} (interet)   : {len(trig_b)} hits")

# tri par trigger pour un merge efficace (il ne peut y avoir qu'un hit par
# channel par trigger, la fenetre de matching ne permet qu'un match)
order_a = np.argsort(trig_a)
trig_a_sorted = trig_a[order_a]
delay_a_sorted = delay_a[order_a]

# pour chaque hit du channel B, cherche si le meme trigger existe cote A
pos_in_a = np.searchsorted(trig_a_sorted, trig_b)
pos_in_a_clipped = np.clip(pos_in_a, 0, len(trig_a_sorted) - 1)
found = (pos_in_a < len(trig_a_sorted)) & (trig_a_sorted[pos_in_a_clipped] == trig_b)

n_coinc = found.sum()
print(f"\nCoincidences reelles (meme trigger, channel {args.ch_a} ET {args.ch_b}) : {n_coinc}")

if n_coinc > 0:
    coinc_trigger = trig_b[found]
    coinc_delay_b = delay_b[found]
    coinc_delay_a = delay_a_sorted[pos_in_a_clipped[found]]
    tof = coinc_delay_b - coinc_delay_a

    print(f"TOF (delai channel {args.ch_b} - channel {args.ch_a}) :")
    print(f"  moyenne = {tof.mean():.3f} ns")
    print(f"  ecart-type = {tof.std():.3f} ns")
    print(f"  min/max = {tof.min():.3f} / {tof.max():.3f} ns")

    fig, axes = plt.subplots(1, 3, figsize=(16, 5))

    axes[0].hist(delay_a, bins=80, color="#4c72b0", alpha=0.8, label=f"channel {args.ch_a} (N={len(delay_a)})")
    axes[0].set_xlabel("Délai vs trigger (ns)")
    axes[0].set_ylabel("Coups")
    axes[0].set_title(f"Channel {args.ch_a} (référence)")
    axes[0].legend(fontsize=8)

    axes[1].hist(delay_b, bins=80, color="#c44e52", alpha=0.8, label=f"channel {args.ch_b} (N={len(delay_b)})")
    axes[1].set_xlabel("Délai vs trigger (ns)")
    axes[1].set_title(f"Channel {args.ch_b} (scintillateur)")
    axes[1].legend(fontsize=8)

    axes[2].hist(tof, bins=40, color="#55a868", alpha=0.85)
    axes[2].axvline(tof.mean(), color="#1a1a1a", linestyle="--", linewidth=1,
                     label=f"moyenne={tof.mean():.2f} ns")
    axes[2].set_xlabel(f"TOF = délai(ch{args.ch_b}) - délai(ch{args.ch_a}) (ns)")
    axes[2].set_title(f"Coïncidences réelles (N={n_coinc})")
    axes[2].legend(fontsize=8)

    fig.tight_layout()
    out_path = args.bin_path.rsplit(".", 1)[0] + "_analysis.png"
    fig.savefig(out_path, dpi=150)
    print(f"\nFigure sauvegardée : {out_path}")
else:
    print("Aucune coïncidence trouvée -- rien à tracer pour le TOF.")
