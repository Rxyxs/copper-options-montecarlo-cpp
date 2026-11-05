"""Writes docs/backtest_data.js, the data behind the interactive charts of the page.

Read from results/, so the page shows exactly what the backtest measured.
Usage: python tools/build_interactive.py
"""
import csv
import json
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
RES = RAIZ / "results"


def filas(nombre):
    with open(RES / nombre, encoding="utf-8") as f:
        return list(csv.DictReader(f))


origenes = {}
kappa_real = []
for r in filas("backtest_origins_w756.csv"):
    m = origenes.setdefault(r["model"], {"date": [], "realized": [], "q05": [], "q95": []})
    m["date"].append(r["date"])
    for k in ("realized", "q05", "q95"):
        m[k].append(round((float(r[k]) - 1) * 100, 2))
    if r["model"] == "schwartz":
        kappa_real.append(float(r["kappa"]))

datos = {
    "origins": origenes,
    "summary": filas("backtest_summary_w756.csv"),
    "today": filas("current_fit_w756.csv"),
    "validation": [{"truth": r["truth"], "kappa": round(float(r["median_kappa"]), 3)} for r in filas("validation_synthetic.csv")],
    "kappaReal": sorted(kappa_real)[len(kappa_real) // 2],
}
(RAIZ / "docs" / "backtest_data.js").write_text("window.BT=" + json.dumps(datos, separators=(",", ":")) + ";\n", encoding="utf-8")
