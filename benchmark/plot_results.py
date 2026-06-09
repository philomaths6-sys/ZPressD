#!/usr/bin/env python3
"""
plot_results.py — ZPressD benchmark analysis

Terminal output : summary for ALL scenarios (heavy / medium / light)
Graph output    : 2 separate PNG files for the heavy scenario only
  - disk_io_comparison.png  — Disk Swap-Out Rate (pswpout_rate)
  - psi_stall_comparison.png — PSI Memory Full Stall (psi_full_avg10)
"""

import csv
import statistics
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

RESULTS_DIR = Path(__file__).parent / 'results'
SCENARIOS   = ['heavy', 'medium', 'light']

COLOR_BASE = '#c0392b'   # dark red  — Baseline
COLOR_ZP   = '#27ae60'   # dark green — ZPressD
COLOR_FILL_BASE = '#f1948a'
COLOR_FILL_ZP   = '#a9dfbf'

# ── Helpers ────────────────────────────────────────────────────────────────────

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
    if base > 0:
        return (base - opt) / base * 100
    return None

def load_latency(path):
    try:
        return [float(l.strip()) for l in open(path) if l.strip()]
    except FileNotFoundError:
        return []

# ── Terminal summary — ALL scenarios ──────────────────────────────────────────

print()
print("ZPressD Benchmark Summary")
print("-" * 40)

all_pswpout_reductions = []
all_psi_reductions     = []
all_latency_reductions = []
scenario_summaries     = {}

for scenario in SCENARIOS:
    base  = load_csv(RESULTS_DIR / f'{scenario}_baseline.csv')
    opt   = load_csv(RESULTS_DIR / f'{scenario}_daemon.csv')
    lat_b = load_latency(RESULTS_DIR / f'{scenario}_baseline_latency.txt')
    lat_o = load_latency(RESULTS_DIR / f'{scenario}_daemon_latency.txt')

    if not base or not opt:
        print(f"\n  [{scenario}]  (no data)")
        continue

    print(f"\n  [{scenario}]")
    s = {}

    b_swap = avg(col(base, 'pswpout_rate'))
    o_swap = avg(col(opt,  'pswpout_rate'))
    r = pct_reduction(b_swap, o_swap)
    if r is not None:
        all_pswpout_reductions.append(r)
        print(f"    Disk swap I/O:  {b_swap:.1f} → {o_swap:.1f} pages/s  ({r:+.0f}%)")
    else:
        print(f"    Disk swap I/O:  baseline=0 (no swap pressure generated)")
    s['swap'] = (b_swap, o_swap, r)

    b_zs = avg(col(base, 'zswap_stored_pages'))
    o_zs = avg(col(opt,  'zswap_stored_pages'))
    print(f"    zswap stored:   {b_zs:.0f} → {o_zs:.0f} pages")
    s['zswap'] = (b_zs, o_zs)

    b_psi = avg(col(base, 'psi_full_avg10'))
    o_psi = avg(col(opt,  'psi_full_avg10'))
    r = pct_reduction(b_psi, o_psi)
    if r is not None:
        all_psi_reductions.append(r)
        print(f"    PSI stall:      {b_psi:.2f}% → {o_psi:.2f}%  ({r:+.0f}%)")
    else:
        print(f"    PSI stall:      baseline=0 (no stall generated)")
    s['psi'] = (b_psi, o_psi, r)

    if lat_b and lat_o:
        b_lat = avg(lat_b)
        o_lat = avg(lat_o)
        r = pct_reduction(b_lat, o_lat)
        if r is not None:
            all_latency_reductions.append(r)
            print(f"    Latency:        {b_lat:.1f}ms → {o_lat:.1f}ms  ({r:+.0f}%)")
        s['latency'] = (b_lat, o_lat, r)
    else:
        s['latency'] = None

    scenario_summaries[scenario] = s

print()
print("Aggregate (across all scenarios with data)")
print("-" * 40)
swap_str = f"{avg(all_pswpout_reductions):.0f}%" if all_pswpout_reductions else "n/a"
psi_str  = f"{avg(all_psi_reductions):.0f}%"     if all_psi_reductions     else "n/a"
lat_str  = f"{avg(all_latency_reductions):.0f}%"  if all_latency_reductions  else "n/a"
print(f"  Disk swap I/O reduction:  {swap_str}")
print(f"  PSI stall reduction:      {psi_str}")
print(f"  Interactive latency Δ:    {lat_str}")
print()
print("One-liner:")
print(f"  ZPressD reduced disk swap I/O by {swap_str}, PSI stall time by {psi_str},")
print(f"  keeping interactive latency within {lat_str} of baseline.")
print()

# ── Graph helper ───────────────────────────────────────────────────────────────

