#!/usr/bin/env python3
"""
plot_results.py — ZPressD benchmark analysis and visualization

- Prints terminal summary for ALL scenarios (heavy / medium / light)
- Plots graph ONLY for the heavy scenario (where ZPressD effect is clearest)
- White background, clean axes, summary numbers embedded in the figure
"""

import csv
import statistics
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import matplotlib.patches as mpatches
import numpy as np

RESULTS_DIR = Path(__file__).parent / 'results'
SCENARIOS   = ['heavy', 'medium', 'light']

COLOR_BASE = '#d62728'   # red  — baseline
COLOR_ZP   = '#2ca02c'   # green — ZPressD

# ── Helpers ───────────────────────────────────────────────────────────────────

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

def pct_reduction(base, opt):
    """Return % reduction from base to opt. Positive = improvement."""
    if base > 0:
        return (base - opt) / base * 100
    return None

def load_latency(path):
    try:
        return [float(l.strip()) for l in open(path) if l.strip()]
    except FileNotFoundError:
        return []

# ── Terminal summary for ALL scenarios ────────────────────────────────────────

print()
print("ZPressD Benchmark Summary")
print("-" * 40)

all_pswpout_reductions = []
all_psi_reductions     = []
all_latency_reductions = []

scenario_summaries = {}   # used later for graph annotation

for scenario in SCENARIOS:
    base = load_csv(RESULTS_DIR / f'{scenario}_baseline.csv')
    opt  = load_csv(RESULTS_DIR / f'{scenario}_daemon.csv')
    lat_b = load_latency(RESULTS_DIR / f'{scenario}_baseline_latency.txt')
    lat_o = load_latency(RESULTS_DIR / f'{scenario}_daemon_latency.txt')

    if not base or not opt:
        print(f"\n  [{scenario}]  (no data)")
        continue

    print(f"\n  [{scenario}]")
    s = {}

    # Disk swap I/O
    b_swap = avg(col(base, 'pswpout_rate'))
    o_swap = avg(col(opt,  'pswpout_rate'))
    r = pct_reduction(b_swap, o_swap)
    if r is not None:
        all_pswpout_reductions.append(r)
        print(f"    Disk swap I/O:  {b_swap:.1f} → {o_swap:.1f} pages/s  ({r:+.0f}%)")
        s['swap'] = (b_swap, o_swap, r)
    else:
        print(f"    Disk swap I/O:  baseline=0 (no pressure generated)")
        s['swap'] = (0, 0, None)

    # zswap stored pages
    b_zs = avg(col(base, 'zswap_stored_pages'))
    o_zs = avg(col(opt,  'zswap_stored_pages'))
    print(f"    zswap stored:   baseline={b_zs:.0f}  ZPressD={o_zs:.0f} pages")
    s['zswap'] = (b_zs, o_zs)

    # PSI stall
    b_psi = avg(col(base, 'psi_full_avg10'))
    o_psi = avg(col(opt,  'psi_full_avg10'))
    r = pct_reduction(b_psi, o_psi)
    if r is not None:
        all_psi_reductions.append(r)
        print(f"    PSI stall:      {b_psi:.2f}% → {o_psi:.2f}%  ({r:+.0f}%)")
        s['psi'] = (b_psi, o_psi, r)
    else:
        print(f"    PSI stall:      baseline=0")
        s['psi'] = (0, 0, None)

    # Latency
    if lat_b and lat_o:
        b_lat = avg(lat_b)
        o_lat = avg(lat_o)
        r = pct_reduction(b_lat, o_lat)
        if r is not None:
            all_latency_reductions.append(r)
            print(f"    Latency:        {b_lat:.1f}ms → {o_lat:.1f}ms  ({r:+.0f}%)")
            s['latency'] = (b_lat, o_lat, lat_b, lat_o)
        else:
            s['latency'] = None
    else:
        s['latency'] = None

    scenario_summaries[scenario] = s

