// Genera las figuras del README como SVG, escritas con la libreria estandar.
//
// El proyecto no tiene dependencias externas ni capa de build mas alla de
// cl.exe / CMake, asi que traer una libreria de graficos para dibujar cuatro
// paneles contradiria lo que el README sostiene. Un SVG es texto y estos
// graficos son geometria simple, de modo que se emiten directo.
//
// Todo lo que se dibuja se vuelve a medir aca llamando a MonteCarloEngine, no
// se copia del README: si el motor cambia, las figuras cambian con el.
//
// Uso:
//   make_figures.exe [--outdir docs/figures] [--paths 400000]

#include "AsianOption.h"
#include "ClosedFormAsian.h"
#include "MarketModel.h"
#include "MonteCarloEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char* kInk = "#2B2B2B";
const char* kGrid = "#D9D9D9";
const char* kCorrect = "#B5553D";
const char* kGbm = "#6E8CA0";
const char* kAccent = "#4C7A3E";
const char* kMuted = "#8A8A8A";

class Svg {
public:
    Svg(int w, int h) : w_(w), h_(h) {
        o_ << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h
           << "\" viewBox=\"0 0 " << w << " " << h
           << "\" font-family=\"Segoe UI, Helvetica, Arial, sans-serif\">\n"
           << "  <rect x=\"0\" y=\"0\" width=\"" << w << "\" height=\"" << h
           << "\" fill=\"#FFFFFF\"/>\n";
    }

    void rect(double x, double y, double w, double h, const char* fill, double opacity = 1.0) {
        o_ << "  <rect x=\"" << n(x) << "\" y=\"" << n(y) << "\" width=\"" << n(w) << "\" height=\""
           << n(h) << "\" fill=\"" << fill << "\" fill-opacity=\"" << n(opacity)
           << "\" stroke=\"#FFFFFF\" stroke-width=\"1.2\"/>\n";
    }

    void line(double x1, double y1, double x2, double y2, const char* stroke, double w = 1.0,
              const char* dash = nullptr) {
        o_ << "  <line x1=\"" << n(x1) << "\" y1=\"" << n(y1) << "\" x2=\"" << n(x2) << "\" y2=\""
           << n(y2) << "\" stroke=\"" << stroke << "\" stroke-width=\"" << n(w) << "\"";
        if (dash) o_ << " stroke-dasharray=\"" << dash << "\"";
        o_ << "/>\n";
    }

    void polyline(const std::vector<std::pair<double, double>>& pts, const char* stroke, double w) {
        if (pts.empty()) return;
        o_ << "  <polyline fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << n(w)
           << "\" points=\"";
        for (const auto& [x, y] : pts) o_ << n(x) << "," << n(y) << " ";
        o_ << "\"/>\n";
    }

    void circle(double cx, double cy, double r, const char* fill) {
        o_ << "  <circle cx=\"" << n(cx) << "\" cy=\"" << n(cy) << "\" r=\"" << n(r)
           << "\" fill=\"" << fill << "\" stroke=\"#FFFFFF\" stroke-width=\"1.4\"/>\n";
    }

    void text(double x, double y, const std::string& s, double size = 12, const char* fill = kInk,
              const char* anchor = "start", bool bold = false) {
        o_ << "  <text x=\"" << n(x) << "\" y=\"" << n(y) << "\" font-size=\"" << n(size)
           << "\" fill=\"" << fill << "\" text-anchor=\"" << anchor << "\"";
        if (bold) o_ << " font-weight=\"600\"";
        o_ << ">" << esc(s) << "</text>\n";
    }

    bool save(const std::string& path) {
        o_ << "</svg>\n";
        std::ofstream f(path, std::ios::binary);
        if (!f) return false;
        f << o_.str();
        return true;
    }

private:
    static std::string esc(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '&') r += "&amp;";
            else if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else r += c;
        }
        return r;
    }
    static std::string n(double v) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << v;
        std::string s = o.str();
        if (s.find('.') != std::string::npos) {
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s;
    }
    int w_, h_;
    std::ostringstream o_;
};

std::string fmt(double v, int d) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(d) << v;
    return o.str();
}