def make_graph(metric_key, ylabel, title, out_filename, annotation_lines):
    """
    Produce a single clean comparison graph for the heavy scenario.
    Saves to RESULTS_DIR / out_filename.
    """
    base_h = load_csv(RESULTS_DIR / 'heavy_baseline.csv')
    opt_h  = load_csv(RESULTS_DIR / 'heavy_daemon.csv')

    if not base_h or not opt_h:
        print(f"[!] No heavy data — skipping {out_filename}")
        return

    t_b = [i * 2 for i in range(len(base_h))]   # 2-second intervals → seconds
    t_o = [i * 2 for i in range(len(opt_h))]

    vals_b = col(base_h, metric_key)
    vals_o = col(opt_h,  metric_key)

    fig, ax = plt.subplots(figsize=(12, 6), facecolor='white')
    ax.set_facecolor('white')

    # Lines
    ax.plot(t_b, vals_b, color=COLOR_BASE, linewidth=2.5,
            label='Baseline (no daemon)', zorder=3)
    ax.plot(t_o, vals_o, color=COLOR_ZP,  linewidth=2.5,
            label='ZPressD daemon',       zorder=3)

    # Shaded fill under each line
    ax.fill_between(t_b, vals_b, alpha=0.15, color=COLOR_BASE, zorder=2)
    ax.fill_between(t_o, vals_o, alpha=0.15, color=COLOR_ZP,   zorder=2)

    # Shaded difference region
    min_len = min(len(t_b), len(t_o))
    ax.fill_between(t_b[:min_len], vals_b[:min_len], vals_o[:min_len],
                    where=[b > o for b, o in zip(vals_b[:min_len], vals_o[:min_len])],
                    alpha=0.12, color='#2980b9', zorder=1,
                    label='Reduction by ZPressD')

    # Axes styling
    ax.set_title(title, fontsize=15, fontweight='bold', color='#111111', pad=14)
    ax.set_xlabel('Time (seconds)', fontsize=12, color='#333333')
    ax.set_ylabel(ylabel, fontsize=12, color='#333333')
    ax.tick_params(axis='both', labelsize=10, colors='#444444')
    ax.grid(True, linestyle='--', alpha=0.4, color='#bbbbbb')
    for spine in ax.spines.values():
        spine.set_edgecolor('#cccccc')

    legend = ax.legend(fontsize=11, framealpha=0.9, loc='upper right',
                       edgecolor='#cccccc')

    # Annotation box with stats
    ann_text = "\n".join(annotation_lines)
    ax.text(0.02, 0.97, ann_text,
            transform=ax.transAxes,
            fontsize=10, verticalalignment='top', fontfamily='monospace',
            color='#111111',
            bbox=dict(boxstyle='round,pad=0.6', facecolor='#f0faf0',
                      edgecolor=COLOR_ZP, linewidth=1.5, alpha=0.95),
            zorder=5)

    fig.tight_layout()
    out = RESULTS_DIR / out_filename
    fig.savefig(out, dpi=150, bbox_inches='tight', facecolor='white')
    print(f"Saved: {out}")
    plt.close(fig)


# ── Graph 1 — Disk Swap-Out Rate ───────────────────────────────────────────────

hs = scenario_summaries.get('heavy', {})

swap_data = hs.get('swap', (0, 0, None))
b_s, o_s, r_s = swap_data
if r_s is not None:
    ann_swap = [
        f"Heavy scenario — disk swap I/O",
        f"",
        f"Baseline avg : {b_s:.1f} pages/s",
        f"ZPressD avg  : {o_s:.1f} pages/s",
        f"",
        f"Reduction    : {r_s:.0f}%",
    ]
else:
    ann_swap = ["Heavy scenario — disk swap I/O", "", "No swap pressure observed"]

make_graph(
    metric_key   = 'pswpout_rate',
    ylabel       = 'Disk Swap-Out Rate (pages/s)',
    title        = 'ZPressD vs Baseline — Disk Swap-Out Rate (Heavy Workload)',
    out_filename = 'disk_io_comparison.png',
    annotation_lines = ann_swap,
)

# ── Graph 2 — PSI Memory Full Stall ───────────────────────────────────────────

psi_data = hs.get('psi', (0, 0, None))
b_p, o_p, r_p = psi_data
if r_p is not None:
    ann_psi = [
        f"Heavy scenario — PSI memory stall",
        f"",
        f"Baseline avg : {b_p:.2f}%",
        f"ZPressD avg  : {o_p:.2f}%",
        f"",
        f"Reduction    : {r_p:.0f}%",
    ]
else:
    ann_psi = ["Heavy scenario — PSI stall", "", "No stall observed"]

make_graph(
    metric_key   = 'psi_full_avg10',
    ylabel       = 'PSI Memory Full Stall (%)',
    title        = 'ZPressD vs Baseline — PSI Memory Full Stall (Heavy Workload)',
    out_filename = 'psi_stall_comparison.png',
    annotation_lines = ann_psi,
)