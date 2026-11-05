"""Draws the backtest figures as hand-written SVG from the CSVs in results/.

Standard library only, like tools/make_figures.cpp: these are simple geometry
and a plotting library would be the project's only dependency.

Usage: python tools/backtest_figures.py
"""
import csv
import statistics
from pathlib import Path

RAIZ = Path(__file__).resolve().parent.parent
RES, FIG = RAIZ / "results", RAIZ / "docs" / "figures"
INK, GRID, RED, BLUE, GREEN, MUTED = "#2B2B2B", "#D9D9D9", "#B5553D", "#6E8CA0", "#4C7A3E", "#8A8A8A"


class Svg:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
                  f'font-family="Segoe UI, Helvetica, Arial, sans-serif">',
                  f'<rect width="{w}" height="{h}" fill="#FFFFFF"/>']

    def text(self, x, y, s, size=12, fill=INK, anchor="start", weight="normal"):
        self.o.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
                      f'text-anchor="{anchor}" font-weight="{weight}">{s}</text>')

    def line(self, x1, y1, x2, y2, stroke=GRID, width=1.0, dash=""):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.o.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{stroke}" stroke-width="{width}"{d}/>')

    def circle(self, x, y, r, fill, opacity=1.0):
        self.o.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}" fill-opacity="{opacity}"/>')

    def poly(self, pts, fill="none", stroke="none", width=1.0, opacity=1.0):
        p = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        self.o.append(f'<polyline points="{p}" fill="{fill}" stroke="{stroke}" stroke-width="{width}" fill-opacity="{opacity}"/>')

    def save(self, name):
        (FIG / name).write_text("\n".join(self.o + ["</svg>"]), encoding="utf-8")


def filas(nombre):
    with open(RES / nombre, encoding="utf-8") as f:
        return list(csv.DictReader(f))


def banda():
    o = [r for r in filas("backtest_origins_w756.csv") if r["model"] == "gbm_window"]
    W, H, L, R, T, B = 900, 400, 58, 20, 70, 42
    a = lambda d: int(d[:4]) + (int(d[5:7]) - 1) / 12
    x0, x1 = a(o[0]["date"]), a(o[-1]["date"])
    lo, hi = -45, 45
    X = lambda v: L + (a(v) - x0) / (x1 - x0) * (W - L - R)
    Y = lambda v: T + (hi - v) / (hi - lo) * (H - T - B)
    pct = lambda v: (float(v) - 1) * 100
    s = Svg(W, H)
    s.text(L, 26, "Three-month average against the 90% band the model gave beforehand", 16, weight="600")
    fuera = sum(not (float(r["q05"]) <= float(r["realized"]) <= float(r["q95"])) for r in o)
    arriba = sum(float(r["realized"]) > float(r["q95"]) for r in o)
    s.text(L, 47, f"GBM on a trailing 3-year window. {len(o)} origins, {fuera} outside the band, {arriba} of them above it. Nominal: 5% each side.", 12, MUTED)
    for v in range(-40, 41, 10):
        s.line(L, Y(v), W - R, Y(v))
        s.text(L - 8, Y(v) + 4, f"{v:+d}%", 11, MUTED, "end")
    for y in range(2000, 2027, 5):
        px = L + (y - x0) / (x1 - x0) * (W - L - R)
        s.text(px, H - B + 20, str(y), 11, MUTED, "middle")
    pts = [(X(r["date"]), Y(pct(r["q95"]))) for r in o] + [(X(r["date"]), Y(pct(r["q05"]))) for r in reversed(o)]
    s.poly(pts + pts[:1], fill=BLUE, opacity=0.28)
    s.poly([(X(r["date"]), Y(pct(r["realized"]))) for r in o], stroke=INK, width=1.1)
    for r in o:
        if not (float(r["q05"]) <= float(r["realized"]) <= float(r["q95"])):
            s.circle(X(r["date"]), Y(pct(r["realized"])), 3.2, RED)
    s.save("backtest_band.svg")