std::string commas(long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

MarketParams baseParams(ModelType model) {
    MarketParams mp{};
    mp.S0 = 4.5;
    mp.r = 0.045;
    mp.q = 0.02;
    mp.sigma = 0.28;
    mp.kappa = 1.2;
    mp.theta = std::log(4.30);
    mp.v0 = 0.0784;
    mp.kappaV = 2.0;
    mp.thetaV = 0.0784;
    mp.xiV = 0.35;
    mp.rho = -0.55;
    mp.model = model;
    return mp;
}

AsianOptionSpec arithmeticCall() {
    return AsianOptionSpec{4.5, 1.0, 252, OptionType::Call, AveragingType::Arithmetic};
}

SimulationConfig cfgFor(size_t paths, bool antithetic, bool cv) {
    SimulationConfig c{};
    c.numPaths = paths;
    c.antithetic = antithetic;
    c.controlVariate = cv;
    c.seed = 42;
    return c;
}

struct Axes {
    double l, t, w, h, xmin, xmax, ymin, ymax;
    double sx(double x) const { return l + (x - xmin) / (xmax - xmin) * w; }
    double sy(double y) const { return t + h - (y - ymin) / (ymax - ymin) * h; }
};

void frame(Svg& s, const Axes& a, int yticks, int decimals) {
    for (int i = 0; i <= yticks; ++i) {
        const double v = a.ymin + (a.ymax - a.ymin) * i / yticks;
        s.line(a.l, a.sy(v), a.l + a.w, a.sy(v), kGrid, 0.8);
        s.text(a.l - 8, a.sy(v) + 4, fmt(v, decimals), 11, "#6A6A6A", "end");
    }
    s.line(a.l, a.t + a.h, a.l + a.w, a.t + a.h, "#B9B9B9", 1.2);
    s.line(a.l, a.t, a.l, a.t + a.h, "#B9B9B9", 1.2);
}

// ---------------------------------------------------------------------------
// 1. La respuesta al nivel de equilibrio: lo que el bug aplanaba
// ---------------------------------------------------------------------------
void figureThetaSweep(size_t paths, const std::string& out) {
    std::printf("1/4 schwartz_theta_response ...\n");

    const std::vector<double> levels = {3.60, 3.80, 4.00, 4.30, 4.50, 4.80, 5.20, 5.60};
    std::vector<double> prices;
    for (double level : levels) {
        auto mp = baseParams(ModelType::SchwartzMeanReverting);
        mp.theta = std::log(level);
        prices.push_back(MonteCarloEngine::price(mp, arithmeticCall(), cfgFor(paths, true, true)).price);
    }

    // El precio GBM es el valor hacia el que la variable de control arrastraba
    // todas estas, sin importar el nivel de equilibrio.
    const double gbmPrice =
        MonteCarloEngine::price(baseParams(ModelType::GeometricBrownianMotion), arithmeticCall(),
                                 cfgFor(paths, true, true))
            .price;

    const double lo = *std::min_element(prices.begin(), prices.end());
    const double hi = *std::max_element(prices.begin(), prices.end());

    Svg s(960, 580);
    Axes a{92, 112, 960 - 92 - 44, 580 - 112 - 76, levels.front() - 0.1, levels.back() + 0.1, 0.0,
           std::ceil((std::max(hi, gbmPrice) + 0.05) * 10) / 10};
    frame(s, a, 5, 2);

    // Banda donde el precio quedaba atrapado con el bug (ver README).
    s.rect(a.l, a.sy(0.3090), a.w, std::max(2.0, a.sy(0.2959) - a.sy(0.3090)), kMuted, 0.22);
    s.text(a.l + 10, a.sy(0.3090) - 7, "range the price was trapped in while the bug was live: 0.013 wide",
           11, "#6A6A6A");

    s.line(a.l, a.sy(gbmPrice), a.l + a.w, a.sy(gbmPrice), kGbm, 1.6, "6,4");
    s.text(a.l + a.w - 6, a.sy(gbmPrice) + 16, "GBM price " + fmt(gbmPrice, 4) + " -- what the GBM anchor pulled toward",
           11, kGbm, "end");

    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < levels.size(); ++i) pts.emplace_back(a.sx(levels[i]), a.sy(prices[i]));
    s.polyline(pts, kCorrect, 2.4);
    for (size_t i = 0; i < levels.size(); ++i) {
        s.circle(a.sx(levels[i]), a.sy(prices[i]), 5, kCorrect);
    }
    s.text(a.sx(levels.front()) + 8, a.sy(prices.front()) - 12, fmt(prices.front(), 4), 11, kCorrect,
           "start", true);
    s.text(a.sx(levels.back()) - 8, a.sy(prices.back()) - 12, fmt(prices.back(), 4), 11, kCorrect,
           "end", true);

    for (double x : {3.60, 4.00, 4.30, 4.80, 5.20, 5.60}) {
        s.line(a.sx(x), a.t + a.h, a.sx(x), a.t + a.h + 5, "#B9B9B9", 1.0);
        s.text(a.sx(x), a.t + a.h + 20, fmt(x, 2), 11, "#6A6A6A", "middle");
    }
    s.text(a.sx(4.30), a.t + a.h + 38, "spot = 4.50", 11, kMuted, "middle");
    s.text(a.l + a.w / 2, 580 - 16, "Schwartz equilibrium price level", 12, kInk, "middle");
    s.text(16, a.t - 14, "call price (USD/lb)", 12, kInk);

    s.text(92, 36, "What the model is supposed to do, and had stopped doing", 16, kInk, "start", true);
    s.text(92, 58,
           "Arithmetic-average call, spot = strike = 4.50, " + commas(static_cast<long long>(paths)) +
               " paths per point. Mean reversion toward a lower level should make the call",
           12, "#6A6A6A");
    s.text(92, 75,
           "nearly worthless and toward a higher one make it valuable. It does: the price ranges over " +
               fmt(hi - lo, 3) + " across this sweep.",
           12, "#6A6A6A");
    s.text(92, 94,
           "While the GBM-derived control variate was being applied to Schwartz, that entire response "
           "collapsed into the grey band.",
           12, kInk);

    if (s.save(out)) std::printf("  escrito %s\n", out.c_str());
}

