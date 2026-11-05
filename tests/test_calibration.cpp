// Tests for the backtest's estimators and scores. The scores have exact answers;
// the estimators are checked by recovering parameters from simulated paths long
// enough for sampling noise to be small next to the tolerance.
#include <algorithm>
#include <cmath>
#include <vector>
#include "Calibration.h"
#include "MarketModel.h"
#include "PathSimulator.h"
#include "RandomEngine.h"
#include "test_framework.h"

namespace {

constexpr double kDt = 1.0 / kTradingDaysPerYear;
constexpr double kPi = 3.14159265358979;

std::vector<double> simulated(const MarketParams& mp, size_t steps, uint64_t seed) {
    FastGaussianRNG rng(seed);
    std::vector<double> path(steps + 1);
    simulatePath(path.data(), steps, kDt, mp, rng);
    return path;
}

MarketParams schwartzTruth(double kappa) {
    MarketParams mp{};
    mp.S0 = 4.0;
    mp.sigma = 0.25;
    mp.kappa = kappa;
    mp.theta = std::log(4.0);
    mp.model = ModelType::SchwartzMeanReverting;
    return mp;
}

MarketParams randomWalk() {
    MarketParams mp{};
    mp.S0 = 4.0;
    mp.sigma = 0.25;
    mp.model = ModelType::GeometricBrownianMotion;
    return mp;
}

}  // namespace

TEST(crps_of_a_single_point_forecast_is_the_absolute_error) {
    CHECK_NEAR(crpsSorted({2.0}, 5.0), 3.0, 1e-12);
    CHECK_NEAR(crpsSorted({2.0}, 2.0), 0.0, 1e-12);
}

// E|X-y| = 1 and E|X-X'| = 1 over all ordered pairs, so the score is 1 - 1/2.
TEST(crps_matches_a_hand_computed_two_point_sample) {
    CHECK_NEAR(crpsSorted({0.0, 2.0}, 1.0), 0.5, 1e-12);
}

// CRPS(N(0,1), 0) = 2 phi(0) - 1/sqrt(pi)
TEST(crps_of_a_standard_normal_sample_matches_its_closed_form) {
    FastGaussianRNG rng(7);
    std::vector<double> x(400000);
    for (double& v : x) v = rng.next();
    std::sort(x.begin(), x.end());
    const double exact = 2.0 / std::sqrt(2.0 * kPi) - 1.0 / std::sqrt(kPi);
    CHECK_NEAR(crpsSorted(x, 0.0), exact, 2e-3);
}

// A forecast centered on the outcome has to score better than a biased one, or
// comparing models with this score would mean nothing.
TEST(crps_prefers_the_forecast_centered_on_the_outcome) {
    std::vector<double> centered, biased;
    for (int i = -50; i <= 50; ++i) {
        centered.push_back(i * 0.01);
        biased.push_back(i * 0.01 + 0.3);
    }
    CHECK(crpsSorted(centered, 0.0) < crpsSorted(biased, 0.0));
}

TEST(pit_and_quantile_read_a_sorted_sample) {
    const std::vector<double> x{1.0, 2.0, 3.0, 4.0};
    CHECK_NEAR(pitSorted(x, 0.5), 0.0, 1e-12);
    CHECK_NEAR(pitSorted(x, 2.5), 0.5, 1e-12);
    CHECK_NEAR(pitSorted(x, 9.0), 1.0, 1e-12);
    CHECK_NEAR(quantileSorted(x, 0.0), 1.0, 1e-12);
    CHECK_NEAR(quantileSorted(x, 0.5), 2.5, 1e-12);
    CHECK_NEAR(quantileSorted(x, 1.0), 4.0, 1e-12);
}

TEST(fit_ar1_recovers_an_exact_line) {
    std::vector<double> x{1.0};
    for (int i = 0; i < 20; ++i) x.push_back(0.5 + 0.8 * x.back());
    const Ar1Fit f = fitAr1(x);
    CHECK_NEAR(f.slope, 0.8, 1e-9);
    CHECK_NEAR(f.intercept, 0.5, 1e-9);
    CHECK_NEAR(f.residualVariance, 0.0, 1e-12);
}

TEST(fit_schwartz_recovers_its_parameters_from_a_long_path) {
    const MarketParams truth = schwartzTruth(2.0);
    const std::vector<double> p = simulated(truth, 252 * 400, 11);
    MarketParams fit{};
    fitSchwartz(p.data(), p.size(), fit);
    CHECK_BETWEEN(fit.kappa, 1.7, 2.3);
    CHECK_BETWEEN(fit.sigma, 0.24, 0.26);
    CHECK_NEAR(fit.theta, truth.theta, 0.02);
}

// The result the README leans on: a short window invents reversion. On a pure
// random walk, three years of data still produce a median kappa far above zero.
TEST(fit_schwartz_on_a_random_walk_over_three_years_invents_reversion) {
    std::vector<double> kappas;
    for (uint64_t seed = 0; seed < 400; ++seed) {
        const std::vector<double> p = simulated(randomWalk(), 756, seed);
        MarketParams fit{};
        fitSchwartz(p.data(), p.size(), fit);
        kappas.push_back(fit.kappa);
    }
    std::sort(kappas.begin(), kappas.end());
    CHECK_BETWEEN(quantileSorted(kappas, 0.5), 0.8, 2.5);
}

TEST(fit_schwartz_floors_kappa_on_a_trending_window) {
    std::vector<double> p;
    for (int i = 0; i < 300; ++i) p.push_back(std::exp(0.01 * i) * (1.0 + 0.001 * (i % 2)));
    MarketParams fit{};
    fitSchwartz(p.data(), p.size(), fit);
    CHECK(fit.kappa >= kMinKappa * 0.99);
    CHECK(std::isfinite(fit.sigma) && std::isfinite(fit.theta));
}

TEST(fit_heston_gets_the_variance_level_and_stays_in_bounds) {
    const std::vector<double> p = simulated(randomWalk(), 252 * 40, 3);
    MarketParams fit{};
    fitHeston(p.data(), p.size(), fit);
    CHECK_NEAR(fit.thetaV, 0.0625, 0.004);
    CHECK_NEAR(fit.v0, 0.0625, 0.05);
    CHECK(fit.kappaV > 0.0 && fit.xiV >= 0.0);
    CHECK_BETWEEN(fit.rho, -0.95, 0.95);
}

TEST(estimators_only_see_the_window_they_are_given) {
    const std::vector<double> p = simulated(schwartzTruth(2.0), 2000, 5);
    std::vector<double> changed = p;
    changed[1500] *= 10.0;
    MarketParams a{}, b{};
    fitSchwartz(p.data(), 1000, a);
    fitSchwartz(changed.data(), 1000, b);
    CHECK_NEAR(a.kappa, b.kappa, 0.0);
    CHECK_NEAR(a.sigma, b.sigma, 0.0);
}

int main() { return testing::runAllTests("calibration"); }
