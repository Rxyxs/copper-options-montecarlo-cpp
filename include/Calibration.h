#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "MarketModel.h"

// Estimators used by the walk-forward backtest (tools/backtest_lme.cpp). Each
// one sees only the window of history it is handed, so feeding it the prices
// up to day t cannot leak anything from after t.
//
// They estimate under the *observed* measure. Without market option quotes
// there is nothing to calibrate a risk-neutral measure to, so r and q stay at
// zero: the expected GBM and Heston price equals today's price.

constexpr double kTradingDaysPerYear = 252.0;

inline std::vector<double> logReturns(const double* prices, size_t n) {
    std::vector<double> r;
    if (n < 2) return r;
    r.reserve(n - 1);
    for (size_t i = 1; i < n; ++i) r.push_back(std::log(prices[i] / prices[i - 1]));
    return r;
}

inline double sampleVariance(const std::vector<double>& x) {
    if (x.size() < 2) return 0.0;
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= static_cast<double>(x.size());
    double ss = 0.0;
    for (double v : x) ss += (v - mean) * (v - mean);
    return ss / static_cast<double>(x.size() - 1);
}

// RiskMetrics-style exponentially weighted daily variance, seeded with the
// window's own variance so the start of the window does not dominate.
inline double ewmaVariance(const std::vector<double>& r, double lambda = 0.94) {
    double v = sampleVariance(r);
    for (double x : r) v = lambda * v + (1.0 - lambda) * x * x;
    return v;
}

struct Ar1Fit {
    double intercept;
    double slope;
    double residualVariance;
};

// Ordinary least squares of x[t+1] on x[t].
inline Ar1Fit fitAr1(const std::vector<double>& x) {
    const size_t n = x.size() - 1;
    double mx = 0.0, my = 0.0;
    for (size_t i = 0; i < n; ++i) {
        mx += x[i];
        my += x[i + 1];
    }
    mx /= static_cast<double>(n);
    my /= static_cast<double>(n);
    double sxx = 0.0, sxy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sxx += (x[i] - mx) * (x[i] - mx);
        sxy += (x[i] - mx) * (x[i + 1] - my);
    }
    const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
    const double intercept = my - slope * mx;
    double sse = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double e = x[i + 1] - intercept - slope * x[i];
        sse += e * e;
    }
    return {intercept, slope, n > 2 ? sse / static_cast<double>(n - 2) : 0.0};
}

// Below this speed the Schwartz transition divides by nearly zero; it also
// corresponds to a half-life of about 70 years, indistinguishable from none.
constexpr double kMinKappa = 0.01;

// Discretized OU on ln(S): slope = exp(-kappa dt). A window that trends gives
// slope >= 1, meaning no measurable reversion, and kappa is floored there.
inline void fitSchwartz(const double* prices, size_t n, MarketParams& mp) {
    constexpr double dt = 1.0 / kTradingDaysPerYear;
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = std::log(prices[i]);
    const Ar1Fit fit = fitAr1(x);
    const double slope = std::min(fit.slope, std::exp(-kMinKappa * dt));
    mp.kappa = -std::log(slope) / dt;
    double sampleMean = 0.0;
    for (double v : x) sampleMean += v;
    sampleMean /= static_cast<double>(n);
    // With the slope floored the OLS intercept no longer pins the level, so
    // theta falls back on the window mean, where a weak pull does little harm.
    mp.theta = fit.slope < slope ? fit.intercept / (1.0 - fit.slope) : sampleMean;
    mp.sigma = std::sqrt(fit.residualVariance * 2.0 * mp.kappa / (1.0 - slope * slope));
}