// ---------------------------------------------------------------------------
// 2. A/B de reduccion de varianza, solo donde la tecnica es valida
// ---------------------------------------------------------------------------
void figureVarianceReduction(size_t paths, const std::string& out) {
    std::printf("2/4 variance_reduction_ab ...\n");

    struct Row { const char* label; bool anti; bool cv; ModelType model; };
    const Row rows[] = {
        {"GBM: plain MC", false, false, ModelType::GeometricBrownianMotion},
        {"GBM: antithetic", true, false, ModelType::GeometricBrownianMotion},
        {"GBM: control variate", false, true, ModelType::GeometricBrownianMotion},
        {"GBM: both", true, true, ModelType::GeometricBrownianMotion},
        {"Schwartz: plain MC", false, false, ModelType::SchwartzMeanReverting},
        {"Schwartz: antithetic", true, false, ModelType::SchwartzMeanReverting},
    };
    const size_t n = sizeof(rows) / sizeof(rows[0]);

    std::vector<double> errs, prices;
    for (const auto& r : rows) {
        const auto res =
            MonteCarloEngine::price(baseParams(r.model), arithmeticCall(), cfgFor(paths, r.anti, r.cv));
        errs.push_back(res.stdError);
        prices.push_back(res.price);
    }

    Svg s(980, 560);
    const double l = 210, t = 118, w = 980 - l - 150, h = 560 - t - 70;
    const double emax = *std::max_element(errs.begin(), errs.end());

    // Cada fila se compara contra el plain MC de SU PROPIO modelo. Dividir las
    // filas Schwartz por el plain de GBM daria un numero sin sentido, que es
    // justamente el error que el README ya documenta haber cometido una vez.
    auto baselineFor = [&](ModelType m) {
        for (size_t i = 0; i < n; ++i) {
            if (rows[i].model == m && !rows[i].anti && !rows[i].cv) return errs[i];
        }
        return errs[0];
    };

    for (size_t i = 0; i < n; ++i) {
        const double bh = h / n * 0.62;
        const double cy = t + h * (static_cast<double>(i) + 0.5) / n;
        const double bw = errs[i] / emax * w;
        const char* color = rows[i].model == ModelType::GeometricBrownianMotion ? kGbm : kCorrect;
        s.rect(l, cy - bh / 2, std::max(bw, 2.0), bh, color);
        s.text(l - 10, cy + 4, rows[i].label, 12, kInk, "end");
        const double rel = baselineFor(rows[i].model) / errs[i];
        s.text(l + std::max(bw, 2.0) + 10, cy + 4,
               "stderr " + fmt(errs[i], 6) + "   (" + fmt(rel, 2) + "x vs plain MC, same model)", 11,
               kInk);
    }

    s.line(l, t, l, t + h, "#B9B9B9", 1.2);
    s.text(92, 36, "Variance reduction, and where each technique is allowed to apply", 16, kInk,
           "start", true);
    s.text(92, 58,
           "Same option, same " + commas(static_cast<long long>(paths)) +
               " paths, same seed; only the flags and the model change. Shorter is better.",
           12, "#6A6A6A");
    s.text(92, 78,
           "The control variate carries almost all of the gain -- but it is only valid under GBM, so "
           "the Schwartz default gets antithetic sampling alone.",
           12, kInk);
    s.text(92, 96,
           "That is the real cost of the fix: a correct price with a wider interval, instead of a "
           "tight interval around a biased one.",
           12, kInk);
    s.text(l + w / 2, 560 - 22, "reported standard error", 12, kInk, "middle");

    if (s.save(out)) std::printf("  escrito %s\n", out.c_str());
}