def cobertura():
    r = {f["model"]: f for f in filas("backtest_summary_w756.csv")}
    orden = [("gbm_window", "GBM, 3-year vol"), ("gbm_ewma", "GBM, EWMA vol"), ("heston", "Heston"),
             ("historical", "Block bootstrap (no model)"), ("merton", "Merton jumps"), ("regime_vol", "Two vol regimes"),
             ("regime_drift", "Two regimes with drift"), ("schwartz", "Schwartz")]
    W, H, L, R, T = 900, 420, 210, 110, 70
    lo, hi = 0.70, 1.0
    X = lambda v: L + (v - lo) / (hi - lo) * (W - L - R)
    s = Svg(W, H)
    s.text(20, 26, "How often the realized average fell inside the 90% band", 16, weight="600")
    s.text(20, 47, "Real LME copper, 1998-2026. Bars: 95% block-bootstrap interval, needed because the 63-day horizons overlap.", 12, MUTED)
    for v in (0.7, 0.8, 0.9, 1.0):
        s.line(X(v), T - 6, X(v), H - 36, RED if v == 0.9 else GRID, 1.4 if v == 0.9 else 1, "5,4" if v == 0.9 else "")
        s.text(X(v), H - 16, f"{v:.0%}", 11, RED if v == 0.9 else MUTED, "middle")
    for i, (k, nombre) in enumerate(orden):
        y = T + 16 + i * 38
        c = float(r[k]["cover90"])
        s.text(L - 12, y + 4, nombre, 12.5, INK, "end")
        s.line(X(float(r[k]["cover90_lo"])), y, X(float(r[k]["cover90_hi"])), y, INK, 2)
        s.circle(X(c), y, 6, RED if k == "schwartz" else BLUE)
        s.text(W - R + 12, y + 4, f"{c:.1%}", 12.5, INK)
    s.save("backtest_coverage.svg")


def kappa():
    v = filas("validation_synthetic.csv")
    real = statistics.median(float(f["kappa"]) for f in filas("backtest_origins_w756.csv") if f["model"] == "schwartz")
    W, H, L, R, T = 900, 300, 210, 40, 70
    lo, hi = 0.0, 5.5
    X = lambda k: L + (k - lo) / (hi - lo) * (W - L - R)
    s = Svg(W, H)
    s.text(20, 26, "Median mean-reversion speed estimated from three-year windows", 16, weight="600")
    s.text(20, 47, "Each dot is one synthetic 30-year series with a known truth. The window finds reversion in a random walk too.", 12, MUTED)
    for k in range(0, 6):
        s.line(X(k), T - 6, X(k), H - 36)
        s.text(X(k), H - 16, f"{k}", 11, MUTED, "middle")
    grupos = [("gbm", "Random walk (true &#954; = 0)", BLUE, 0), ("schwartz_k1", "Reversion, true &#954; = 1", GREEN, 1),
              ("schwartz_k3", "Reversion, true &#954; = 3", GREEN, 3)]
    for i, (k, nombre, color, verdad) in enumerate(grupos):
        y = T + 16 + i * 48
        s.text(L - 12, y + 4, nombre, 12.5, INK, "end")
        s.line(X(verdad), y - 11, X(verdad), y + 11, INK, 1.6)
        for f in v:
            if f["truth"] == k:
                s.circle(X(float(f["median_kappa"])), y, 5, color, 0.6)
    y = T + 16 + 3 * 48
    s.text(L - 12, y + 4, "Real LME copper", 12.5, INK, "end", "600")
    s.circle(X(real), y, 7, RED)
    s.text(X(real) + 12, y + 4, f"{real:.2f}, inside the random-walk range".replace(".", ".", 1), 12.5, RED)
    s.save("backtest_kappa.svg")


if __name__ == "__main__":
    banda()
    cobertura()
    kappa()
    print("3 figures in", FIG)