// Moment-matched Heston: v0 from EWMA, long-run variance from the window, the
// variance dynamics from an AR(1) on rolling realized variance, and rho from
// its changes against returns. Rough on purpose, and it stays on the data's
// side: a likelihood fit needs option prices or a filter this project lacks.
inline void fitHeston(const double* prices, size_t n, MarketParams& mp, size_t rvWindow = 21) {
    const std::vector<double> r = logReturns(prices, n);
    mp.v0 = ewmaVariance(r) * kTradingDaysPerYear;
    mp.thetaV = sampleVariance(r) * kTradingDaysPerYear;

    // Non-overlapping blocks: overlapping ones would make the AR(1) slope
    // measure the overlap instead of how persistent the variance is.
    std::vector<double> rv, blockReturn;
    for (size_t i = 0; i + rvWindow <= r.size(); i += rvWindow) {
        double ss = 0.0, sum = 0.0;
        for (size_t j = i; j < i + rvWindow; ++j) {
            ss += r[j] * r[j];
            sum += r[j];
        }
        rv.push_back(ss / static_cast<double>(rvWindow) * kTradingDaysPerYear);
        blockReturn.push_back(sum);
    }
    const double dtBlock = static_cast<double>(rvWindow) / kTradingDaysPerYear;
    const Ar1Fit fit = fitAr1(rv);
    const double slope = std::clamp(fit.slope, 0.05, 0.99);
    mp.kappaV = -std::log(slope) / dtBlock;

    double meanV = 0.0;
    for (double v : rv) meanV += v;
    meanV /= static_cast<double>(rv.size());
    // Residual variance of a square-root process over dt is about xi^2 v dt.
    // Sampling noise in realized variance inflates it, so this is an upper bound.
    mp.xiV = std::sqrt(std::max(fit.residualVariance, 0.0) / std::max(meanV * dtBlock, 1e-12));

    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (size_t i = 1; i < rv.size(); ++i) {
        const double dv = rv[i] - rv[i - 1];
        sxy += blockReturn[i] * dv;
        sxx += blockReturn[i] * blockReturn[i];
        syy += dv * dv;
    }
    mp.rho = (sxx > 0.0 && syy > 0.0) ? std::clamp(sxy / std::sqrt(sxx * syy), -0.95, 0.95) : 0.0;
}

struct MertonFit {
    double dailyVar;  // diffusive variance per day
    double lambda;    // jumps per day
    double muJ, sigJ;
};

// Threshold estimator: returns beyond 3 robustly re-estimated standard deviations
// count as jumps. A normal series still flags about 0.27% of days, so this
// overstates jumps a little; with fewer than two flagged there is nothing to fit.
inline MertonFit fitMerton(const std::vector<double>& r) {
    double var = sampleVariance(r);
    std::vector<double> jumps;
    for (int iter = 0; iter < 5; ++iter) {
        std::vector<double> calm;
        jumps.clear();
        const double cut = 3.0 * std::sqrt(var);
        for (double x : r) (std::abs(x) > cut ? jumps : calm).push_back(x);
        if (calm.size() < 2) break;
        var = sampleVariance(calm);
    }
    if (jumps.size() < 2) return {sampleVariance(r), 0.0, 0.0, 0.0};
    double mean = 0.0;
    for (double x : jumps) mean += x;
    mean /= static_cast<double>(jumps.size());
    return {var, static_cast<double>(jumps.size()) / static_cast<double>(r.size()), mean,
            std::sqrt(sampleVariance(jumps))};
}

struct RegimeFit {
    double mean[2], var[2];    // per day; state 0 is the calm one
    double stay[2];            // probability of staying in each state
    double probHighNow;        // filtered probability of state 1 on the last day
};

