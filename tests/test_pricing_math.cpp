// Unit tests for the deterministic pricing math: averaging, payoffs, and the
// Kemna-Vorst closed form. No Monte Carlo here -- every expected value below is
// either exact arithmetic or a textbook identity, so these tests fail loudly on
// a sign error or an off-by-one in indexing rather than hiding inside sampling
// noise the way an end-to-end price comparison would.
#include <cmath>
#include "AsianOption.h"
#include "ClosedFormAsian.h"
#include "MarketModel.h"
#include "test_framework.h"

namespace {

MarketParams gbmParams() {
    MarketParams mp{};
    mp.S0 = 4.5;
    mp.r = 0.045;
    mp.q = 0.0;
    mp.sigma = 0.28;
    mp.kappa = 1.0;
    mp.theta = 1.5;
    mp.model = ModelType::GeometricBrownianMotion;
    return mp;
}

}  // namespace

// path[0] is the t=0 spot and must NOT be averaged -- only path[1..N] are
// fixings. A version that accidentally included path[0] would produce 1.5 here
// instead of 2.0, which is exactly the kind of off-by-one this pins down.
TEST(arithmetic_average_uses_fixings_only) {
    const double path[] = {0.0, 1.0, 2.0, 3.0};
    CHECK_NEAR(averagePrice(path, 3, AveragingType::Arithmetic), 2.0, 1e-12);
}

TEST(geometric_average_uses_fixings_only) {
    // Geometric mean of 1, 2, 4 is cbrt(8) = 2 exactly.
    const double path[] = {0.0, 1.0, 2.0, 4.0};
    CHECK_NEAR(averagePrice(path, 3, AveragingType::Geometric), 2.0, 1e-12);
}

TEST(both_averages_agree_on_a_constant_path) {
    const double path[] = {0.0, 3.7, 3.7, 3.7, 3.7};
    CHECK_NEAR(averagePrice(path, 4, AveragingType::Arithmetic), 3.7, 1e-12);
    CHECK_NEAR(averagePrice(path, 4, AveragingType::Geometric), 3.7, 1e-12);
}

// AM-GM: the geometric mean of positive numbers never exceeds the arithmetic
// mean, with equality only when every value is identical (covered above).
TEST(geometric_average_is_below_arithmetic_for_a_varying_path) {
    const double path[] = {0.0, 1.0, 5.0, 2.0, 9.0, 3.0};
    const double arith = averagePrice(path, 5, AveragingType::Arithmetic);
    const double geo = averagePrice(path, 5, AveragingType::Geometric);
    CHECK_MSG(geo < arith, "geometric mean should be strictly below arithmetic here");
}

TEST(call_payoff_is_intrinsic_and_clamped_at_zero) {
    AsianOptionSpec spec{8.0, 1.0, 1, OptionType::Call, AveragingType::Arithmetic};
    CHECK_NEAR(payoff(10.0, spec), 2.0, 1e-12);
    CHECK_NEAR(payoff(8.0, spec), 0.0, 1e-12);
    CHECK_NEAR(payoff(6.0, spec), 0.0, 1e-12);  // out of the money: clamped, never negative
}

TEST(put_payoff_is_intrinsic_and_clamped_at_zero) {
    AsianOptionSpec spec{8.0, 1.0, 1, OptionType::Put, AveragingType::Arithmetic};
    CHECK_NEAR(payoff(6.0, spec), 2.0, 1e-12);
    CHECK_NEAR(payoff(8.0, spec), 0.0, 1e-12);
    CHECK_NEAR(payoff(10.0, spec), 0.0, 1e-12);
}

TEST(normal_cdf_matches_known_values) {
    CHECK_NEAR(ClosedFormAsian::normalCDF(0.0), 0.5, 1e-12);
    CHECK_NEAR(ClosedFormAsian::normalCDF(1.959964), 0.975, 1e-6);
    CHECK_NEAR(ClosedFormAsian::normalCDF(-1.959964), 0.025, 1e-6);
    CHECK_NEAR(ClosedFormAsian::normalCDF(6.0), 1.0, 1e-8);
    CHECK_NEAR(ClosedFormAsian::normalCDF(-6.0), 0.0, 1e-8);
}