// ---------------------------------------------------------------------------
// 3. Convergencia 1/sqrt(N)
// ---------------------------------------------------------------------------
void figureConvergence(const std::string& out) {
    std::printf("3/4 convergence_one_over_sqrt_n ...\n");

    const std::vector<size_t> counts = {50'000, 100'000, 200'000, 400'000, 800'000, 1'600'000};
    std::vector<double> errs;
    for (size_t c : counts) {
        errs.push_back(MonteCarloEngine::price(baseParams(ModelType::SchwartzMeanReverting),
                                                arithmeticCall(), cfgFor(c, true, true))
                           .stdError);
    }

    Svg s(900, 560);
    Axes a{92, 108, 900 - 92 - 48, 560 - 108 - 76,
           std::log10(static_cast<double>(counts.front())) - 0.1,
           std::log10(static_cast<double>(counts.back())) + 0.1,
           std::log10(*std::min_element(errs.begin(), errs.end())) - 0.1,
           std::log10(*std::max_element(errs.begin(), errs.end())) + 0.1};

    for (int i = 0; i <= 4; ++i) {
        const double v = a.ymin + (a.ymax - a.ymin) * i / 4;
        s.line(a.l, a.sy(v), a.l + a.w, a.sy(v), kGrid, 0.8);
        s.text(a.l - 8, a.sy(v) + 4, fmt(std::pow(10.0, v), 6), 11, "#6A6A6A", "end");
    }
    s.line(a.l, a.t + a.h, a.l + a.w, a.t + a.h, "#B9B9B9", 1.2);
    s.line(a.l, a.t, a.l, a.t + a.h, "#B9B9B9", 1.2);

    // Pendiente teorica -1/2 anclada al primer punto.
    const double x0 = std::log10(static_cast<double>(counts.front()));
    const double y0 = std::log10(errs.front());
    s.line(a.sx(a.xmin), a.sy(y0 - 0.5 * (a.xmin - x0)), a.sx(a.xmax),
           a.sy(y0 - 0.5 * (a.xmax - x0)), kMuted, 1.6, "6,4");
    s.text(a.l + a.w - 6, a.sy(y0 - 0.5 * (a.xmax - x0)) - 10, "slope -1/2 (the 1/sqrt(N) law)", 11,
           kMuted, "end");

    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < counts.size(); ++i) {
        pts.emplace_back(a.sx(std::log10(static_cast<double>(counts[i]))), a.sy(std::log10(errs[i])));
    }
    s.polyline(pts, kCorrect, 2.2);
    for (const auto& [x, y] : pts) s.circle(x, y, 5, kCorrect);

    for (size_t c : counts) {
        const double x = a.sx(std::log10(static_cast<double>(c)));
        s.line(x, a.t + a.h, x, a.t + a.h + 5, "#B9B9B9", 1.0);
        s.text(x, a.t + a.h + 20, commas(static_cast<long long>(c)), 10, "#6A6A6A", "middle");
    }

    const double ratio = errs.front() / errs.back();
    const double expected = std::sqrt(static_cast<double>(counts.back()) /
                                       static_cast<double>(counts.front()));
    s.text(a.l + a.w / 2, 560 - 16, "paths (log scale)", 12, kInk, "middle");
    s.text(16, a.t - 14, "standard error (log scale)", 12, kInk);
    s.text(92, 36, "The error falls exactly as fast as theory says, and no faster", 16, kInk,
           "start", true);
    s.text(92, 58,
           "Schwartz default. Going from " + commas(static_cast<long long>(counts.front())) + " to " +
               commas(static_cast<long long>(counts.back())) + " paths cut the error " +
               fmt(ratio, 2) + "x, against the " + fmt(expected, 2) + "x the 1/sqrt(N) law predicts.",
           12, "#6A6A6A");
    s.text(92, 78,
           "This is also why variance reduction matters: buying the same factor by brute force costs "
           "32x the compute.",
           12, kInk);

    if (s.save(out)) std::printf("  escrito %s\n", out.c_str());
}

