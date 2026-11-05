"""Downloads the daily LME copper cash price (USD/lb, 1996 onward) from Cochilco.

Uses the datoschile library (pip install datoschile). Only this step and the
synthetic validation need Python; the engine and the backtest are C++.
"""
from pathlib import Path

from datoschile import cochilco

salida = Path(__file__).resolve().parent.parent / "data" / "lme_copper_daily.csv"
salida.parent.mkdir(exist_ok=True)
serie = cochilco.precio("cobre", desde=1996).dropna().rename(columns={"cobre": "usd_lb"})
serie.to_csv(salida, index=False)
print(f"{len(serie)} days, {serie.fecha.min().date()} to {serie.fecha.max().date()} -> {salida}")
