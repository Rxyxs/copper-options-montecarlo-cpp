// Walk-forward backtest of the path models on the real LME copper price.
//
// Every 21 trading days it re-estimates each model on the previous window only,
// simulates the arithmetic average of the next 63 fixings (a three-month
// quotational period) and scores that distribution against what happened.
//
// No option quotes are public for LME copper, so this tests the distribution
// the models assign to the average, not whether they match market prices.
//
// Usage:
//   backtest_lme.exe data/lme_copper_daily.csv [--window 756] [--paths 20000]
//                    [--outdir results] [--start 2000-01-01]

#include "AsianOption.h"
#include "Calibration.h"
#include "MarketModel.h"
#include "PathSimulator.h"
#include "RandomEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <execution>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kHorizon = 63;
constexpr size_t kStep = 21;
constexpr size_t kBlockDays = 5;
constexpr size_t kBootstrapDraws = 4000;
constexpr size_t kBootstrapBlock = 12;  // origins, about a year: spans the 3-month overlap

enum Model { kGbmWindow, kGbmEwma, kSchwartz, kHeston, kHistorical, kMerton, kRegimeVol, kRegimeDrift, kNumModels };
const char* const kModelNames[kNumModels] = {"gbm_window", "gbm_ewma", "schwartz", "heston", "historical", "merton", "regime_vol", "regime_drift"};

struct Series {
    std::vector<std::string> dates;
    std::vector<double> prices;
};

Series readSeries(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "Cannot open " << path << " (run tools/fetch_lme.py first)\n";
        std::exit(1);
    }
    Series s;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        const size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        s.dates.push_back(line.substr(0, comma));
        s.prices.push_back(std::stod(line.substr(comma + 1)));
    }
    return s;
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Every path is the mean of its fixings over the origin's spot, so scores from
// 1996 and 2026 are comparable despite a tenfold price difference.
std::vector<double> simulateAverages(const MarketParams& mp, size_t paths, uint64_t seed) {
    const double dt = 1.0 / kTradingDaysPerYear;
    FastGaussianRNG rng(seed);
    std::vector<double> out(paths), path(kHorizon + 1);
    for (size_t p = 0; p < paths; ++p) {
        if (mp.model == ModelType::Heston) simulateHestonPath(path.data(), kHorizon, dt, mp, rng);
        else simulatePath(path.data(), kHorizon, dt, mp, rng);
        out[p] = averagePrice(path.data(), kHorizon, AveragingType::Arithmetic) / mp.S0;
    }
    return out;
}

// Baseline with no model: resample the window's own returns in 5-day blocks,
// demeaned so that it carries the same zero drift as GBM.
std::vector<double> historicalAverages(const std::vector<double>& returns, size_t paths, uint64_t seed) {
    double mean = 0.0;
    for (double r : returns) mean += r;
    mean /= static_cast<double>(returns.size());
    std::mt19937_64 engine(seed);
    std::uniform_int_distribution<size_t> pick(0, returns.size() - kBlockDays);
    std::vector<double> out(paths);
    for (size_t p = 0; p < paths; ++p) {
        double logPrice = 0.0, sum = 0.0;
        size_t done = 0;
        while (done < kHorizon) {
            const size_t start = pick(engine);
            for (size_t j = 0; j < kBlockDays && done < kHorizon; ++j, ++done) {
                logPrice += returns[start + j] - mean;
                sum += std::exp(logPrice);
            }
        }
        out[p] = sum / static_cast<double>(kHorizon);
    }
    return out;
}

// Merton jump diffusion, compensated so the expected price stays at the spot.
std::vector<double> mertonAverages(const MertonFit& m, size_t paths, uint64_t seed) {
    FastGaussianRNG rng(seed);
    std::mt19937_64 engine(seed ^ 0x5bd1e995);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double k = std::exp(m.muJ + 0.5 * m.sigJ * m.sigJ) - 1.0;
    const double drift = -0.5 * m.dailyVar - m.lambda * k;
    const double sd = std::sqrt(m.dailyVar);
    std::vector<double> out(paths);
    for (size_t p = 0; p < paths; ++p) {
        double logPrice = 0.0, sum = 0.0;
        for (size_t d = 0; d < kHorizon; ++d) {
            logPrice += drift + sd * rng.next();
            if (u(engine) < m.lambda) logPrice += m.muJ + m.sigJ * rng.next();
            sum += std::exp(logPrice);
        }
        out[p] = sum / static_cast<double>(kHorizon);
    }
    return out;
}

