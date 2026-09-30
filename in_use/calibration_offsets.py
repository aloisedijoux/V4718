#!/usr/bin/env python3
"""
calibration_offsets.py

Calibration des offsets par channel TDC (voir toodo.txt, Todo2).

Pour chaque channel, on trace l'histogramme du delai (delay_ns) mesure
pendant une acquisition a un momentum donne, on y repere la "ligne des
electrons" (toujours au meme TOF, independamment du momentum -- c'est ce
qui permet de l'identifier), et on en deduit l'offset du channel : la
position de cette ligne dans l'histogramme brut. Ce pic est detecte
automatiquement (bin le plus rempli d'un histogramme lisse), avec la
possibilite de corriger a la main un channel dont la detection s'est
trompee (ex: bruit, particule parasite plus intense que la ligne
d'electrons). Les offsets de tous les channels et de tous les momentums
testes sont accumules dans un seul fichier de calibration JSON, qui sert
ensuite a realigner les channels (sous-traction de l'offset propre a
chaque channel) au moment de construire l'histogramme co-added.

Sous-commandes :

  measure   Construit l'histogramme de chaque channel demande a partir
            d'un run, detecte (ou recoit en manuel) l'offset, l'enregistre
            dans le fichier de calibration, et sauvegarde une figure par
            channel (nom de fichier taggue avec le momentum).

  apply     Recharge le fichier de calibration pour un momentum donne,
            realigne chaque channel (delay_ns -= offset[channel]) et
            construit/sauvegarde l'histogramme co-added (tous les
            channels superposes/sommes, lignes d'electrons alignees).

Usage :
  python3 calibration_offsets.py measure run_p1500.bin --momentum 1500 \
      --channels 0,1,2,3 [--search-window-ns MIN MAX] \
      [--manual-offset 2=118.4] [--calib-file calibration.json]

  python3 calibration_offsets.py apply run_p1500.bin --momentum 1500 \
      --calib-file calibration.json [--channels 0,1,2,3] [--out coadded.png]

Entree acceptee (les deux sous-commandes) : un fichier binaire brut (-b
fichier.bin de read_output_buffer_blt) ou un CSV "trigger,channel,delai_ns"
(mode stream -s, ou sortie de decode_binary.py --csv-out).
"""

import argparse
import csv as csv_module
import json
import os
import sys
from datetime import datetime, timezone

import numpy as np
import matplotlib.pyplot as plt

ASSUMED_RESOLUTION_PS = 25
TYPE_TDC_MEASUREMENT = 0x00


# ---------------------------------------------------------------------- #
# Chargement des hits (channel, delay_ns) depuis .bin ou .csv
# ---------------------------------------------------------------------- #
def load_hits(path, offset_ns=0.0):
    if path.lower().endswith(".csv"):
        return _load_hits_csv(path)
    return _load_hits_bin(path, offset_ns)


def _load_hits_bin(path, offset_ns):
    words = np.fromfile(path, dtype="<u4")
    if words.size == 0:
        sys.exit(f"ERREUR : {path} est vide ou illisible.")
    word_type = (words >> 27) & 0x1F
    is_meas = word_type == TYPE_TDC_MEASUREMENT
    meas = words[is_meas]
    channel = (meas >> 21) & 0x1F
    raw = meas & 0x1FFFFF
    delay_ns = offset_ns + raw.astype(np.float64) * ASSUMED_RESOLUTION_PS / 1000.0
    return channel.astype(np.int64), delay_ns


def _load_hits_csv(path):
    channels, delays = [], []
    with open(path, encoding="utf-8") as f:
        for row in csv_module.reader(f):
            if len(row) < 3:
                continue
            try:
                channels.append(int(float(row[1])))
                delays.append(float(row[2]))
            except ValueError:
                continue
    if not channels:
        sys.exit(f"ERREUR : aucune ligne exploitable dans {path} (attendu : trigger,channel,delai_ns).")
    return np.array(channels, dtype=np.int64), np.array(delays, dtype=np.float64)


# ---------------------------------------------------------------------- #
# Detection automatique du pic (ligne des electrons)
# ---------------------------------------------------------------------- #
def find_peak(delay_ns, bins, search_range, smooth_bins):
    counts, edges = np.histogram(delay_ns, bins=bins, range=search_range)
    if smooth_bins > 1:
        kernel = np.ones(smooth_bins) / smooth_bins
        smoothed = np.convolve(counts, kernel, mode="same")
    else:
        smoothed = counts.astype(np.float64)
    centers = 0.5 * (edges[:-1] + edges[1:])
    if smoothed.sum() == 0:
        return None, counts, edges
    peak_idx = int(np.argmax(smoothed))
    return float(centers[peak_idx]), counts, edges


