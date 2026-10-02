#!/usr/bin/env python3
import sys
import numpy as np

RES_PS = 25

fichier = sys.argv[1]
channel = int(sys.argv[2])

w = np.fromfile(fichier, dtype="<u4")

types = w >> 27
headers = np.where(types == 0x08)[0]
meas = np.where(types == 0x00)[0]

event = np.searchsorted(headers, meas, side="right") - 1

ch = (w[meas] >> 21) & 0x1F
raw = w[meas] & 0x1FFFFF

raw = raw[ch == channel]
event = event[ch == channel]

n = 0
gaps = []

for e in np.unique(event):
    x = np.sort(raw[event == e])

    if len(x) > 1:
        n += 1
        gaps.extend(np.diff(x) * RES_PS / 1000)

print(f"Channel {channel}")
print(f"Hits       : {len(raw)}")
print(f"Multi-hits : {n}")
if gaps:
    print(f"Ecart min  : {min(gaps):.2f} ns")
    print(f"Ecart méd. : {np.median(gaps):.2f} ns")
    print(f"Ecart max  : {max(gaps):.2f} ns")
