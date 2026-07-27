#!/usr/bin/env python3

import csv
import os
import sys
import matplotlib.pyplot as plt

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <manifest.txt>", file=sys.stderr)
    sys.exit(1)

manifest_path = sys.argv[1]
configs = []

with open(manifest_path, encoding="utf-8") as manifest:
    for line in manifest:
        line = line.strip()
        if not line:
            continue
        label, width_ns, offset_ns, csv_path = line.split(",", 3)
        configs.append({"label": label, "width_ns": float(width_ns), "offset_ns": float(offset_ns), "csv_path": csv_path})

fig, ax = plt.subplots(figsize=(10, 6))

for config in configs:
    values = []
    try:
        with open(config["csv_path"], encoding="utf-8") as csv_file:
            reader = csv.reader(csv_file)
            for row in reader:
                if not row:
                    continue
                try:
                    values.append(float(row[-1]))
                except ValueError:
                    continue
    except FileNotFoundError:
        print(f"Fichier introuvable : {config['csv_path']}", file=sys.stderr)
        continue
    label = f"width={config['width_ns']:.0f} ns offset={config['offset_ns']:.0f} ns (N={len(values)})"
    if values:
        ax.hist(values, bins=40, histtype="step", linewidth=1.8, label=label)
    else:
        ax.plot([], [], linewidth=1.8, label=f"{label} [aucun hit]")

ax.set_xlabel("Délai par rapport au trigger (ns)")
ax.set_ylabel("Coups")
ax.set_title("Comparaison des distributions de délais")
ax.legend()
fig.tight_layout()

output_path = os.path.join(os.path.dirname(manifest_path), "comparison.png")
fig.savefig(output_path, dpi=150)
print(f"Figure sauvegardée : {output_path}")
plt.show()