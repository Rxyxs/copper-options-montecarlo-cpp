"""Draws the architecture diagram of the READMEs as hand-written SVG, in both languages.

Usage: python tools/architecture_figure.py
"""
from pathlib import Path

FIG = Path(__file__).resolve().parent.parent / "docs" / "figures"
INK, MUTED, LINE, RED, BLUE, GREEN = "#2B2B2B", "#6B645D", "#8A8A8A", "#B5553D", "#6E8CA0", "#4C7A3E"
TINT = {RED: "#F8ECE8", BLUE: "#EDF2F5", GREEN: "#EDF3EA", MUTED: "#F4F2EF"}
W, H = 1000, 640

TEXT = {
    "en": {
        "input": "Input", "cli": ["CLI arguments", "model, option, paths"],
        "market": ["Market model", "GBM, Schwartz or Heston"], "spec": ["Asian option", "strike, maturity, fixings"],
        "engine": "Pricing engine: one parallel work item per antithetic pair",
        "seed": ["Seed", "from work index"], "rng": ["Normals", "Marsaglia polar"],
        "path": ["Price path", "exact, Euler for Heston"], "pay": ["Payoff", "arithmetic + geometric"],
        "cv": ["Control variate", "GBM only: Kemna-Vorst"],
        "sum": ["Parallel reduction", "sum, sum of squares, count"], "res": ["Result", "price, std. error, 95% CI"],
        "check": "Correctness: does it compute well?",
        "c1": ["Self-test", "closed form, Heston parity"], "c2": ["3 test suites", "math, engine, calibration"],
        "bt": "Backtest: do the models describe copper?",
        "b1": ["LME price", "Cochilco, 1996 onward"], "b2": ["Calibration", "previous window only"],
        "b3": ["Average of 63 fixings", "8 models, same simulators"], "b4": ["Scores", "CRPS, coverage"],
    },
    "es": {
        "input": "Entrada", "cli": ["Argumentos CLI", "modelo, opción, trayectorias"],
        "market": ["Modelo de mercado", "GBM, Schwartz o Heston"], "spec": ["Opción asiática", "strike, plazo, fijaciones"],
        "engine": "Motor de precios: una unidad paralela por par antitético",
        "seed": ["Semilla", "según índice"], "rng": ["Normales", "polar de Marsaglia"],
        "path": ["Trayectoria", "exacta, Euler en Heston"], "pay": ["Pago", "aritmético + geométrico"],
        "cv": ["Variable de control", "solo GBM: Kemna-Vorst"],
        "sum": ["Reducción paralela", "suma, suma de cuadrados, n"], "res": ["Resultado", "precio, error est., IC 95%"],
        "check": "Corrección: ¿calcula bien?",
        "c1": ["Autotest", "forma cerrada, paridad Heston"], "c2": ["3 suites de tests", "matemática, motor, calibración"],
        "bt": "Backtest: ¿los modelos describen al cobre?",
        "b1": ["Precio LME", "Cochilco, desde 1996"], "b2": ["Calibración", "solo la ventana anterior"],
        "b3": ["Promedio de 63 fijaciones", "8 modelos, mismos simuladores"], "b4": ["Puntajes", "CRPS, cobertura"],
    },
}


def draw(lang):
    t = TEXT[lang]
    o = ['<?xml version="1.0" encoding="UTF-8"?>', f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         'font-family="Segoe UI, Helvetica, Arial, sans-serif">',
         f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>',
         f'<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
         f'<path d="M0 0L10 5L0 10z" fill="{LINE}"/></marker></defs>']

    def box(x, y, w, h, lines, color):
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="7" fill="{TINT[color]}" stroke="{color}" stroke-width="1.3"/>')
        o.append(f'<text x="{x + w / 2}" y="{y + h / 2 - 3}" font-size="13" font-weight="600" fill="{INK}" text-anchor="middle">{lines[0]}</text>')
        o.append(f'<text x="{x + w / 2}" y="{y + h / 2 + 14}" font-size="11" fill="{MUTED}" text-anchor="middle">{lines[1]}</text>')

    def arrow(*pts, dash=False):
        d = ' stroke-dasharray="5,4"' if dash else ""
        p = " ".join(f"{x},{y}" for x, y in pts)
        o.append(f'<polyline points="{p}" fill="none" stroke="{LINE}" stroke-width="1.4" marker-end="url(#a)"{d}/>')

    def band(x, y, w, h, title):
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="none" stroke="#D9D9D9"/>')
        o.append(f'<text x="{x + 14}" y="{y + 21}" font-size="12.5" font-weight="600" fill="{MUTED}">{title}</text>')

    # Entrada
    band(20, 16, 960, 112, t["input"])
    box(60, 46, 230, 66, t["cli"], MUTED)
    box(385, 46, 230, 66, t["market"], MUTED)
    box(710, 46, 230, 66, t["spec"], MUTED)
    arrow((290, 79), (385, 79))
    arrow((175, 46), (175, 34), (825, 34), (825, 46))

    # Motor
    band(20, 146, 960, 258, t["engine"])
    w, y = 166, 180
    xs = [40, 225, 410, 595, 780]
    for x, k in zip(xs, ("seed", "rng", "path", "pay", "cv")):
        box(x, y, w, 66, t[k], RED if k == "path" else BLUE)
    for a, b in zip(xs, xs[1:]):
        arrow((a + w, y + 33), (b, y + 33))
    arrow((500, 112), (500, 180))
    arrow((825, 112), (825, 150), (678, 150), (678, 180))
    box(410, 310, 250, 66, t["sum"], BLUE)
    box(720, 310, 226, 66, t["res"], GREEN)
    arrow((863, 246), (863, 278), (535, 278), (535, 310))
    arrow((660, 343), (720, 343))

    # Validacion
    band(20, 422, 960, 96, t["check"])
    box(300, 450, 250, 58, t["c1"], MUTED)
    box(600, 450, 250, 58, t["c2"], MUTED)

    # Backtest
    band(20, 534, 960, 96, t["bt"])
    bx = [40, 270, 500, 780]
    bw = [190, 190, 240, 166]
    for x, wd, k, c in zip(bx, bw, ("b1", "b2", "b3", "b4"), (GREEN, BLUE, RED, GREEN)):
        box(x, 562, wd, 58, t[k], c)
    for i in range(3):
        arrow((bx[i] + bw[i], 591), (bx[i + 1], 591))
    o.append("</svg>")
    (FIG / f"architecture_{lang}.svg").write_text("\n".join(x for x in o if x), encoding="utf-8")


if __name__ == "__main__":
    for lang in TEXT:
        draw(lang)