def parse_channel_list(s):
    try:
        return [int(c.strip()) for c in s.split(",") if c.strip() != ""]
    except ValueError:
        sys.exit("ERREUR : liste de channels invalide, attendu ex: 0,1,2,3")


def parse_manual_offsets(items):
    manual = {}
    for item in items or []:
        if "=" not in item:
            sys.exit(f"ERREUR : --manual-offset '{item}' invalide, attendu CHANNEL=VALEUR_NS")
        ch_s, val_s = item.split("=", 1)
        try:
            manual[int(ch_s.strip())] = float(val_s.strip())
        except ValueError:
            sys.exit(f"ERREUR : --manual-offset '{item}' invalide, attendu CHANNEL=VALEUR_NS")
    return manual


# ---------------------------------------------------------------------- #
# Fichier de calibration
# ---------------------------------------------------------------------- #
def load_calib(path):
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return {"momentums": {}}


def save_calib(path, calib):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(calib, f, indent=2, sort_keys=True)


# ---------------------------------------------------------------------- #
# measure
# ---------------------------------------------------------------------- #
def cmd_measure(args):
    channels_wanted = parse_channel_list(args.channels)
    manual = parse_manual_offsets(args.manual_offset)
    search_range = tuple(args.search_window_ns) if args.search_window_ns else None

    print(f"Lecture de {args.input} ...")
    all_channels, all_delays = load_hits(args.input, offset_ns=args.offset_ns)

    plot_dir = args.plot_dir or os.path.dirname(os.path.abspath(args.calib_file)) or "."
    os.makedirs(plot_dir, exist_ok=True)

    calib = load_calib(args.calib_file)
    momentum_key = str(args.momentum)
    calib.setdefault("momentums", {}).setdefault(momentum_key, {})

    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    print(f"\nMomentum : {momentum_key}")
    print(f"{'channel':>8} {'n_hits':>8} {'peak_ns':>10} {'source':>8}")

    for ch in channels_wanted:
        mask = all_channels == ch
        delays = all_delays[mask]
        n_hits = int(mask.sum())

        if ch in manual:
            peak_ns = manual[ch]
            source = "manual"
            if n_hits > 0:
                _, counts, edges = find_peak(delays, args.bins, search_range or (delays.min(), delays.max()), args.smooth_bins)
            else:
                counts = edges = None
        else:
            if n_hits == 0:
                print(f"{ch:8d} {n_hits:8d} {'--':>10} {'--':>8}   AUCUN HIT, ignore (utiliser --manual-offset pour forcer une valeur)")
                continue
            rng = search_range or (float(delays.min()), float(delays.max()))
            peak_ns, counts, edges = find_peak(delays, args.bins, rng, args.smooth_bins)
            source = "auto"

        print(f"{ch:8d} {n_hits:8d} {peak_ns:10.3f} {source:>8}")

        calib["momentums"][momentum_key][str(ch)] = {
            "offset_ns": peak_ns,
            "peak_ns": peak_ns,
            "source": source,
            "n_hits": n_hits,
            "updated": now,
        }

        if n_hits > 0 and counts is not None:
            fig, ax = plt.subplots(figsize=(6, 4))
            centers = 0.5 * (edges[:-1] + edges[1:])
            ax.bar(centers, counts, width=(edges[1] - edges[0]), color="#4c72b0", align="center")
            ax.axvline(peak_ns, color="#c44e52", linestyle="--",
                       label=f"offset = {peak_ns:.2f} ns ({source})")
            ax.set_xlabel("delay_ns")
            ax.set_ylabel("counts")
            ax.set_title(f"channel {ch} -- momentum {momentum_key}")
            ax.legend(fontsize=8)
            fig.tight_layout()
            fig_path = os.path.join(plot_dir, f"calib_hist_p{momentum_key}_ch{ch}.png")
            fig.savefig(fig_path, dpi=150)
            plt.close(fig)

    save_calib(args.calib_file, calib)
    print(f"\nFichier de calibration mis a jour : {args.calib_file}")
    print(f"Histogrammes enregistres dans     : {plot_dir}/calib_hist_p{momentum_key}_ch<N>.png")


