#!/usr/bin/env python3
"""
plot_results.py — ZPressD benchmark analysis and visualization

Generates comparison graphs for each metric ZPressD directly affects:
  1. pswpout_rate       — disk swap I/O (ZPressD reduces this)
  2. zswap_stored_pages — compressed page pool (ZPressD increases this)
  3. psi_full_avg10     — CPU stall time on memory (ZPressD reduces this)
  4. mem_avail_kb       — available memory (ZPressD keeps this higher longer)
  5. Interactive latency summary bar chart

Also prints the summary numbers for the README/interview one-liner.
"""

import csv
import statistics
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np

RESULTS_DIR = Path(__file__).parent / 'results'
SCENARIOS   = ['heavy', 'medium', 'light']

# Color palette
COLOR_BASE = '#e05252'   # red-ish for baseline
COLOR_ZP   = '#52c87a'   # green for ZPressD
COLOR_BG   = '#1a1a2e'

def load_csv(path):
    rows = []
    try:
        with open(path) as f:
            for row in csv.DictReader(f):
                try:
                    rows.append({k: float(v) for k, v in row.items()})
                except ValueError:
                    pass
    except FileNotFoundError:
        return []
    return rows

def col(rows, key):
    return [r.get(key, 0.0) for r in rows]

def avg(lst):
    return statistics.mean(lst) if lst else 0.0

def load_latency(path):
    try:
        vals = [float(l.strip()) for l in open(path) if l.strip()]
        return vals
    except FileNotFoundError:
        return []

# ── Build figure ──────────────────────────────────────────────────────────────
fig = plt.figure(figsize=(20, 16), facecolor=COLOR_BG)
fig.suptitle('ZPressD Benchmark: Baseline vs ZPressD Daemon',
             fontsize=18, fontweight='bold', color='white', y=0.98)

# 4 metric rows × 3 scenario columns + 1 summary row
gs = gridspec.GridSpec(5, 3, figure=fig, hspace=0.55, wspace=0.35,
                       top=0.94, bottom=0.04, left=0.06, right=0.97)

METRIC_ROWS = [
    ('pswpout_rate',       'Disk Swap-Out Rate (pages/s)',    'pages/s',  True),
    ('zswap_stored_pages', 'zswap Compressed Pages',          'pages',    False),
    ('psi_full_avg10',     'PSI Memory Full Stall (%)',       '%',        True),
    ('mem_avail_kb',       'Available RAM (kB)',              'kB',       False),
]

summary = {}   # { scenario: { metric: (base_avg, zp_avg) } }

for si, scenario in enumerate(SCENARIOS):
    base = load_csv(RESULTS_DIR / f'{scenario}_baseline.csv')
    opt  = load_csv(RESULTS_DIR / f'{scenario}_daemon.csv')

    if not base or not opt:
        print(f'[!] Missing CSV data for scenario "{scenario}" — skipping')
        continue

    summary[scenario] = {}
    t_b = list(range(len(base)))
    t_o = list(range(len(opt)))

    for ri, (metric, title, unit, lower_is_better) in enumerate(METRIC_ROWS):
        ax = fig.add_subplot(gs[ri, si])
        ax.set_facecolor('#0d0d1a')
        for spine in ax.spines.values():
            spine.set_edgecolor('#333355')

        vals_b = col(base, metric)
        vals_o = col(opt,  metric)

        ax.plot(t_b, vals_b, color=COLOR_BASE, linewidth=1.5, label='Baseline', alpha=0.9)
        ax.plot(t_o, vals_o, color=COLOR_ZP,   linewidth=1.5, label='ZPressD',  alpha=0.9)

        ax.set_title(f'{scenario.capitalize()} — {title}',
                     color='#cccccc', fontsize=8.5, pad=4)
        ax.set_ylabel(unit, color='#888888', fontsize=7.5)
        ax.tick_params(colors='#888888', labelsize=7)
        ax.legend(fontsize=7, framealpha=0.2, labelcolor='white')
        ax.grid(True, alpha=0.15, color='#444466')

        # Store averages for summary
        avg_b = avg(vals_b)
        avg_o = avg(vals_o)
        summary[scenario][metric] = (avg_b, avg_o, lower_is_better)

