#!/usr/bin/env python3
"""
decode_binary.py

Relit a posteriori un fichier binaire brut enregistre par
read_output_buffer_blt (option -b <fichier>) : une suite de mots de 32
bits, tels que lus en BLT32 depuis l'Output Buffer du V1290N, sans
aucune perte (contrairement au CSV du mode stream qui ne garde que les
TDC MEASUREMENT). Reproduit le meme decodage que v1290_decode.h /
mode stream (-s) de read_output_buffer_blt.c.

Usage :
  python3 decode_binary.py <fichier.bin> [--offset-ns N] [--csv-out sortie.csv] [--verbose N]

  --offset-ns N     offset (ns) ajoute a la mesure pour reconstruire le
                     delai par rapport au trigger, comme le -s du C
                     (necessite que SUB_TRG etait actif pendant
                     l'acquisition). Defaut : 0.
  --csv-out FICHIER si fourni, ecrit une ligne CSV
                     "numero_trigger,channel,delai_ns" par TDC
                     MEASUREMENT (meme format que le mode stream).
  --verbose N       affiche le decodage detaille des N premiers mots
                     (0 = aucun, defaut).
"""

import argparse
import struct
import sys

ASSUMED_RESOLUTION_PS = 25

TYPE_GLOBAL_HEADER = 0x08
TYPE_GLOBAL_TRAILER = 0x10
TYPE_TDC_HEADER = 0x01
TYPE_TDC_MEASUREMENT = 0x00
TYPE_TDC_ERROR = 0x04
TYPE_TDC_TRAILER = 0x03
TYPE_FILLER = 0x18


def word_type(w):
    return (w >> 27) & 0x1F


def decode_word(w, index, verbose):
    t = word_type(w)
    if verbose:
        line = f"  [{index:5d}] 0x{w:08X}"

    if t == TYPE_GLOBAL_HEADER:
        event_count = (w >> 5) & 0x3FFFFF
        geo = w & 0x1F
        if verbose:
            print(f"{line}  GLOBAL HEADER  (GEO={geo}, event_count={event_count})")
        return ("global_header", event_count)

    if t == TYPE_GLOBAL_TRAILER:
        word_count = (w >> 5) & 0x3FFF
        status_bits = (w >> 24) & 0x7
        if verbose:
            print(f"{line}  GLOBAL TRAILER  (word_count={word_count}, status_bits=0x{status_bits:X})")
        return ("global_trailer", None)

    if t == TYPE_TDC_HEADER:
        tdc_num = (w >> 24) & 0x3
        event_id = (w >> 12) & 0xFFF
        bunch_id = w & 0xFFF
        if verbose:
            print(f"{line}  TDC HEADER  (TDC={tdc_num}, event_id={event_id}, bunch_id={bunch_id})")
        return ("tdc_header", None)

    if t == TYPE_TDC_ERROR:
        tdc_num = (w >> 24) & 0x3
        flags = w & 0x7FFF
        if verbose:
            print(f"{line}  TDC ERROR  (TDC={tdc_num}, error_flags=0x{flags:04X})")
        return ("tdc_error", None)

    if t == TYPE_TDC_TRAILER:
        tdc_num = (w >> 24) & 0x3
        event_id = (w >> 12) & 0xFFF
        word_cnt = w & 0xFFF
        if verbose:
            print(f"{line}  TDC TRAILER  (TDC={tdc_num}, event_id={event_id}, word_count={word_cnt})")
        return ("tdc_trailer", None)

    if t == TYPE_FILLER:
        if verbose:
            print(f"{line}  FILLER")
        return ("filler", None)

    if t == TYPE_TDC_MEASUREMENT:
        edge = (w >> 26) & 0x1
        channel = (w >> 21) & 0x1F
        measure = w & 0x1FFFFF
        measure_ns = measure * ASSUMED_RESOLUTION_PS / 1000.0
        if verbose:
            edge_name = "trailing" if edge else "leading"
            print(f"{line}  TDC MEASUREMENT  ({edge_name}, channel={channel}, "
                  f"mesure={measure} [x{ASSUMED_RESOLUTION_PS}ps] = {measure_ns:.3f} ns)")
        return ("measurement", (channel, measure_ns))

    if verbose:
        print(f"{line}  UNKNOWN")
    return ("unknown", None)


def main():
    ap = argparse.ArgumentParser(description="Decode un dump binaire V1290N (BLT32 brut)")
    ap.add_argument("bin_path")
    ap.add_argument("--offset-ns", type=float, default=0.0)
    ap.add_argument("--csv-out", default=None)
    ap.add_argument("--verbose", type=int, default=0)
    args = ap.parse_args()

    with open(args.bin_path, "rb") as f:
        raw = f.read()

    n_words = len(raw) // 4
    if len(raw) % 4 != 0:
        print(f"ATTENTION : taille fichier ({len(raw)} octets) non multiple de 4, "
              f"{len(raw) % 4} octet(s) ignore(s) en fin de fichier.", file=sys.stderr)
    words = struct.unpack(f"<{n_words}I", raw[: n_words * 4])

    csv_file = open(args.csv_out, "w", encoding="utf-8") if args.csv_out else None

    counts = {
        "global_header": 0, "global_trailer": 0, "tdc_header": 0,
        "tdc_error": 0, "tdc_trailer": 0, "filler": 0, "measurement": 0, "unknown": 0,
    }
    current_event = -1

    for i, w in enumerate(words):
        kind, payload = decode_word(w, i, args.verbose and i < args.verbose)
        counts[kind] += 1

        if kind == "global_header":
            current_event = payload
        elif kind == "measurement" and csv_file:
            channel, measure_ns = payload
            csv_file.write(f"{current_event},{channel},{args.offset_ns + measure_ns:.3f}\n")

    if csv_file:
        csv_file.close()

    print(f"Fichier            : {args.bin_path}")
    print(f"Mots totaux        : {n_words}")
    for k, v in counts.items():
        print(f"  {k:<15} {v}")
    if args.csv_out:
        print(f"CSV ecrit          : {args.csv_out} ({counts['measurement']} lignes)")


if __name__ == "__main__":
    main()