// Two-state Gaussian hidden Markov model fitted with Baum-Welch.
inline RegimeFit fitRegimes(const std::vector<double>& r, int iterations = 60) {
    const size_t n = r.size();
    const double v = sampleVariance(r);
    RegimeFit f{{0.0, 0.0}, {0.5 * v, 2.0 * v}, {0.97, 0.90}, 0.5};
    std::vector<double> a0(n), a1(n), b0(n), b1(n), e0(n), e1(n), c(n);
    constexpr double kPi = 3.14159265358979;
    for (int it = 0; it < iterations; ++it) {
        for (size_t t = 0; t < n; ++t) {
            const double d0 = r[t] - f.mean[0], d1 = r[t] - f.mean[1];
            e0[t] = std::exp(-0.5 * d0 * d0 / f.var[0]) / std::sqrt(2.0 * kPi * f.var[0]);
            e1[t] = std::exp(-0.5 * d1 * d1 / f.var[1]) / std::sqrt(2.0 * kPi * f.var[1]);
        }
        double p0 = 0.5, p1 = 0.5;
        for (size_t t = 0; t < n; ++t) {
            a0[t] = p0 * e0[t];
            a1[t] = p1 * e1[t];
            c[t] = std::max(a0[t] + a1[t], 1e-300);
            a0[t] /= c[t];
            a1[t] /= c[t];
            p0 = a0[t] * f.stay[0] + a1[t] * (1.0 - f.stay[1]);
            p1 = a0[t] * (1.0 - f.stay[0]) + a1[t] * f.stay[1];
        }
        f.probHighNow = a1[n - 1];
        b0[n - 1] = b1[n - 1] = 1.0;
        for (size_t t = n - 1; t-- > 0;) {
            b0[t] = (f.stay[0] * e0[t + 1] * b0[t + 1] + (1.0 - f.stay[0]) * e1[t + 1] * b1[t + 1]) / c[t + 1];
            b1[t] = ((1.0 - f.stay[1]) * e0[t + 1] * b0[t + 1] + f.stay[1] * e1[t + 1] * b1[t + 1]) / c[t + 1];
        }
        double w0 = 0, w1 = 0, m0 = 0, m1 = 0, s0 = 0, s1 = 0, x00 = 0, x01 = 0, x10 = 0, x11 = 0;
        for (size_t t = 0; t < n; ++t) {
            const double g0 = a0[t] * b0[t], g1 = a1[t] * b1[t], g = std::max(g0 + g1, 1e-300);
            w0 += g0 / g;
            w1 += g1 / g;
            m0 += g0 / g * r[t];
            m1 += g1 / g * r[t];
            s0 += g0 / g * r[t] * r[t];
            s1 += g1 / g * r[t] * r[t];
            if (t + 1 < n) {
                const double scale = 1.0 / c[t + 1];
                x00 += a0[t] * f.stay[0] * e0[t + 1] * b0[t + 1] * scale;
                x01 += a0[t] * (1.0 - f.stay[0]) * e1[t + 1] * b1[t + 1] * scale;
                x10 += a1[t] * (1.0 - f.stay[1]) * e0[t + 1] * b0[t + 1] * scale;
                x11 += a1[t] * f.stay[1] * e1[t + 1] * b1[t + 1] * scale;
            }
        }
        f.mean[0] = m0 / std::max(w0, 1e-9);
        f.mean[1] = m1 / std::max(w1, 1e-9);
        // The floor stops a state from collapsing onto a handful of days.
        f.var[0] = std::max(s0 / std::max(w0, 1e-9) - f.mean[0] * f.mean[0], 0.05 * v);
        f.var[1] = std::max(s1 / std::max(w1, 1e-9) - f.mean[1] * f.mean[1], 0.05 * v);
        f.stay[0] = std::clamp(x00 / std::max(x00 + x01, 1e-12), 0.5, 0.999);
        f.stay[1] = std::clamp(x11 / std::max(x10 + x11, 1e-12), 0.5, 0.999);
    }
    if (f.var[0] > f.var[1]) {
        std::swap(f.mean[0], f.mean[1]);
        std::swap(f.var[0], f.var[1]);
        std::swap(f.stay[0], f.stay[1]);
        f.probHighNow = 1.0 - f.probHighNow;
    }
    return f;
}

// CRPS of a *sorted* sample against the realized value y, the proper score for
// a whole predictive distribution: E|X - y| - E|X - X'| / 2. Lower is better.
inline double crpsSorted(const std::vector<double>& sorted, double y) {
    const size_t n = sorted.size();
    const double dn = static_cast<double>(n);
    double absError = 0.0, spread = 0.0;
    for (size_t i = 0; i < n; ++i) {
        absError += std::abs(sorted[i] - y);
        spread += (2.0 * static_cast<double>(i + 1) - dn - 1.0) * sorted[i];
    }
    return absError / dn - spread / (dn * dn);
}

// Share of the predictive sample below y. Calibrated forecasts give uniform values.
inline double pitSorted(const std::vector<double>& sorted, double y) {
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), y);
    return static_cast<double>(it - sorted.begin()) / static_cast<double>(sorted.size());
}

inline double quantileSorted(const std::vector<double>& sorted, double p) {
    const double pos = p * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (pos - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}