TEST(normal_cdf_is_symmetric) {
    for (double x : {0.3, 1.1, 2.4, 3.9}) {
        CHECK_NEAR(ClosedFormAsian::normalCDF(x) + ClosedFormAsian::normalCDF(-x), 1.0, 1e-12);
    }
}

// Put-call parity for the geometric-average option: C - P = e^{-rT}(E[G_T] - K),
// where E[G_T] = exp(m + v^2/2) is the forward of the lognormal geometric
// average. The call and put branches of geometricAsianPrice() are written
// separately with different CDF arguments, so this catches a sign or d1/d2
// mix-up in either branch.
TEST(closed_form_satisfies_put_call_parity) {
    const MarketParams mp = gbmParams();
    for (double strike : {3.0, 4.5, 6.0}) {
        AsianOptionSpec call{strike, 1.0, 252, OptionType::Call, AveragingType::Geometric};
        AsianOptionSpec put = call;
        put.type = OptionType::Put;

        const double T = call.maturity;
        const double m = std::log(mp.S0) + (mp.r - mp.q - 0.5 * mp.sigma * mp.sigma) * (T / 2.0);
        const double v = mp.sigma * std::sqrt(T / 3.0);
        const double forwardG = std::exp(m + 0.5 * v * v);
        const double expected = std::exp(-mp.r * T) * (forwardG - strike);

        const double c = ClosedFormAsian::geometricAsianPrice(mp, call);
        const double p = ClosedFormAsian::geometricAsianPrice(mp, put);
        CHECK_NEAR(c - p, expected, 1e-10);
    }
}

TEST(closed_form_call_decreases_and_put_increases_with_strike) {
    const MarketParams mp = gbmParams();
    double previousCall = 1e9;
    double previousPut = -1e9;
    for (double strike : {2.0, 3.0, 4.0, 4.5, 5.0, 6.0, 8.0}) {
        AsianOptionSpec call{strike, 1.0, 252, OptionType::Call, AveragingType::Geometric};
        AsianOptionSpec put = call;
        put.type = OptionType::Put;
        const double c = ClosedFormAsian::geometricAsianPrice(mp, call);
        const double p = ClosedFormAsian::geometricAsianPrice(mp, put);
        CHECK_MSG(c < previousCall, "call price must fall as strike rises");
        CHECK_MSG(p > previousPut, "put price must rise as strike rises");
        CHECK_MSG(c >= 0.0 && p >= 0.0, "option prices must be non-negative");
        previousCall = c;
        previousPut = p;
    }
}

// A call struck far below any reachable average is exercised with probability
// ~1, so its price collapses to the discounted forward minus the discounted
// strike -- a limit the formula must reproduce.
TEST(deep_in_the_money_call_approaches_discounted_forward_minus_strike) {
    const MarketParams mp = gbmParams();
    const double strike = 0.01;
    AsianOptionSpec call{strike, 1.0, 252, OptionType::Call, AveragingType::Geometric};

    const double T = call.maturity;
    const double m = std::log(mp.S0) + (mp.r - mp.q - 0.5 * mp.sigma * mp.sigma) * (T / 2.0);
    const double v = mp.sigma * std::sqrt(T / 3.0);
    const double forwardG = std::exp(m + 0.5 * v * v);
    const double expected = std::exp(-mp.r * T) * (forwardG - strike);

    CHECK_NEAR(ClosedFormAsian::geometricAsianPrice(mp, call), expected, 1e-9);
}

TEST(closed_form_rises_with_volatility) {
    MarketParams mp = gbmParams();
    AsianOptionSpec call{4.5, 1.0, 252, OptionType::Call, AveragingType::Geometric};
    double previous = -1.0;
    for (double sigma : {0.05, 0.15, 0.28, 0.45, 0.70}) {
        mp.sigma = sigma;
        const double price = ClosedFormAsian::geometricAsianPrice(mp, call);
        CHECK_MSG(price > previous, "an at-the-money option must be worth more as vol rises");
        previous = price;
    }
}

int main() { return testing::runAllTests("pricing math"); }
