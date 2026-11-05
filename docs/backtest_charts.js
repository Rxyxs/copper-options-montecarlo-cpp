// Interactive backtest charts. Data comes from backtest_data.js (tools/build_interactive.py).
(function () {
  var BT = window.BT;
  if (!window.Plotly || !BT) return;  // the static figures in <noscript> and on GitHub remain
  var NAMES = {
    gbm_window: "GBM, 3-year vol", gbm_ewma: "GBM, EWMA vol", heston: "Heston",
    historical: "Block bootstrap (no model)", merton: "Merton jumps", regime_vol: "Two vol regimes",
    regime_drift: "Two regimes with drift", schwartz: "Schwartz"
  };
  var ORDER = Object.keys(NAMES);
  var css = getComputedStyle(document.documentElement);
  var ink = css.getPropertyValue("--ink").trim(), muted = css.getPropertyValue("--muted").trim();
  var line = css.getPropertyValue("--line").trim(), accent = css.getPropertyValue("--accent").trim();
  var BLUE = "#6E8CA0";
  var CONFIG = { responsive: true, displaylogo: false,
    modeBarButtonsToRemove: ["select2d", "lasso2d", "autoScale2d"] };
  function layout(extra) {
    return Object.assign({ paper_bgcolor: "rgba(0,0,0,0)", plot_bgcolor: "rgba(0,0,0,0)",
      font: { color: ink, size: 13 }, margin: { l: 56, r: 16, t: 12, b: 44 },
      hoverlabel: { font: { size: 13 } }, showlegend: false }, extra);
  }
  function axis(extra) { return Object.assign({ gridcolor: line, zerolinecolor: line, color: muted }, extra); }
  var byModel = {};
  BT.summary.forEach(function (r) { byModel[r.model] = r; });

  // 1. Realized average against the band of the chosen model.
  var select = document.getElementById("bt-model");
  ORDER.forEach(function (k) { select.add(new Option(NAMES[k], k)); });
  function drawBand() {
    var o = BT.origins[select.value], s = byModel[select.value];
    var outX = [], outY = [];
    o.date.forEach(function (d, i) {
      if (o.realized[i] < o.q05[i] || o.realized[i] > o.q95[i]) { outX.push(d); outY.push(o.realized[i]); }
    });
    document.getElementById("bt-band-note").textContent = (+s.cover90 * 100).toFixed(1) +
      "% inside the band, " + outX.length + " of " + o.date.length + " origins outside. Drag to zoom, double-click to reset.";
    Plotly.react("bt-band", [
      { x: o.date, y: o.q95, mode: "lines", line: { width: 0 }, hovertemplate: "Band top: %{y:+.1f}%<extra></extra>" },
      { x: o.date, y: o.q05, mode: "lines", line: { width: 0 }, fill: "tonexty", fillcolor: "rgba(110,140,160,.3)",
        hovertemplate: "Band bottom: %{y:+.1f}%<extra></extra>" },
      { x: o.date, y: o.realized, mode: "lines", line: { color: ink, width: 1.3 },
        hovertemplate: "Realized: %{y:+.1f}%<extra></extra>" },
      { x: outX, y: outY, mode: "markers", marker: { color: accent, size: 7 }, hoverinfo: "skip" }
    ], layout({ hovermode: "x unified", xaxis: axis({ type: "date" }),
      yaxis: axis({ ticksuffix: "%" }) }), CONFIG);
  }
  select.addEventListener("change", drawBand);
  drawBand();

  // 2. Coverage or score by model.
  var metric = document.getElementById("bt-metric");
  function drawScore() {
    var rows = ORDER.slice().reverse().map(function (k) { return byModel[k]; });
    var names = rows.map(function (r) { return NAMES[r.model]; });
    var colors = rows.map(function (r) { return r.model === "schwartz" ? accent : BLUE; });
    var cover = metric.value === "cover", x, lo, hi;
    if (cover) {
      x = rows.map(function (r) { return r.cover90 * 100; });
      lo = rows.map(function (r) { return (r.cover90 - r.cover90_lo) * 100; });
      hi = rows.map(function (r) { return (r.cover90_hi - r.cover90) * 100; });
    } else {
      x = rows.map(function (r) { return +r.crps_diff_vs_gbm_window_bps; });
      lo = rows.map(function (r) { return r.crps_diff_vs_gbm_window_bps - r.diff_lo; });
      hi = rows.map(function (r) { return r.diff_hi - r.crps_diff_vs_gbm_window_bps; });
    }
    Plotly.react("bt-score", [{ x: x, y: names, mode: "markers", marker: { size: 12, color: colors },
      error_x: { type: "data", symmetric: false, array: hi, arrayminus: lo, color: muted, thickness: 1.5 },
      customdata: rows.map(function (_, i) { return [(x[i] - lo[i]).toFixed(1), (x[i] + hi[i]).toFixed(1)]; }),
      hovertemplate: "%{y}<br>%{x:.1f} [%{customdata[0]}, %{customdata[1]}]<extra></extra>" }],
    layout({ height: 400, margin: { l: 190, r: 16, t: 12, b: 48 },
      xaxis: axis({ title: { text: cover ? "Inside the 90% band, with 95% interval (%)" : "CRPS minus GBM, bps (negative is better)" } }),
      yaxis: axis({ automargin: true }),
      shapes: [{ type: "line", x0: cover ? 90 : 0, x1: cover ? 90 : 0, yref: "paper", y0: 0, y1: 1,
        line: { color: accent, dash: "dash", width: 1.5 } }] }), CONFIG);
  }
  metric.addEventListener("change", drawScore);
  drawScore();

  // 3. Estimated reversion speed against a known truth.
  var GROUPS = { gbm: "Random walk (true 0)", schwartz_k1: "Reversion (true 1)", schwartz_k3: "Reversion (true 3)" };
  var traces = Object.keys(GROUPS).map(function (k) {
    var v = BT.validation.filter(function (r) { return r.truth === k; });
    return { x: v.map(function (r) { return r.kappa; }), y: v.map(function () { return GROUPS[k]; }), mode: "markers",
      marker: { size: 11, color: BLUE, opacity: .65 }, hovertemplate: "Synthetic series: %{x:.2f}<extra></extra>" };
  });
  traces.push({ x: [BT.kappaReal], y: ["Real LME copper"], mode: "markers", marker: { size: 15, color: accent },
    hovertemplate: "Copper, median: %{x:.2f}<extra></extra>" });
  Plotly.newPlot("bt-kappa", traces, layout({ height: 380, margin: { l: 170, r: 16, t: 12, b: 48 },
    xaxis: axis({ title: { text: "Median estimated kappa" }, rangemode: "tozero" }),
    yaxis: axis({ automargin: true, categoryorder: "array",
      categoryarray: ["Real LME copper", GROUPS.schwartz_k3, GROUPS.schwartz_k1, GROUPS.gbm] }) }), CONFIG);

  // 4. What each model charges today.
  var today = ORDER.map(function (k) { return BT.today.filter(function (r) { return r.model === k; })[0]; });
  var labels = today.map(function (r) { return NAMES[r.model]; });
  function bars(name, key, color) {
    return { type: "bar", name: name, x: labels, y: today.map(function (r) { return +r[key]; }), marker: { color: color },
      hovertemplate: name + ": %{y:.3f} USD/lb<extra>%{x}</extra>" };
  }
  Plotly.newPlot("bt-today", [bars("Put", "atm_put_usd_lb", accent), bars("Call", "atm_call_usd_lb", BLUE)],
    layout({ barmode: "group", showlegend: true, legend: { orientation: "h", y: 1.1 },
      margin: { l: 56, r: 16, t: 28, b: 96 }, xaxis: axis({ automargin: true }),
      yaxis: axis({ title: { text: "USD/lb" } }) }), CONFIG);
})();
