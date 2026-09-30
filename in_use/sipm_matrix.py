#!/usr/bin/env python3
"""
sipm_matrix.py

Carte de couverture faisceau sur une grille de barres scintillantes SiPM
(voir toodo.txt, Todo1), a partir d'un enregistrement binaire brut produit
par read_output_buffer_blt (option -b fichier.bin).

Principe : une moitie des channels TDC actifs lit une extremite des barres
"C" (une ligne de la matrice), l'autre moitie lit une extremite des barres
"D" (une colonne). On lance une acquisition de 2-3s sur les channels
choisis, on compte les hits recus sur chaque channel pendant cette fenetre,
puis pour chaque case (C_i, D_j) de la matrice on affiche la somme des
comptes de la barre C_i et de la barre D_j -- une carte de chaleur de la
zone eclairee par le faisceau (bleu = beaucoup de coups, par defaut).

Le mapping channel -> barre depend entierement du cablage physique : il
n'est PAS hardcode ici, --c-channels/--d-channels donnent la liste des
channels TDC dans l'ordre ou les barres doivent apparaitre sur la grille.

Usage :
  python3 sipm_matrix.py run.bin --c-channels 0,1,2,3 --d-channels 8,9,10,11 \
      --duration-s 2.5 [--save fig.png] [--cmap Blues]

  --c-channels LISTE   channels TDC des barres C (lignes), dans l'ordre
                        d'affichage souhaite, ex: 0,1,2,3
  --d-channels LISTE   channels TDC des barres D (colonnes), meme logique
  --duration-s N       duree reelle de l'acquisition en secondes (pour le
                        taux de comptage global, hits/s)
  --save FICHIER       enregistre la figure au lieu de l'afficher a l'ecran
  --cmap NOM           colormap matplotlib (defaut: Blues)
"""

import argparse
import sys

import numpy as np
import matplotlib.pyplot as plt

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
    try:
        chans = [int(c.strip()) for c in s.split(",") if c.strip() != ""]
    except ValueError:
        sys.exit(f"ERREUR : --{label} doit etre une liste de channels separes par des virgules, ex: 0,1,2,3")
    if not chans:
        sys.exit(f"ERREUR : --{label} est vide.")
    return chans


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin_path", help="fichier binaire brut (-b) produit par read_output_buffer_blt")
    ap.add_argument("--c-channels", required=True, dest="c_channels",
                     help="channels TDC des barres C (lignes), ex: 0,1,2,3")
    ap.add_argument("--d-channels", required=True, dest="d_channels",
                     help="channels TDC des barres D (colonnes), ex: 8,9,10,11")
    ap.add_argument("--duration-s", type=float, required=True,
                     help="duree reelle de l'acquisition en secondes (pour le taux de comptage)")
    ap.add_argument("--save", default=None, help="enregistre la figure dans ce fichier au lieu de l'afficher")
    ap.add_argument("--cmap", default="Blues", help="colormap matplotlib (defaut: Blues)")
    args = ap.parse_args()

    c_channels = parse_channel_list(args.c_channels, "c-channels")
    d_channels = parse_channel_list(args.d_channels, "d-channels")

    overlap = set(c_channels) & set(d_channels)
    if overlap:
        sys.exit(f"ERREUR : channel(s) {sorted(overlap)} present(s) a la fois dans "
                  f"--c-channels et --d-channels -- chaque extremite lue doit etre C ou D, pas les deux.")

    print(f"Lecture de {args.bin_path} ...")
    counts = load_channel_counts(args.bin_path)

    observed = sorted(set(c_channels) | set(d_channels))
    print("\nComptage par channel (fenetre d'acquisition complete) :")
    for ch in observed:
        n = counts.get(ch, 0)
        flag = "  <-- AUCUN HIT" if n == 0 else ""
        print(f"  channel {ch:2d} : {n:8d} hits{flag}")

    missing = [ch for ch in observed if counts.get(ch, 0) == 0]
    if missing:
        print(f"\nATTENTION : aucun hit sur le(s) channel(s) {missing} -- verifier qu'ils "
              f"etaient bien actives (Apply configuration) pendant cette acquisition.", file=sys.stderr)

    matrix = np.zeros((len(c_channels), len(d_channels)), dtype=np.int64)
    for i, cch in enumerate(c_channels):
        for j, dch in enumerate(d_channels):
            matrix[i, j] = counts.get(cch, 0) + counts.get(dch, 0)

    total_hits = sum(counts.get(ch, 0) for ch in observed)
    rate_hz = total_hits / args.duration_s if args.duration_s > 0 else float("nan")
    print(f"\nTotal hits (channels observes)  : {total_hits}")
    print(f"Duree acquisition                : {args.duration_s:.3f} s")
    print(f"Taux de comptage global          : {rate_hz:.1f} hits/s")

    fig_w = max(4.0, 1.1 * len(d_channels) + 2.0)
    fig_h = max(4.0, 1.1 * len(c_channels) + 2.0)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))
    im = ax.imshow(matrix, cmap=args.cmap, aspect="equal", origin="upper")

    ax.set_xticks(range(len(d_channels)))
    ax.set_xticklabels([f"D ch{ch}" for ch in d_channels], rotation=45, ha="right")
    ax.set_yticks(range(len(c_channels)))
    ax.set_yticklabels([f"C ch{ch}" for ch in c_channels])
    ax.set_xlabel("Barres D")
    ax.set_ylabel("Barres C")

    vmax = matrix.max() if matrix.size else 0
    for i in range(len(c_channels)):
        for j in range(len(d_channels)):
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
