#!/usr/bin/env python3
"""
sipm_channel_map.py

Mapping fixe entre les channels du V1290A et les barres SiPM du hodoscope,
d'apres le cablage des MCFD modules #6 et #7 (D 1-8 / D 9-16 a l'origine,
D et C sur ce cablage). Source unique pour sipm_matrix.py,
live_sipm_matrix.py et daq_gui.py -- MODIFIER ICI si le cablage change
a nouveau (une seule barre a corriger plutot que trois scripts).

Cablage MCFD (channels du module, avant renumerotation V1290A) :

  module #6 :
    ch 0-6   = D 4-10 T  (ordre croissant)
    ch 7-13  = D 4-10 B  (ordre croissant)
    ch 14-15 = C 13 L, C 13 R

  module #7 :
    ch 0-7   = C 5-12 L  (ordre croissant)
    ch 8-15  = C 5-12 R  (ordre croissant)

Renumerotation cote V1290A (32 channels) :
  module #6 -> inputs 16-31 (channel 0 du module devient le channel 16)
  module #7 -> inputs 0-15  (channel 0 du module reste le channel 0)

D'ou, en channels V1290A : chaque barre C (C5..C13) est lue par 2 channels
(L, R) et chaque barre D (D4..D10) est lue par 2 channels (T, B) -- les 32
channels du V1290A sont integralement utilises par ce cablage.
"""


def _module7_channel(i):
    return i


def _module6_channel(i):
    return 16 + i


# barre -> [channel L/T, channel R/B] (channels V1290A)
C_BAR_CHANNELS = {}
for _idx, _num in enumerate(range(5, 13)):  # C5..C12, module #7
    _label = f"C{_num}"
    C_BAR_CHANNELS[_label] = [_module7_channel(_idx), _module7_channel(8 + _idx)]
C_BAR_CHANNELS["C13"] = [_module6_channel(14), _module6_channel(15)]  # module #6, L puis R

D_BAR_CHANNELS = {}
for _idx, _num in enumerate(range(4, 11)):  # D4..D10, module #6
    _label = f"D{_num}"
    D_BAR_CHANNELS[_label] = [_module6_channel(_idx), _module6_channel(7 + _idx)]  # T puis B

C_BAR_ORDER = [f"C{n}" for n in range(5, 14)]   # C5..C13
D_BAR_ORDER = [f"D{n}" for n in range(4, 11)]   # D4..D10

# channel V1290A -> (axe, barre, cote) pour reference/debug
CHANNEL_TO_BAR = {}
for _label, _chans in C_BAR_CHANNELS.items():
    for _side, _ch in zip(("L", "R"), _chans):
        CHANNEL_TO_BAR[_ch] = ("C", _label, _side)
for _label, _chans in D_BAR_CHANNELS.items():
    for _side, _ch in zip(("T", "B"), _chans):
        CHANNEL_TO_BAR[_ch] = ("D", _label, _side)

ALL_CHANNELS = sorted(CHANNEL_TO_BAR)


def bar_counts(channel_counts, bar_channels, bar_order):
    """channel_counts: {channel: n_hits}. Retourne {barre: total} en sommant
    les 2 channels (les 2 extremites) de chaque barre, dans l'ordre bar_order."""
    return {bar: sum(channel_counts.get(ch, 0) for ch in bar_channels[bar]) for bar in bar_order}


if __name__ == "__main__":
    print(f"{len(ALL_CHANNELS)} channels V1290A mappes (attendu 32) : {ALL_CHANNELS}")
    print(f"\n{len(C_BAR_ORDER)} barres C : {C_BAR_ORDER}")
    for bar in C_BAR_ORDER:
        print(f"  {bar}: channels {C_BAR_CHANNELS[bar]} (L, R)")
    print(f"\n{len(D_BAR_ORDER)} barres D : {D_BAR_ORDER}")
    for bar in D_BAR_ORDER:
        print(f"  {bar}: channels {D_BAR_CHANNELS[bar]} (T, B)")