# ── Latency summary bar chart (row 4) ─────────────────────────────────────────
for si, scenario in enumerate(SCENARIOS):
    ax = fig.add_subplot(gs[4, si])
    ax.set_facecolor('#0d0d1a')
    for spine in ax.spines.values():
        spine.set_edgecolor('#333355')

    lat_b = load_latency(RESULTS_DIR / f'{scenario}_baseline_latency.txt')
    lat_o = load_latency(RESULTS_DIR / f'{scenario}_daemon_latency.txt')

    if lat_b and lat_o:
        x = np.arange(2)
        means = [avg(lat_b), avg(lat_o)]
        errs  = [statistics.stdev(lat_b) if len(lat_b)>1 else 0,
                 statistics.stdev(lat_o) if len(lat_o)>1 else 0]
        bars = ax.bar(x, means, yerr=errs, color=[COLOR_BASE, COLOR_ZP],
                      alpha=0.85, width=0.5, capsize=5,
                      error_kw={'ecolor': 'white', 'alpha': 0.5})
        ax.set_xticks(x)
        ax.set_xticklabels(['Baseline', 'ZPressD'], color='#cccccc', fontsize=8)
        ax.set_ylabel('Latency (ms)', color='#888888', fontsize=7.5)
        ax.set_title(f'{scenario.capitalize()} — Interactive Latency (under pressure)',
                     color='#cccccc', fontsize=8.5, pad=4)
        ax.tick_params(colors='#888888', labelsize=7)
        ax.grid(True, alpha=0.15, color='#444466', axis='y')
        for bar, val in zip(bars, means):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                    f'{val:.1f}ms', ha='center', va='bottom',
                    color='white', fontsize=7.5, fontweight='bold')
        summary[scenario]['latency'] = (avg(lat_b), avg(lat_o), True)
    else:
        ax.text(0.5, 0.5, 'No latency data', ha='center', va='center',
                transform=ax.transAxes, color='#888888')
        ax.set_title(f'{scenario.capitalize()} — Interactive Latency',
                     color='#cccccc', fontsize=8.5)

# ── Print summary numbers ──────────────────────────────────────────────────────
print()
print("ZPressD Benchmark Summary")
print("-" * 40)

all_pswpout_reductions = []
all_psi_reductions     = []
all_latency_reductions = []

for scenario in SCENARIOS:
    if scenario not in summary:
        continue
    print(f"\n  [{scenario}]")
    s = summary[scenario]

    if 'pswpout_rate' in s:
        b, o, _ = s['pswpout_rate']
        if b > 0:
            pct = (b - o) / b * 100
            all_pswpout_reductions.append(pct)
            print(f"    Disk swap I/O:  {b:.1f} → {o:.1f} pages/s  ({pct:+.0f}%)")
        else:
            print(f"    Disk swap I/O:  baseline=0 (no pressure generated — reduce VM RAM)")

    if 'zswap_stored_pages' in s:
        b, o, _ = s['zswap_stored_pages']
        print(f"    zswap stored:   baseline={b:.0f}  ZPressD={o:.0f} pages")

    if 'psi_full_avg10' in s:
        b, o, _ = s['psi_full_avg10']
        if b > 0:
            pct = (b - o) / b * 100
            all_psi_reductions.append(pct)
            print(f"    PSI stall:      {b:.2f}% → {o:.2f}%  ({pct:+.0f}%)")
        else:
            print(f"    PSI stall:      baseline=0 (no pressure generated)")

    if 'latency' in s:
        b, o, _ = s['latency']
        if b > 0:
            pct = (b - o) / b * 100
            all_latency_reductions.append(pct)
            print(f"    Latency:        {b:.1f}ms → {o:.1f}ms  ({pct:+.0f}%)")

print()
print("Aggregate results (across all scenarios)")
print("-" * 40)

swap_str = f"{avg(all_pswpout_reductions):.0f}%" if all_pswpout_reductions else "n/a"
psi_str  = f"{avg(all_psi_reductions):.0f}%"     if all_psi_reductions     else "n/a"
lat_str  = f"{avg(all_latency_reductions):.0f}%"  if all_latency_reductions  else "n/a"

print(f"  Disk swap I/O reduction:  {swap_str}")
print(f"  PSI stall reduction:      {psi_str}")
print(f"  Interactive latency Δ:    {lat_str}")

print()
print("One-liner summary:")
print(f"  ZPressD reduced disk swap I/O by {swap_str}, PSI stall time by {psi_str},")
print(f"  keeping interactive latency within {lat_str} of baseline.")
print()

# ── Save ──────────────────────────────────────────────────────────────────────
out = RESULTS_DIR / 'benchmark_comparison.png'
plt.savefig(out, dpi=150, bbox_inches='tight', facecolor=COLOR_BG)
print(f'Saved: {out}')
plt.show()