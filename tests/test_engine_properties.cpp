// Property tests for MonteCarloEngine. These target the engineering claims the
// README makes about the engine -- reproducibility across thread counts,
// variance reduction that actually reduces variance, and 1/sqrt(N) error decay
// -- rather than just re-checking the price, which --self-test already covers.
#include <cmath>
#include <cstdio>
#include <vector>
#include "AsianOption.h"
#include "ClosedFormAsian.h"
#include "MarketModel.h"
#include "MonteCarloEngine.h"
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

AsianOptionSpec arithmeticCall() {
    return AsianOptionSpec{4.5, 1.0, 252, OptionType::Call, AveragingType::Arithmetic};
}

SimulationConfig baseConfig(size_t paths, uint64_t seed) {
    SimulationConfig cfg{};
    cfg.numPaths = paths;
    cfg.antithetic = true;
    cfg.controlVariate = false;  // isolate whatever each test is actually measuring
    cfg.seed = seed;
    return cfg;
}

double sampleStdDev(const std::vector<double>& v) {
    double sum = 0.0;
    for (double x : v) sum += x;
    const double mean = sum / static_cast<double>(v.size());
    double ss = 0.0;
    for (double x : v) ss += (x - mean) * (x - mean);
    return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

}  // namespace

// A fixed seed fixes *which paths get simulated* (each work item derives its own
// RNG stream from splitmix64(seed, workIndex)), so two runs sample exactly the
// same set of paths. It does not, however, guarantee bit-identical output: the
// parallel reduction is free to combine partial sums in a different order from
// one run to the next, and floating-point addition is not associative. Observed
// here: the price matched to the last bit but the standard error moved by
// ~1e-18. Reproducibility is therefore asserted to rounding, not bitwise.
TEST(same_seed_reproduces_the_same_price) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();
    const auto a = MonteCarloEngine::price(mp, spec, baseConfig(50'000, 12345));
    const auto b = MonteCarloEngine::price(mp, spec, baseConfig(50'000, 12345));
    CHECK_BETWEEN(std::abs(a.price - b.price) / a.price, 0.0, 1e-12);
    CHECK_BETWEEN(std::abs(a.stdError - b.stdError) / a.stdError, 0.0, 1e-12);
}

TEST(different_seeds_give_different_prices_within_error_bars) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();
    const auto a = MonteCarloEngine::price(mp, spec, baseConfig(50'000, 1));
    const auto b = MonteCarloEngine::price(mp, spec, baseConfig(50'000, 2));

    CHECK_MSG(a.price != b.price, "a different seed must actually change the sample path set");

    // ...but not by more than sampling noise: 5 combined standard errors.
    const double combined = std::sqrt(a.stdError * a.stdError + b.stdError * b.stdError);
    CHECK_MSG(std::abs(a.price - b.price) < 5.0 * combined,
              "two seeds disagreed by far more than their own error bars allow");
}

// The README claims the result "is independent of how many worker threads the
// runtime actually uses", which is what the per-work-item splitmix64 seeding
// buys. Sequential and parallel must therefore agree -- not bit-for-bit, since
// std::transform_reduce is free to combine partial sums in a different order
// and floating-point addition is not associative, but to within rounding.
TEST(sequential_and_parallel_agree_to_floating_point_rounding) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();

    auto seqCfg = baseConfig(200'000, 99);
    seqCfg.numThreads = 1;  // std::execution::seq
    auto parCfg = baseConfig(200'000, 99);
    parCfg.numThreads = 0;  // std::execution::par_unseq

    const auto seq = MonteCarloEngine::price(mp, spec, seqCfg);
    const auto par = MonteCarloEngine::price(mp, spec, parCfg);

    const double relativeDiff = std::abs(seq.price - par.price) / seq.price;
    CHECK_BETWEEN(relativeDiff, 0.0, 1e-12);
}

// Antithetic sampling reuses each path's normals with the sign flipped, so the
// two halves of a pair are negatively correlated and their average is a
// lower-variance sample than either half. This test exists because the engine
// originally accumulated the two halves as if they were independent draws,
// which made the reported standard error identical with and without antithetic
// sampling (ratio 1.000) and hid the benefit entirely.
TEST(antithetic_sampling_lowers_the_reported_standard_error) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();

    auto on = baseConfig(200'000, 7);
    on.antithetic = true;
    auto off = baseConfig(200'000, 7);
    off.antithetic = false;

    const auto withAnti = MonteCarloEngine::price(mp, spec, on);
    const auto withoutAnti = MonteCarloEngine::price(mp, spec, off);

    const double ratio = withoutAnti.stdError / withAnti.stdError;
    std::printf("      antithetic stderr ratio (off/on) = %.3f\n", ratio);
    CHECK_MSG(ratio > 1.15, "antithetic sampling should visibly reduce the reported standard error");
}

// The reported standard error has to actually describe the error the estimator
// makes. Pricing the same option under many seeds gives the true spread; the
// engine's reported stderr should track it. Rather than pin each arm to an
// absolute tolerance (the spread estimate itself is noisy at this seed count),
// this compares the two arms against each other: whatever estimation noise
// exists applies to both, so a mis-specified estimator in only one arm shows up
// as a mismatch between them. Before the antithetic fix this ratio was 1.31.
TEST(reported_standard_error_is_calibrated_with_and_without_antithetic) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();
    const int numSeeds = 40;
    const size_t paths = 25'000;

    double calibration[2] = {0.0, 0.0};
    for (int arm = 0; arm < 2; ++arm) {
        std::vector<double> prices;
        double reportedSum = 0.0;
        for (int k = 0; k < numSeeds; ++k) {
            auto cfg = baseConfig(paths, 500 + static_cast<uint64_t>(k) * 7919);
            cfg.antithetic = (arm == 0);
            const auto r = MonteCarloEngine::price(mp, spec, cfg);
            prices.push_back(r.price);
            reportedSum += r.stdError;
        }
        const double trueSpread = sampleStdDev(prices);
        const double reportedMean = reportedSum / static_cast<double>(numSeeds);
        calibration[arm] = reportedMean / trueSpread;
        std::printf("      %s: reported/true = %.3f\n",
                    arm == 0 ? "antithetic ON " : "antithetic OFF", calibration[arm]);
    }

    const double ratioOfRatios = calibration[0] / calibration[1];
    std::printf("      ratio of calibrations = %.3f\n", ratioOfRatios);
    CHECK_BETWEEN(ratioOfRatios, 0.85, 1.18);
}

// The defining property of Monte Carlo: error falls as 1/sqrt(N), so
// quadrupling the path count should halve the standard error.
TEST(standard_error_decays_as_one_over_sqrt_n) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();

    const auto small = MonteCarloEngine::price(mp, spec, baseConfig(100'000, 11));
    const auto large = MonteCarloEngine::price(mp, spec, baseConfig(400'000, 11));

    const double ratio = small.stdError / large.stdError;
    std::printf("      stderr(100k)/stderr(400k) = %.3f (expect ~2.0)\n", ratio);
    CHECK_BETWEEN(ratio, 1.8, 2.2);
}

// A control variate must cut variance WITHOUT moving the expectation. Both
// halves matter: a "variance reduction" that shifts the price is just a bug
// with a smaller error bar.
TEST(control_variate_cuts_variance_without_biasing_the_price) {
    const auto mp = gbmParams();
    const auto spec = arithmeticCall();

    auto withCv = baseConfig(200'000, 7);
    withCv.controlVariate = true;
    auto withoutCv = baseConfig(200'000, 7);
    withoutCv.controlVariate = false;

    const auto cv = MonteCarloEngine::price(mp, spec, withCv);
    const auto raw = MonteCarloEngine::price(mp, spec, withoutCv);

    const double ratio = raw.stdError / cv.stdError;
    std::printf("      control-variate stderr ratio (off/on) = %.2f\n", ratio);
    CHECK_MSG(ratio > 5.0, "the geometric control variate should cut the standard error severalfold");

    const double combined95 = 1.959964 * std::sqrt(cv.stdError * cv.stdError + raw.stdError * raw.stdError);
    CHECK_MSG(std::abs(cv.price - raw.price) < combined95,
              "control variate changed the price beyond sampling noise -- it is biasing the estimate");
}

// End-to-end anchor at a much lower path count than --self-test uses, so a
// systematic break in the simulator shows up here as a fast, isolated failure.
TEST(geometric_asian_price_matches_the_closed_form) {
    const auto mp = gbmParams();
    AsianOptionSpec geo{4.5, 1.0, 252, OptionType::Call, AveragingType::Geometric};

    const auto mc = MonteCarloEngine::price(mp, geo, baseConfig(400'000, 4242));
    const double closedForm = ClosedFormAsian::geometricAsianPrice(mp, geo);

    std::printf("      MC = %.6f, closed form = %.6f, stderr = %.6f\n", mc.price, closedForm,
                mc.stdError);
    // 4 standard errors, plus a small allowance for the discrete-vs-continuous
    // averaging gap the closed form does not model (252 fixings, a few bp).
    CHECK_MSG(std::abs(mc.price - closedForm) < 4.0 * mc.stdError + 5e-4,
              "Monte Carlo price drifted away from the Kemna-Vorst closed form");
}

// AM-GM again, but at the price level: since the geometric average of a path is
// never above its arithmetic average, the arithmetic-average call is worth more.
TEST(arithmetic_average_call_is_worth_more_than_geometric) {
    const auto mp = gbmParams();
    AsianOptionSpec arith = arithmeticCall();
    AsianOptionSpec geo = arith;
    geo.averaging = AveragingType::Geometric;

    const auto a = MonteCarloEngine::price(mp, arith, baseConfig(200'000, 31));
    const auto g = MonteCarloEngine::price(mp, geo, baseConfig(200'000, 31));

    CHECK_MSG(a.price > g.price, "arithmetic-average call must dominate the geometric-average one");
}

TEST(rejects_more_averaging_points_than_the_buffer_allows) {
    const auto mp = gbmParams();
    AsianOptionSpec tooMany{4.5, 1.0, detail::kMaxAveragingPoints + 1, OptionType::Call,
                             AveragingType::Arithmetic};
    bool threw = false;
    try {
        MonteCarloEngine::price(mp, tooMany, baseConfig(1'000, 1));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK_MSG(threw, "exceeding kMaxAveragingPoints must throw, not overflow the stack buffer");
}

int main() { return testing::runAllTests("engine properties"); }
