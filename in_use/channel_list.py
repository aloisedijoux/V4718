#!/usr/bin/env python3
"""
channel_list.py

Parsing partage pour les listes de channels TDC saisies en texte (GUI ou
ligne de commande) : accepte les valeurs individuelles ET les plages
"LOW-HIGH", melangees, separees par des virgules.

Exemples :
  "1,2,3"      -> [1, 2, 3]
  "1-5,16"     -> [1, 2, 3, 4, 5, 16]
  "0-3,8,10-12" -> [0, 1, 2, 3, 8, 10, 11, 12]

L'ordre est preserve tel qu'ecrit (les plages sont deroulees en ordre
croissant), les doublons ne sont PAS retires (si l'appelant veut les
dedupliquer, c'est son choix).
"""


def parse_channel_list(s):
    """Parse une liste de channels avec plages. Leve ValueError (message
    clair, utilisable tel quel dans un message d'erreur) sur une entree
    invalide."""
    channels = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            bounds = part.split("-")
            if len(bounds) != 2:
                raise ValueError(f"invalid range '{part}' (expected LOW-HIGH)")
            try:
                low, high = int(bounds[0].strip()), int(bounds[1].strip())
            except ValueError:
                raise ValueError(f"invalid range '{part}' (expected LOW-HIGH, integers)")
            if low > high:
                raise ValueError(f"invalid range '{part}' (LOW must be <= HIGH)")
            channels.extend(range(low, high + 1))
        else:
            try:
                channels.append(int(part))
            except ValueError:
                raise ValueError(f"invalid channel '{part}' (expected an integer or a LOW-HIGH range)")
    return channels


if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:]:
        try:
            print(f"{arg!r} -> {parse_channel_list(arg)}")
        except ValueError as e:
            print(f"{arg!r} -> ERROR: {e}")