# ---------------------------------------------------------------------- #
# apply
# ---------------------------------------------------------------------- #
def cmd_apply(args):
    calib = load_calib(args.calib_file)
    momentum_key = str(args.momentum)
    offsets = calib.get("momentums", {}).get(momentum_key)
    if not offsets:
        sys.exit(f"ERREUR : pas de calibration pour momentum '{momentum_key}' dans {args.calib_file} "
                  f"(lancer 'measure' d'abord).")

    channels_wanted = parse_channel_list(args.channels) if args.channels else [int(c) for c in offsets]
    missing = [ch for ch in channels_wanted if str(ch) not in offsets]
    if missing:
        sys.exit(f"ERREUR : pas d'offset calibre pour le(s) channel(s) {missing} au momentum '{momentum_key}'.")

    print(f"Lecture de {args.input} ...")
    all_channels, all_delays = load_hits(args.input, offset_ns=args.offset_ns)

    aligned = []
    print(f"\n{'channel':>8} {'n_hits':>8} {'offset_ns':>10}")
    for ch in channels_wanted:
        offset_ns = offsets[str(ch)]["offset_ns"]
        mask = all_channels == ch
        delays = all_delays[mask]
        print(f"{ch:8d} {int(mask.sum()):8d} {offset_ns:10.3f}")
        aligned.append(delays - offset_ns)

    coadded = np.concatenate(aligned) if aligned else np.array([])
    if coadded.size == 0:
        sys.exit("ERREUR : aucun hit trouve pour les channels demandes.")

    fig, ax = plt.subplots(figsize=(7, 4.5))
    rng = tuple(args.range_ns) if args.range_ns else (float(coadded.min()), float(coadded.max()))
    ax.hist(coadded, bins=args.bins, range=rng, color="#4c72b0")
    ax.axvline(0.0, color="#c44e52", linestyle="--", label="ligne des electrons (realignee a 0)")
    ax.set_xlabel("delay_ns - offset(channel)")
    ax.set_ylabel("counts")
    ax.set_title(f"Histogramme co-added -- momentum {momentum_key} -- channels {channels_wanted}")
    ax.legend(fontsize=8)
    fig.tight_layout()

    out_path = args.out or (os.path.splitext(args.input)[0] + f"_coadded_p{momentum_key}.png")
    fig.savefig(out_path, dpi=150)
    print(f"\nTotal hits co-addes : {coadded.size}")
    print(f"Figure enregistree  : {out_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    p_measure = sub.add_parser("measure", help="mesure les offsets d'un run et met a jour le fichier de calibration")
    p_measure.add_argument("input", help="fichier .bin (brut) ou .csv (trigger,channel,delai_ns)")
    p_measure.add_argument("--momentum", required=True, help="label du momentum de ce run (ex: 1500, 1500MeV)")
    p_measure.add_argument("--channels", required=True, help="channels a calibrer, ex: 0,1,2,3")
    p_measure.add_argument("--calib-file", default="calibration.json", help="fichier de calibration JSON (cree si absent)")
    p_measure.add_argument("--plot-dir", default=None, help="dossier de sortie des histogrammes (defaut: a cote du fichier de calibration)")
    p_measure.add_argument("--bins", type=int, default=200)
    p_measure.add_argument("--smooth-bins", type=int, default=3,
                            help="largeur (en bins) du lissage avant detection du pic, 1 = pas de lissage")
    p_measure.add_argument("--search-window-ns", type=float, nargs=2, metavar=("MIN", "MAX"), default=None,
                            help="restreint la recherche automatique du pic a cette plage de delai_ns")
    p_measure.add_argument("--manual-offset", action="append", metavar="CHANNEL=VALEUR_NS",
                            help="force l'offset d'un channel au lieu de la detection automatique (repetable)")
    p_measure.add_argument("--offset-ns", type=float, default=0.0,
                            help="offset (ns) pour la reconstruction du delai depuis un .bin, comme decode_binary.py")
    p_measure.set_defaults(func=cmd_measure)

    p_apply = sub.add_parser("apply", help="realigne les channels avec un fichier de calibration et trace l'histogramme co-added")
    p_apply.add_argument("input", help="fichier .bin (brut) ou .csv (trigger,channel,delai_ns)")
    p_apply.add_argument("--momentum", required=True, help="label du momentum a utiliser dans le fichier de calibration")
    p_apply.add_argument("--calib-file", default="calibration.json")
    p_apply.add_argument("--channels", default=None, help="sous-ensemble de channels (defaut: tous ceux calibres pour ce momentum)")
    p_apply.add_argument("--bins", type=int, default=200)
    p_apply.add_argument("--range-ns", type=float, nargs=2, metavar=("MIN", "MAX"), default=None)
    p_apply.add_argument("--out", default=None, help="fichier image de sortie")
    p_apply.add_argument("--offset-ns", type=float, default=0.0)
    p_apply.set_defaults(func=cmd_apply)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
