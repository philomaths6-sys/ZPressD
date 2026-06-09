#!/usr/bin/env python3
"""plot_results.py — generate comparison graphs from benchmark CSVs"""

import csv
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
from pathlib import Path

RESULTS_DIR = Path(__file__).parent / 'results'
SCENARIOS   = ['light', 'medium', 'heavy']

def load_csv(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append({k: float(v) for k, v in row.items()})
    return rows

def col(rows, key):
    return [r[key] for r in rows]

fig = plt.figure(figsize=(18, 14))
fig.suptitle('ZPressD Benchmark Results — Baseline vs Optimized', fontsize=16, fontweight='bold')
gs = gridspec.GridSpec(3, 3, figure=fig, hspace=0.5, wspace=0.4)

for si, scenario in enumerate(SCENARIOS):
    try:
        base = load_csv(RESULTS_DIR / f'{scenario}_baseline.csv')
        opt  = load_csv(RESULTS_DIR / f'{scenario}_daemon.csv')
    except FileNotFoundError:
        print(f'Missing data for {scenario} — skipping')
        continue

    t_b = list(range(len(base)))
    t_o = list(range(len(opt)))

    # PSI some.avg10
    ax1 = fig.add_subplot(gs[si, 0])
    ax1.plot(t_b, col(base, 'psi_some_avg10'), 'r-', label='Baseline', alpha=0.8)
    ax1.plot(t_o, col(opt,  'psi_some_avg10'), 'g-', label='ZPressD',  alpha=0.8)
    ax1.set_title(f'{scenario.capitalize()} — PSI some.avg10 (%)')
    ax1.set_ylabel('PSI (%)'); ax1.legend(); ax1.grid(True, alpha=0.3)

    # Swap-out rate
    ax2 = fig.add_subplot(gs[si, 1])
    ax2.plot(t_b, col(base, 'swap_out_rate'), 'r-', label='Baseline', alpha=0.8)
    ax2.plot(t_o, col(opt,  'swap_out_rate'), 'g-', label='ZPressD',  alpha=0.8)
    ax2.set_title(f'{scenario.capitalize()} — Swap-Out Rate (pages/s)')
    ax2.set_ylabel('pages/s'); ax2.legend(); ax2.grid(True, alpha=0.3)
    # zram compression ratio
    ax3 = fig.add_subplot(gs[si, 2])
    ax3.plot(t_b, col(base, 'zram_ratio'), 'r-', label='Baseline', alpha=0.8)
    ax3.plot(t_o, col(opt,  'zram_ratio'), 'g-', label='ZPressD',  alpha=0.8)
    ax3.set_title(f'{scenario.capitalize()} — zram Compression Ratio')
    ax3.set_ylabel('orig/compr'); ax3.legend(); ax3.grid(True, alpha=0.3)

out = RESULTS_DIR / 'benchmark_comparison.png'
plt.savefig(out, dpi=150, bbox_inches='tight')
print(f'Saved: {out}')
plt.show()