# Aggregate
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

# ── Graph: HEAVY scenario only ─────────────────────────────────────────────────

base_h = load_csv(RESULTS_DIR / 'heavy_baseline.csv')
opt_h  = load_csv(RESULTS_DIR / 'heavy_daemon.csv')
lat_b_h = load_latency(RESULTS_DIR / 'heavy_baseline_latency.txt')
lat_o_h = load_latency(RESULTS_DIR / 'heavy_daemon_latency.txt')

if not base_h or not opt_h:
    print("[!] No heavy scenario data to plot.")
    exit(0)

t_b = list(range(len(base_h)))
t_o = list(range(len(opt_h)))

METRICS = [
    ('pswpout_rate',       'Disk Swap-Out Rate',    'pages/s'),
    ('zswap_stored_pages', 'zswap Compressed Pages', 'pages'),
    ('psi_full_avg10',     'PSI Full Stall',         '% stall'),
    ('mem_avail_kb',       'Available RAM',           'kB'),
]

fig = plt.figure(figsize=(16, 11), facecolor='white')
fig.suptitle('ZPressD — Heavy Workload: Baseline vs ZPressD Daemon',
             fontsize=15, fontweight='bold', color='#111111', y=0.99)

# 2 rows × 3 cols: top row = 3 metrics, bottom-left 2 = 1 metric + latency bar, bottom-right = annotation
gs = gridspec.GridSpec(2, 3, figure=fig, hspace=0.45, wspace=0.38,
                       top=0.93, bottom=0.08, left=0.07, right=0.97)

METRIC_AXES = [
    gs[0, 0], gs[0, 1], gs[0, 2],   # top row: swap, zswap, psi
    gs[1, 0],                         # bottom-left: mem_avail
]
METRIC_PLOT = METRICS  # 4 metrics → 4 axes

legend_added = False
for i, ((metric, title, unit), pos) in enumerate(zip(METRIC_PLOT, METRIC_AXES)):
    ax = fig.add_subplot(pos)
    ax.set_facecolor('#f9f9f9')
    for spine in ax.spines.values():
        spine.set_edgecolor('#cccccc')

    vals_b = col(base_h, metric)
    vals_o = col(opt_h,  metric)

    l1, = ax.plot(t_b, vals_b, color=COLOR_BASE, linewidth=2.0, label='Baseline', alpha=0.9)
    l2, = ax.plot(t_o, vals_o, color=COLOR_ZP,   linewidth=2.0, label='ZPressD',  alpha=0.9)

    # Shade the difference area
    min_len = min(len(vals_b), len(vals_o))
    ax.fill_between(range(min_len), vals_b[:min_len], vals_o[:min_len],
                    alpha=0.08, color=COLOR_ZP)

    ax.set_title(title, fontsize=10, fontweight='semibold', color='#222222', pad=5)
    ax.set_ylabel(unit, fontsize=8.5, color='#555555')
    ax.set_xlabel('sample (2s interval)', fontsize=7.5, color='#888888')
    ax.tick_params(colors='#555555', labelsize=7.5)
    ax.grid(True, alpha=0.35, color='#dddddd', linestyle='--')
    if not legend_added:
        ax.legend(fontsize=8.5, framealpha=0.8, loc='upper right')
        legend_added = True

# ── Latency bar chart (bottom-centre) ─────────────────────────────────────────
ax_lat = fig.add_subplot(gs[1, 1])
ax_lat.set_facecolor('#f9f9f9')
for spine in ax_lat.spines.values():
    spine.set_edgecolor('#cccccc')