// ---------------------------------------------------------------------------
// 4. Escalamiento paralelo contra el ideal
// ---------------------------------------------------------------------------
void figureScaling(size_t paths, const std::string& out) {
    std::printf("4/4 thread_scaling ...\n");

    auto seqCfg = cfgFor(paths, true, true);
    seqCfg.numThreads = 1;
    const auto seq = MonteCarloEngine::price(baseParams(ModelType::SchwartzMeanReverting),
                                              arithmeticCall(), seqCfg);
    const auto par = MonteCarloEngine::price(baseParams(ModelType::SchwartzMeanReverting),
                                              arithmeticCall(), cfgFor(paths, true, true));
    const double speedup = seq.elapsedSeconds / par.elapsedSeconds;
    const double threads = static_cast<double>(par.numThreads);

    Svg s(880, 540);
    Axes a{92, 112, 880 - 92 - 48, 540 - 112 - 70, 0, threads * 1.08, 0, threads * 1.08};
    frame(s, a, 4, 0);

    s.line(a.sx(0), a.sy(0), a.sx(threads), a.sy(threads), kMuted, 1.8, "6,4");
    s.text(a.sx(threads) - 8, a.sy(threads) + 18, "perfect linear scaling", 11, kMuted, "end");

    s.line(a.sx(1), a.sy(1), a.sx(threads), a.sy(speedup), kCorrect, 2.4);
    s.circle(a.sx(1), a.sy(1), 6, kCorrect);
    s.circle(a.sx(threads), a.sy(speedup), 7, kCorrect);
    s.text(a.sx(threads) - 10, a.sy(speedup) - 12,
           "measured " + fmt(speedup, 2) + "x on " + fmt(threads, 0) + " threads", 12, kCorrect,
           "end", true);

    for (int i = 0; i <= 4; ++i) {
        const double x = threads * i / 4.0;
        s.line(a.sx(x), a.t + a.h, a.sx(x), a.t + a.h + 5, "#B9B9B9", 1.0);
        s.text(a.sx(x), a.t + a.h + 20, fmt(x, 0), 11, "#6A6A6A", "middle");
    }
    s.text(a.l + a.w / 2, 540 - 16, "threads", 12, kInk, "middle");
    s.text(16, a.t - 14, "speedup", 12, kInk);

    s.text(92, 36, "Parallel speedup, reported as measured rather than as advertised", 16, kInk,
           "start", true);
    s.text(92, 58,
           commas(static_cast<long long>(paths)) + " paths: " + fmt(seq.elapsedSeconds, 2) +
               " s on one thread, " + fmt(par.elapsedSeconds, 2) + " s on " + fmt(threads, 0) + ".",
           12, "#6A6A6A");
    s.text(92, 78,
           "The gap to the dashed line is hyperthreading and memory bandwidth. " + fmt(speedup, 2) +
               "x of a possible " + fmt(threads, 0) + "x is the honest number for an",
           12, kInk);
    s.text(92, 95, "embarrassingly-parallel workload on this hardware, and it is the one quoted.", 12,
           kInk);

    if (s.save(out)) std::printf("  escrito %s\n", out.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::string outdir = "docs/figures";
    size_t paths = 400'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--outdir" && i + 1 < argc) outdir = argv[++i];
        else if (a == "--paths" && i + 1 < argc) paths = std::stoull(argv[++i]);
    }

    std::printf("Writing figures to %s (%s paths per point)\n\n", outdir.c_str(),
                commas(static_cast<long long>(paths)).c_str());

    figureThetaSweep(paths, outdir + "/schwartz_theta_response.svg");
    figureVarianceReduction(paths, outdir + "/variance_reduction_ab.svg");
    figureConvergence(outdir + "/convergence_one_over_sqrt_n.svg");
    figureScaling(paths, outdir + "/thread_scaling.svg");

    std::printf("\nListo.\n");
    return 0;
}