// Starts from the filtered state. keepDrift uses each regime's estimated mean;
// otherwise each regime is a martingale and only the volatility switches.
std::vector<double> regimeAverages(const RegimeFit& f, bool keepDrift, size_t paths, uint64_t seed) {
    FastGaussianRNG rng(seed);
    std::mt19937_64 engine(seed ^ 0x5bd1e995);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double sd[2] = {std::sqrt(f.var[0]), std::sqrt(f.var[1])};
    const double mu[2] = {keepDrift ? f.mean[0] : -0.5 * f.var[0], keepDrift ? f.mean[1] : -0.5 * f.var[1]};
    std::vector<double> out(paths);
    for (size_t p = 0; p < paths; ++p) {
        int state = u(engine) < f.probHighNow ? 1 : 0;
        double logPrice = 0.0, sum = 0.0;
        for (size_t d = 0; d < kHorizon; ++d) {
            logPrice += mu[state] + sd[state] * rng.next();
            sum += std::exp(logPrice);
            if (u(engine) >= f.stay[state]) state = 1 - state;
        }
        out[p] = sum / static_cast<double>(kHorizon);
    }
    return out;
}

struct Score {
    double crps = 0, pit = 0, q05 = 0, q10 = 0, q25 = 0, q75 = 0, q90 = 0, q95 = 0;
    double putPremium = 0, callPremium = 0;
};

struct Origin {
    std::string date;
    double spot = 0, realized = 0, kappa = 0, halfLifeYears = 0, thetaGap = 0, volWindow = 0, volEwma = 0;
    Score score[kNumModels];
};

double meanPayoff(const std::vector<double>& averages, double strike, bool call) {
    double sum = 0.0;
    for (double a : averages) sum += std::max(call ? a - strike : strike - a, 0.0);
    return sum / static_cast<double>(averages.size());
}

Origin runOrigin(const Series& s, size_t t, size_t window, size_t paths) {
    Origin o;
    o.date = s.dates[t];
    o.spot = s.prices[t];
    const double* hist = &s.prices[t + 1 - window];
    // The latest origin has no outcome yet: its scores mean nothing, only its
    // distribution is read, for the fit printed at the end.
    if (t + kHorizon < s.prices.size()) {
        double sum = 0.0;
        for (size_t j = 1; j <= kHorizon; ++j) sum += s.prices[t + j];
        o.realized = sum / static_cast<double>(kHorizon) / o.spot;
    }

    const std::vector<double> returns = logReturns(hist, window);
    const double volWindow = std::sqrt(sampleVariance(returns) * kTradingDaysPerYear);
    const double volEwma = std::sqrt(ewmaVariance(returns) * kTradingDaysPerYear);
    o.volWindow = volWindow;
    o.volEwma = volEwma;

    MarketParams base{};
    base.S0 = o.spot;
    MarketParams gbm = base;
    gbm.model = ModelType::GeometricBrownianMotion;
    gbm.sigma = volWindow;
    MarketParams ewma = gbm;
    ewma.sigma = volEwma;
    MarketParams schwartz = base;
    schwartz.model = ModelType::SchwartzMeanReverting;
    fitSchwartz(hist, window, schwartz);
    o.kappa = schwartz.kappa;
    o.halfLifeYears = std::log(2.0) / schwartz.kappa;
    o.thetaGap = schwartz.theta - std::log(o.spot);
    MarketParams heston = base;
    heston.model = ModelType::Heston;
    fitHeston(hist, window, heston);
    const MertonFit merton = fitMerton(returns);
    const RegimeFit regimes = fitRegimes(returns);

    const uint64_t seed = splitmix64(static_cast<uint64_t>(t));
    for (int m = 0; m < kNumModels; ++m) {
        const uint64_t modelSeed = splitmix64(seed + static_cast<uint64_t>(m));
        std::vector<double> avg;
        switch (m) {
            case kGbmWindow: avg = simulateAverages(gbm, paths, modelSeed); break;
            case kGbmEwma: avg = simulateAverages(ewma, paths, modelSeed); break;
            case kSchwartz: avg = simulateAverages(schwartz, paths, modelSeed); break;
            case kHeston: avg = simulateAverages(heston, paths, modelSeed); break;
            case kMerton: avg = mertonAverages(merton, paths, modelSeed); break;
            case kRegimeVol: avg = regimeAverages(regimes, false, paths, modelSeed); break;
            case kRegimeDrift: avg = regimeAverages(regimes, true, paths, modelSeed); break;
            default: avg = historicalAverages(returns, paths, modelSeed); break;
        }
        std::sort(avg.begin(), avg.end());
        Score& sc = o.score[m];
        sc.crps = crpsSorted(avg, o.realized);
        sc.pit = pitSorted(avg, o.realized);
        sc.q05 = quantileSorted(avg, 0.05);
        sc.q10 = quantileSorted(avg, 0.10);
        sc.q25 = quantileSorted(avg, 0.25);
        sc.q75 = quantileSorted(avg, 0.75);
        sc.q90 = quantileSorted(avg, 0.90);
        sc.q95 = quantileSorted(avg, 0.95);
        sc.putPremium = meanPayoff(avg, 1.0, false);
        sc.callPremium = meanPayoff(avg, 1.0, true);
    }
    return o;
}