if lat_b_h and lat_o_h:
    x = np.arange(2)
    means = [avg(lat_b_h), avg(lat_o_h)]
    errs  = [statistics.stdev(lat_b_h) if len(lat_b_h) > 1 else 0,
             statistics.stdev(lat_o_h) if len(lat_o_h) > 1 else 0]
    bars = ax_lat.bar(x, means, yerr=errs, color=[COLOR_BASE, COLOR_ZP],
                      alpha=0.85, width=0.5, capsize=6,
                      error_kw={'ecolor': '#333333', 'alpha': 0.6})
    ax_lat.set_xticks(x)
    ax_lat.set_xticklabels(['Baseline', 'ZPressD'], fontsize=9, color='#222222')
    ax_lat.set_ylabel('Latency (ms)', fontsize=8.5, color='#555555')
    ax_lat.set_title('Interactive Latency\n(under peak pressure)', fontsize=10,
                     fontweight='semibold', color='#222222', pad=5)
    ax_lat.tick_params(colors='#555555', labelsize=7.5)
    ax_lat.grid(True, alpha=0.35, color='#dddddd', linestyle='--', axis='y')
    for bar, val in zip(bars, means):
        ax_lat.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + max(errs) * 0.05 + 0.5,
                    f'{val:.1f} ms', ha='center', va='bottom',
                    fontsize=9, fontweight='bold', color='#111111')
else:
    ax_lat.text(0.5, 0.5, 'No latency data', ha='center', va='center',
                transform=ax_lat.transAxes, color='#888888', fontsize=10)
    ax_lat.set_title('Interactive Latency', fontsize=10, fontweight='semibold')

# ── Summary annotation box (bottom-right) ─────────────────────────────────────
ax_ann = fig.add_subplot(gs[1, 2])
ax_ann.axis('off')

# Build annotation lines from heavy-specific numbers where available
hs = scenario_summaries.get('heavy', {})

def fmt_r(key, label, unit='%'):
    entry = hs.get(key)
    if entry and entry[2] is not None:
        return f"{label}:  {entry[2]:.0f}%"
    return f"{label}:  n/a"

swap_h = hs.get('swap')
psi_h  = hs.get('psi')
lat_h  = hs.get('latency')

lines = []
lines.append("Heavy Workload Results")
lines.append("")

if swap_h and swap_h[2] is not None:
    lines.append(f"Disk swap I/O:  ↓{swap_h[2]:.0f}%")
    lines.append(f"  {swap_h[0]:.1f} → {swap_h[1]:.1f} pages/s")
else:
    lines.append("Disk swap I/O:  n/a")

lines.append("")

if psi_h and psi_h[2] is not None:
    lines.append(f"PSI stall time:  ↓{psi_h[2]:.0f}%")
    lines.append(f"  {psi_h[0]:.2f}% → {psi_h[1]:.2f}%")
else:
    lines.append("PSI stall time:  n/a")

lines.append("")

if lat_h:
    b_l, o_l = lat_h[0], lat_h[1]
    r_l = pct_reduction(b_l, o_l)
    if r_l is not None:
        lines.append(f"Interactive latency:  ↓{r_l:.0f}%")
        lines.append(f"  {b_l:.1f} ms → {o_l:.1f} ms")
    else:
        lines.append("Interactive latency:  n/a")
else:
    lines.append("Interactive latency:  n/a")

lines.append("")
lines.append("─" * 28)
lines.append(f"Reduced disk I/O by {swap_str},")
lines.append(f"PSI stall by {psi_str},")
lines.append(f"latency within {lat_str} of baseline.")

ann_text = "\n".join(lines)

ax_ann.text(0.05, 0.97, ann_text,
            transform=ax_ann.transAxes,
            fontsize=8.5, verticalalignment='top', fontfamily='monospace',
            color='#111111',
            bbox=dict(boxstyle='round,pad=0.7', facecolor='#eef6ee',
                      edgecolor='#2ca02c', linewidth=1.5, alpha=0.95))

# ── Save ──────────────────────────────────────────────────────────────────────
out = RESULTS_DIR / 'benchmark_comparison.png'
plt.savefig(out, dpi=150, bbox_inches='tight', facecolor='white')
print(f"Graph saved: {out}")
plt.show()