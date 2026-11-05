"""Runs the backtest on synthetic series whose true model is known.

Answers two questions the real data cannot: does the scorer reward the true
model, and does Schwartz estimated on three years win even when reversion is real?

Usage: python tools/validate_backtest.py [--seeds 10]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import pandas as pd

RAIZ = Path(__file__).resolve().parent.parent
EXE = RAIZ / "bin" / "backtest_lme.exe"
N, DT, VOL = 7789, 1 / 252, 0.25


def gbm(g):
    return np.exp(np.cumsum(VOL * np.sqrt(DT) * g.standard_normal(N) - 0.5 * VOL**2 * DT))


def schwartz(g, kappa):
    x, a = np.zeros(N), np.exp(-kappa * DT)
    sd = VOL * np.sqrt((1 - a * a) / (2 * kappa))
    for i in range(1, N):
        x[i] = a * x[i - 1] + sd * g.standard_normal()
    return np.exp(x)


def regimenes(g):
    d, r, alta = VOL * np.sqrt(DT), np.zeros(N), False
    for i in range(N):
        r[i] = (0.6 if not alta else 2.0) * d * g.standard_normal()
        if g.random() < (0.01 if not alta else 0.03):
            alta = not alta
    return np.exp(np.cumsum(r))


def saltos(g):
    d = 0.8 * VOL * np.sqrt(DT)
    return np.exp(np.cumsum(d * g.standard_normal(N) + (g.random(N) < 0.01) * 0.04 * g.standard_normal(N)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=10)
    args = ap.parse_args()
    fechas = pd.bdate_range("1996-01-01", periods=N).strftime("%Y-%m-%d")
    verdades = {"gbm": gbm, "schwartz_k1": lambda g: schwartz(g, 1.0), "schwartz_k3": lambda g: schwartz(g, 3.0),
                "regimes": regimenes, "jumps": saltos}
    filas = []
    with tempfile.TemporaryDirectory() as d:
        for nombre, gen in verdades.items():
            for seed in range(args.seeds):
                f = Path(d) / "s.csv"
                pd.DataFrame({"date": fechas, "p": gen(np.random.default_rng(1000 + seed))}).to_csv(f, index=False)
                subprocess.run([EXE, f, "--outdir", d, "--paths", "8000"], check=True, capture_output=True)
                r = pd.read_csv(Path(d) / "backtest_summary_w756.csv").set_index("model")
                o = pd.read_csv(Path(d) / "backtest_origins_w756.csv")
                filas.append({"truth": nombre, "seed": seed, "median_kappa": o[o.model == "schwartz"].kappa.median(),
                              "cover90_gbm": r.cover90["gbm_window"], "cover90_schwartz": r.cover90["schwartz"],
                              "cover90_heston": r.cover90["heston"],
                              "schwartz_minus_gbm_bps": r.crps_diff_vs_gbm_window_bps["schwartz"],
                              "heston_minus_gbm_bps": r.crps_diff_vs_gbm_window_bps["heston"],
                              "merton_minus_gbm_bps": r.crps_diff_vs_gbm_window_bps["merton"],
                              "regime_vol_minus_gbm_bps": r.crps_diff_vs_gbm_window_bps["regime_vol"],
                              "regime_drift_minus_gbm_bps": r.crps_diff_vs_gbm_window_bps["regime_drift"],
                              "cover90_merton": r.cover90["merton"], "cover90_regime_vol": r.cover90["regime_vol"],
                              "cover90_regime_drift": r.cover90["regime_drift"]})
    t = pd.DataFrame(filas)
    t.to_csv(RAIZ / "results" / "validation_synthetic.csv", index=False)
    print(t.drop(columns="seed").groupby("truth").agg(["mean", "min", "max"]).round(3).T.to_string())
    print("Schwartz beats GBM in", t.assign(w=t.schwartz_minus_gbm_bps < 0).groupby("truth").w.sum().to_dict(), "of", args.seeds)


if __name__ == "__main__":
    main()
