#pragma once
#include <cmath>
#include "AsianOption.h"
#include "MarketModel.h"

// Continuous-averaging geometric Asian option price under GBM
// (Kemna & Vorst, 1990).
//
// Under risk-neutral GBM, ln S_t = ln S0 + (r - q - sigma^2/2) t + sigma W_t,
// so the continuously-monitored geometric average
//   ln G_T = (1/T) * integral_0^T ln S_t dt
// is itself normal, with
//   mean m = ln S0 + (r - q - sigma^2/2) * T/2
//   var  v^2 = sigma^2 * T / 3
// (the T/2 and T/3 factors come from integrating the linear drift and the
// variance of the running Brownian integral over [0,T]). G_T is therefore
// lognormal, so its call/put price follows the usual Black-Scholes-style
// closed form for E[max(e^{m + v*Z} - K, 0)].
//
// With numAveragingPoints in the low hundreds (e.g. daily fixings over a
// year) the true discrete-fixing price converges to this continuous-limit
// formula to within a few basis points, which is why it is accurate enough
// to serve both as (a) an independent correctness check for the whole
// engine via --self-test, and (b) the analytic anchor for the control
// variate used when pricing the arithmetic-average option.
namespace ClosedFormAsian {

inline double normalCDF(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

inline double geometricAsianPrice(const MarketParams& mp, const AsianOptionSpec& spec) {
    const double T = spec.maturity;
    const double m = std::log(mp.S0) + (mp.r - mp.q - 0.5 * mp.sigma * mp.sigma) * (T / 2.0);
    const double v = mp.sigma * std::sqrt(T / 3.0);

    const double forwardG = std::exp(m + 0.5 * v * v);
    const double d1 = (m - std::log(spec.strike) + v * v) / v;
    const double d2 = d1 - v;
    const double discount = std::exp(-mp.r * T);

    if (spec.type == OptionType::Call) {
        return discount * (forwardG * normalCDF(d1) - spec.strike * normalCDF(d2));
    }
    return discount * (spec.strike * normalCDF(-d2) - forwardG * normalCDF(-d1));
}

}  // namespace ClosedFormAsian
