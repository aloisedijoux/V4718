#!/usr/bin/env python3
"""
sipm_matrix.py

Carte de couverture faisceau sur une grille de barres scintillantes SiPM
(voir toodo.txt, Todo1), a partir d'un enregistrement binaire brut produit
par read_output_buffer_blt (option -b fichier.bin).

Par defaut, utilise le mapping FIXE channel V1290A -> barre defini dans
sipm_channel_map.py (cablage MCFD modules #6 et #7) : 9 barres C (C5-C13,
lues en L+R) en lignes, 7 barres D (D4-D10, lues en T+B) en colonnes.
Chaque case (C_i, D_j) de la matrice affiche la somme des comptes des 2
channels de la barre C_i (L+R) et des 2 channels de la barre D_j (T+B) --
une carte de chaleur de la zone eclairee par le faisceau (bleu = beaucoup
de coups, par defaut). Voir sipm_channel_map.py si le cablage change.

--c-channels/--d-channels permet de court-circuiter ce mapping avec une
liste de channels bruts (mode manuel, un channel par ligne/colonne) --
utile pour tester avant que le mapping ne soit a jour, ou un autre cablage.

Usage :
  python3 sipm_matrix.py run.bin --duration-s 2.5 [--save fig.png]
  python3 sipm_matrix.py run.bin --duration-s 2.5 --c-bars C5,C6,C7 --d-bars D4,D5
  python3 sipm_matrix.py run.bin --duration-s 2.5 --c-channels 0,1,2,3 --d-channels 8,9,10,11

  --c-bars LISTE       sous-ensemble de barres C a afficher (defaut: toutes, C5-C13)
  --d-bars LISTE       sous-ensemble de barres D a afficher (defaut: toutes, D4-D10)
  --c-channels LISTE   mode manuel : channels TDC bruts pour les lignes (ignore le mapping)
  --d-channels LISTE   mode manuel : channels TDC bruts pour les colonnes (ignore le mapping)
  --duration-s N       duree reelle de l'acquisition en secondes (pour le taux de comptage)
  --save FICHIER       enregistre la figure au lieu de l'afficher a l'ecran
  --cmap NOM           colormap matplotlib (defaut: Blues)
"""

import argparse
import sys

import numpy as np
import matplotlib.pyplot as plt

import sipm_channel_map as chmap
from channel_list import parse_channel_list as _parse_channel_ranges

TYPE_TDC_MEASUREMENT = 0x00


def load_channel_counts(bin_path):
    """Compte, pour chaque channel, le nombre de mots TDC MEASUREMENT dans
    le fichier binaire brut (meme decodage que analyze_run.py/decode_binary.py)."""
    words = np.fromfile(bin_path, dtype="<u4")
    if words.size == 0:
        sys.exit(f"ERREUR : {bin_path} est vide ou illisible.")
    word_type = (words >> 27) & 0x1F
    is_meas = word_type == TYPE_TDC_MEASUREMENT
    channels = (words[is_meas] >> 21) & 0x1F
    ids, counts = np.unique(channels, return_counts=True)
    return dict(zip(ids.tolist(), counts.tolist()))


def parse_channel_list(s, label):
    """Accepte les valeurs individuelles et les plages "LOW-HIGH", melangees :
    '0,1,2' ou '0-2' ou '0-3,8,10-12' -- voir channel_list.py."""
    try:
        chans = _parse_channel_ranges(s)
    except ValueError as e:
        sys.exit(f"ERREUR : --{label} invalide ({e}), ex: 0,1,2,3 ou 0-3,8,10-12")
    if not chans:
        sys.exit(f"ERREUR : --{label} est vide.")
    return chans


def parse_bar_list(s, valid_order, label):
    bars = [b.strip().upper() for b in s.split(",") if b.strip() != ""]
    unknown = [b for b in bars if b not in valid_order]
    if unknown:
        sys.exit(f"ERREUR : --{label} contient des barres inconnues {unknown}, "
                  f"attendu parmi {valid_order}")
    return bars