// Moving-block bootstrap. Origins 21 days apart share two thirds of their
// 63-day horizon, so treating them as independent would overstate certainty.
template <typename Stat>
std::pair<double, double> bootstrapCi(size_t n, Stat&& stat) {
    std::mt19937_64 engine(20261104);
    std::uniform_int_distribution<size_t> pick(0, n - 1);
    std::vector<double> draws(kBootstrapDraws);
    for (double& d : draws) {
        double sum = 0.0;
        size_t count = 0;
        while (count < n) {
            const size_t start = pick(engine);
            for (size_t j = 0; j < kBootstrapBlock && count < n; ++j, ++count) sum += stat((start + j) % n);
        }
        d = sum / static_cast<double>(n);
    }
    std::sort(draws.begin(), draws.end());
    return {quantileSorted(draws, 0.025), quantileSorted(draws, 0.975)};
}

template <typename Stat>
double meanOver(size_t n, Stat&& stat) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += stat(i);
    return sum / static_cast<double>(n);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: backtest_lme <csv> [--window N] [--paths N] [--outdir DIR] [--start YYYY-MM-DD]\n";
        return 1;
    }
    size_t window = 756, paths = 20000;
    std::string outdir = "results", start = "";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--window") window = std::stoul(argv[i + 1]);
        else if (a == "--paths") paths = std::stoul(argv[i + 1]);
        else if (a == "--outdir") outdir = argv[i + 1];
        else if (a == "--start") start = argv[i + 1];
    }

    const Series s = readSeries(argv[1]);
    std::vector<size_t> ts;
    for (size_t t = window - 1; t + kHorizon < s.prices.size(); t += kStep)
        if (s.dates[t] >= start) ts.push_back(t);

    std::vector<Origin> origins(ts.size());
    std::vector<size_t> idx(ts.size());
    std::iota(idx.begin(), idx.end(), size_t{0});
    std::for_each(std::execution::par, idx.begin(), idx.end(),
                  [&](size_t i) { origins[i] = runOrigin(s, ts[i], window, paths); });
    const size_t n = origins.size();

    const std::string tag = "w" + std::to_string(window);
    std::ofstream csv(outdir + "/backtest_origins_" + tag + ".csv");
    csv << std::setprecision(6)
        << "date,spot,realized,kappa,half_life_years,theta_gap,vol_window,vol_ewma,model,crps,pit,q05,q10,q25,q75,q90,q95,put_premium,call_premium\n";
    for (const Origin& o : origins)
        for (int m = 0; m < kNumModels; ++m) {
            const Score& c = o.score[m];
            csv << o.date << ',' << o.spot << ',' << o.realized << ',' << o.kappa << ',' << o.halfLifeYears << ','
                << o.thetaGap << ',' << o.volWindow << ',' << o.volEwma << ',' << kModelNames[m] << ',' << c.crps << ','
                << c.pit << ',' << c.q05 << ',' << c.q10 << ',' << c.q25 << ',' << c.q75 << ',' << c.q90 << ',' << c.q95
                << ',' << c.putPremium << ',' << c.callPremium << '\n';
        }

    std::ofstream sum(outdir + "/backtest_summary_" + tag + ".csv");
    sum << "model,n,crps_bps,crps_diff_vs_gbm_window_bps,diff_lo,diff_hi,cover50,cover80,cover90,cover90_lo,cover90_hi,"
           "below_q05,above_q95,put_premium,put_payoff,put_payoff_lo,put_payoff_hi,call_premium,call_payoff,call_payoff_lo,call_payoff_hi\n";
    sum << std::fixed << std::setprecision(5);
    auto payoffPut = [&](size_t i) { return std::max(1.0 - origins[i].realized, 0.0); };
    auto payoffCall = [&](size_t i) { return std::max(origins[i].realized - 1.0, 0.0); };
    const auto putCi = bootstrapCi(n, payoffPut);
    const auto callCi = bootstrapCi(n, payoffCall);
    std::cout << "origins " << n << " from " << origins.front().date << " to " << origins.back().date << "\n";
    for (int m = 0; m < kNumModels; ++m) {
        auto S = [&](size_t i) -> const Score& { return origins[i].score[m]; };
        auto inside = [&](size_t i, double lo, double hi) { return origins[i].realized >= lo && origins[i].realized <= hi ? 1.0 : 0.0; };
        auto c90 = [&](size_t i) { return inside(i, S(i).q05, S(i).q95); };
        auto diff = [&](size_t i) { return S(i).crps - origins[i].score[kGbmWindow].crps; };
        const double crps = meanOver(n, [&](size_t i) { return S(i).crps; });
        const double dCrps = meanOver(n, diff);
        const auto dCi = bootstrapCi(n, diff);
        const auto cCi = bootstrapCi(n, c90);
        const double cover50 = meanOver(n, [&](size_t i) { return inside(i, S(i).q25, S(i).q75); });
        const double cover80 = meanOver(n, [&](size_t i) { return inside(i, S(i).q10, S(i).q90); });
        const double cover90 = meanOver(n, c90);
        const double low = meanOver(n, [&](size_t i) { return origins[i].realized < S(i).q05 ? 1.0 : 0.0; });
        const double high = meanOver(n, [&](size_t i) { return origins[i].realized > S(i).q95 ? 1.0 : 0.0; });
        const double put = meanOver(n, [&](size_t i) { return S(i).putPremium; });
        const double call = meanOver(n, [&](size_t i) { return S(i).callPremium; });
        const double putPay = meanOver(n, payoffPut);
        const double callPay = meanOver(n, payoffCall);
        sum << kModelNames[m] << ',' << n << ',' << crps * 1e4 << ',' << dCrps * 1e4 << ',' << dCi.first * 1e4 << ','
            << dCi.second * 1e4 << ',' << cover50 << ',' << cover80 << ',' << cover90 << ',' << cCi.first << ','
            << cCi.second << ',' << low << ',' << high << ',' << put << ',' << putPay << ',' << putCi.first << ','
            << putCi.second << ',' << call << ',' << callPay << ',' << callCi.first << ',' << callCi.second << '\n';
        std::printf("%-11s crps %6.1f bps  vs gbm %+5.1f [%+5.1f,%+5.1f]  cover 50/80/90 %.3f %.3f %.3f [%.3f,%.3f]  low %.3f high %.3f  put %.4f/%.4f call %.4f/%.4f\n",
                    kModelNames[m], crps * 1e4, dCrps * 1e4, dCi.first * 1e4, dCi.second * 1e4, cover50, cover80, cover90,
                    cCi.first, cCi.second, low, high, put, putPay, call, callPay);
    }

    // What the models say today, in USD/lb, from the last window available.
    const Origin now = runOrigin(s, s.prices.size() - 1, window, paths);
    std::ofstream cur(outdir + "/current_fit_" + tag + ".csv");
    cur << std::fixed << std::setprecision(4) << "date,spot,model,atm_put_usd_lb,atm_call_usd_lb,avg_q05_usd_lb,avg_q95_usd_lb\n";
    std::cout << "today " << now.date << " spot " << now.spot << " kappa " << now.kappa << " vol window " << now.volWindow
              << " vol ewma " << now.volEwma << "\n";
    for (int m = 0; m < kNumModels; ++m) {
        const Score& c = now.score[m];
        cur << now.date << ',' << now.spot << ',' << kModelNames[m] << ',' << c.putPremium * now.spot << ','
            << c.callPremium * now.spot << ',' << c.q05 * now.spot << ',' << c.q95 * now.spot << '\n';
        std::printf("%-11s put %.4f call %.4f  avg 90%% [%.3f, %.3f]\n", kModelNames[m], c.putPremium * now.spot,
                    c.callPremium * now.spot, c.q05 * now.spot, c.q95 * now.spot);
    }
    return 0;
}
