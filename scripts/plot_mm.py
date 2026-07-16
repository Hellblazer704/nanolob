#!/usr/bin/env python3
"""Plots for the market-making simulation (phase 4).

Reads results/{as,naive}_{timeseries,fills}.csv written by nanolob_mmsim and
produces three PNGs in results/:

  pnl_decomposition.png   total / spread-capture / inventory PnL, both strategies
  inventory.png           inventory paths + mid price
  spread_vs_vol.png       quoted half-spread vs realized volatility regime

Usage: python scripts/plot_mm.py [--results-dir results]
"""

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

AS_COLOR = "#1f77b4"
NAIVE_COLOR = "#d62728"


def load(results: Path, prefix: str):
    ts = pd.read_csv(results / f"{prefix}_timeseries.csv")
    fills = pd.read_csv(results / f"{prefix}_fills.csv")
    t0 = ts["ts_ms"].iloc[0]
    ts["t_min"] = (ts["ts_ms"] - t0) / 60000.0
    if not fills.empty:
        fills["t_min"] = (fills["ts_ms"] - t0) / 60000.0
    return ts, fills


def plot_pnl(as_ts, naive_ts, out: Path):
    fig, axes = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
    for ax, col, title in zip(
        axes,
        ["pnl_usdt", "spread_pnl_usdt", "inv_pnl_usdt"],
        ["Total PnL", "Spread capture", "Inventory / adverse-selection PnL"],
    ):
        ax.plot(as_ts["t_min"], as_ts[col], color=AS_COLOR, label="Avellaneda-Stoikov")
        ax.plot(naive_ts["t_min"], naive_ts[col], color=NAIVE_COLOR, label="Naive fixed spread")
        ax.axhline(0, color="gray", lw=0.6)
        ax.set_title(title)
        ax.set_ylabel("USDT")
        ax.grid(alpha=0.3)
    axes[0].legend()
    axes[-1].set_xlabel("minutes")
    fig.suptitle("PnL decomposition: total = spread capture + inventory PnL", y=0.995)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_inventory(as_ts, naive_ts, out: Path):
    fig, (ax_inv, ax_mid) = plt.subplots(
        2, 1, figsize=(11, 7), sharex=True, gridspec_kw={"height_ratios": [2, 1]}
    )
    ax_inv.plot(as_ts["t_min"], as_ts["inventory_lots"] * 1e-5, color=AS_COLOR,
                label="Avellaneda-Stoikov")
    ax_inv.plot(naive_ts["t_min"], naive_ts["inventory_lots"] * 1e-5, color=NAIVE_COLOR,
                label="Naive fixed spread")
    ax_inv.axhline(0, color="gray", lw=0.6)
    ax_inv.set_ylabel("inventory (BTC)")
    ax_inv.set_title("Inventory path")
    ax_inv.legend()
    ax_inv.grid(alpha=0.3)

    ax_mid.plot(as_ts["t_min"], as_ts["mid_ticks"] * 0.01, color="black", lw=0.8)
    ax_mid.set_ylabel("mid (USDT)")
    ax_mid.set_xlabel("minutes")
    ax_mid.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_spread_vs_vol(as_ts, naive_ts, out: Path):
    def quoted_half_spread(ts):
        quoting = (ts["bid_px"] > 0) & (ts["ask_px"] > 0)
        hs = (ts["ask_px"] - ts["bid_px"]) / 2.0
        return ts["t_min"], hs.where(quoting), ts["sigma_ticks_per_sqrt_s"]

    t_as, hs_as, sigma = quoted_half_spread(as_ts)
    t_nv, hs_nv, _ = quoted_half_spread(naive_ts)

    fig, (ax_sig, ax_hs, ax_sc) = plt.subplots(3, 1, figsize=(11, 10))
    ax_sig.plot(t_as, sigma, color="purple", lw=0.8)
    ax_sig.set_ylabel("sigma (ticks/sqrt s)")
    ax_sig.set_title("Realized volatility (EWMA)")
    ax_sig.grid(alpha=0.3)

    ax_hs.plot(t_as, hs_as, color=AS_COLOR, lw=0.9, label="A-S quoted half-spread")
    ax_hs.plot(t_nv, hs_nv, color=NAIVE_COLOR, lw=0.9, label="naive quoted half-spread")
    ax_hs.set_ylabel("half-spread (ticks)")
    ax_hs.set_xlabel("minutes")
    ax_hs.legend()
    ax_hs.grid(alpha=0.3)

    # Regime scatter: tercile the sigma distribution, show A-S spread response.
    df = pd.DataFrame({"sigma": sigma, "hs": hs_as}).dropna()
    if len(df) > 10:
        terciles = df["sigma"].quantile([1 / 3, 2 / 3]).values
        regime = np.digitize(df["sigma"], terciles)
        colors = ["#2ca02c", "#ff7f0e", "#d62728"]
        names = ["low vol", "mid vol", "high vol"]
        for r in range(3):
            sel = regime == r
            ax_sc.scatter(df["sigma"][sel], df["hs"][sel], s=6, alpha=0.4,
                          color=colors[r], label=f"{names[r]} (mean {df['hs'][sel].mean():.2f})")
        ax_sc.set_xlabel("sigma (ticks/sqrt s)")
        ax_sc.set_ylabel("A-S half-spread (ticks)")
        ax_sc.set_title("A-S quoted spread vs volatility regime")
        ax_sc.legend()
        ax_sc.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-dir", default="results")
    args = ap.parse_args()
    results = Path(args.results_dir)

    as_ts, as_fills = load(results, "as")
    naive_ts, naive_fills = load(results, "naive")

    plot_pnl(as_ts, naive_ts, results / "pnl_decomposition.png")
    plot_inventory(as_ts, naive_ts, results / "inventory.png")
    plot_spread_vs_vol(as_ts, naive_ts, results / "spread_vs_vol.png")

    print(f"A-S:   {len(as_fills)} fills, final pnl {as_ts['pnl_usdt'].iloc[-1]:+.4f} USDT "
          f"(spread {as_ts['spread_pnl_usdt'].iloc[-1]:+.4f}, "
          f"inventory {as_ts['inv_pnl_usdt'].iloc[-1]:+.4f})")
    print(f"naive: {len(naive_fills)} fills, final pnl {naive_ts['pnl_usdt'].iloc[-1]:+.4f} USDT "
          f"(spread {naive_ts['spread_pnl_usdt'].iloc[-1]:+.4f}, "
          f"inventory {naive_ts['inv_pnl_usdt'].iloc[-1]:+.4f})")
    print(f"wrote 3 plots to {results}/")


if __name__ == "__main__":
    main()