def build_axes(args):
    """Retourne (row_labels, row_channel_groups, col_labels, col_channel_groups).
    Chaque *_channel_groups[i] est la liste des channels TDC associes a cette
    ligne/colonne (2 en mode barre par defaut -- L+R ou T+B --, 1 en mode manuel)."""
    manual = bool(args.c_channels or args.d_channels)
    if manual:
        if not (args.c_channels and args.d_channels):
            sys.exit("ERREUR : --c-channels et --d-channels doivent etre donnes ensemble.")
        c_channels = parse_channel_list(args.c_channels, "c-channels")
        d_channels = parse_channel_list(args.d_channels, "d-channels")
        row_labels = [f"C ch{c}" for c in c_channels]
        col_labels = [f"D ch{d}" for d in d_channels]
        return row_labels, [[c] for c in c_channels], col_labels, [[d] for d in d_channels]

    c_bars = parse_bar_list(args.c_bars, chmap.C_BAR_ORDER, "c-bars") if args.c_bars else chmap.C_BAR_ORDER
    d_bars = parse_bar_list(args.d_bars, chmap.D_BAR_ORDER, "d-bars") if args.d_bars else chmap.D_BAR_ORDER
    row_labels = [f"{b} (ch{','.join(map(str, chmap.C_BAR_CHANNELS[b]))})" for b in c_bars]
    col_labels = [f"{b} (ch{','.join(map(str, chmap.D_BAR_CHANNELS[b]))})" for b in d_bars]
    return row_labels, [chmap.C_BAR_CHANNELS[b] for b in c_bars], col_labels, [chmap.D_BAR_CHANNELS[b] for b in d_bars]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_path", help="fichier binaire brut (-b) produit par read_output_buffer_blt")
    ap.add_argument("--c-bars", default=None, help="sous-ensemble de barres C, ex: C5,C6,C7 (defaut: toutes)")
    ap.add_argument("--d-bars", default=None, help="sous-ensemble de barres D, ex: D4,D5 (defaut: toutes)")
    ap.add_argument("--c-channels", default=None, dest="c_channels",
                     help="mode manuel : channels TDC bruts pour les lignes, ex: 0,1,2 ou 0-3,8,10-12 "
                          "(ignore le mapping C5-C13)")
    ap.add_argument("--d-channels", default=None, dest="d_channels",
                     help="mode manuel : channels TDC bruts pour les colonnes, ex: 8,9,10 ou 8-11 "
                          "(ignore le mapping D4-D10)")
    ap.add_argument("--duration-s", type=float, required=True,
                     help="duree reelle de l'acquisition en secondes (pour le taux de comptage)")
    ap.add_argument("--save", default=None, help="enregistre la figure dans ce fichier au lieu de l'afficher")
    ap.add_argument("--cmap", default="Blues", help="colormap matplotlib (defaut: Blues)")
    args = ap.parse_args()

    row_labels, row_groups, col_labels, col_groups = build_axes(args)

    row_channels = {ch for grp in row_groups for ch in grp}
    col_channels = {ch for grp in col_groups for ch in grp}
    overlap = row_channels & col_channels
    if overlap:
        print(f"NOTE : channel(s) {sorted(overlap)} present(s) a la fois cote lignes et "
              f"colonnes -- leur(s) case(s) affichera(ont) un comptage double.", file=sys.stderr)

    print(f"Lecture de {args.bin_path} ...")
    counts = load_channel_counts(args.bin_path)

    observed = sorted(row_channels | col_channels)
    print("\nComptage par channel (fenetre d'acquisition complete) :")
    for ch in observed:
        n = counts.get(ch, 0)
        flag = "  <-- AUCUN HIT" if n == 0 else ""
        print(f"  channel {ch:2d} : {n:8d} hits{flag}")

    missing = [ch for ch in observed if counts.get(ch, 0) == 0]
    if missing:
        print(f"\nATTENTION : aucun hit sur le(s) channel(s) {missing} -- verifier qu'ils "
              f"etaient bien actives (Apply configuration) pendant cette acquisition.", file=sys.stderr)

    matrix = np.zeros((len(row_groups), len(col_groups)), dtype=np.int64)
    for i, rchans in enumerate(row_groups):
        row_total = sum(counts.get(ch, 0) for ch in rchans)
        for j, cchans in enumerate(col_groups):
            matrix[i, j] = row_total + sum(counts.get(ch, 0) for ch in cchans)

    total_hits = sum(counts.get(ch, 0) for ch in observed)
    rate_hz = total_hits / args.duration_s if args.duration_s > 0 else float("nan")
    print(f"\nTotal hits (channels observes)  : {total_hits}")
    print(f"Duree acquisition                : {args.duration_s:.3f} s")
    print(f"Taux de comptage global          : {rate_hz:.1f} hits/s")

    fig_w = max(4.0, 1.1 * len(col_groups) + 2.5)
    fig_h = max(4.0, 1.1 * len(row_groups) + 2.0)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))
    im = ax.imshow(matrix, cmap=args.cmap, aspect="equal", origin="upper")

    ax.set_xticks(range(len(col_groups)))
    ax.set_xticklabels(col_labels, rotation=45, ha="right")
    ax.set_yticks(range(len(row_groups)))
    ax.set_yticklabels(row_labels)
    ax.set_xlabel("Barres D")
    ax.set_ylabel("Barres C")

    vmax = matrix.max() if matrix.size else 0
    for i in range(len(row_groups)):
        for j in range(len(col_groups)):
            color = "white" if vmax and matrix[i, j] > 0.5 * vmax else "black"
            ax.text(j, i, str(matrix[i, j]), ha="center", va="center", color=color, fontsize=9)

    fig.colorbar(im, ax=ax, label="Comptage (barre C + barre D)")
    ax.set_title(f"{args.bin_path}\nTaux global : {rate_hz:.1f} hits/s sur {args.duration_s:.2f} s")
    fig.tight_layout()

    if args.save:
        fig.savefig(args.save, dpi=150)
        print(f"\nFigure enregistree : {args.save}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
