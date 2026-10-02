#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <execution>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AsianOption.h"
#include "ClosedFormAsian.h"
#include "MarketModel.h"
#include "PathSimulator.h"
#include "RandomEngine.h"
#include "Timer.h"

struct SimulationConfig {
    size_t numPaths = 1'000'000;
    // 0 = parallel (std::execution::par_unseq, runtime picks worker count);
    // 1 = forced sequential (std::execution::seq) -- the single-thread
    // baseline `--benchmark-scaling` compares against. Any other value is
    // treated the same as 0: C++20's parallel execution policies do not
    // expose portable control over an exact worker-thread count, unlike
    // the hand-rolled std::thread chunking this replaced.
    unsigned numThreads = 0;
    bool antithetic = true;
    bool controlVariate = true;  // only applies when spec.averaging == Arithmetic
    uint64_t seed = 42;
};

struct SimulationResult {
    double price = 0.0;
    double stdError = 0.0;
    double confInterval95 = 0.0;
    double elapsedSeconds = 0.0;
    double pathsPerSecond = 0.0;
    size_t numPaths = 0;
    unsigned numThreads = 0;
    // Whether the control variate was actually applied, which is not the same
    // as having been requested: the engine declines it for models with no valid
    // analytic anchor. Reported here so callers read one source of truth
    // instead of re-deriving the condition and drifting out of sync with it.
    bool controlVariateApplied = false;
};

namespace detail {

// A generous fixed upper bound on averaging fixings lets each parallel work
// item simulate its path(s) into a stack-allocated std::array -- no heap
// allocation per path, and no shared mutable state between work items, so
// std::execution::par_unseq has nothing to synchronize.
inline constexpr size_t kMaxAveragingPoints = 4096;

struct Accum {
    double sum = 0.0;
    double sumSq = 0.0;
    size_t count = 0;
};

inline Accum operator+(const Accum& a, const Accum& b) {
    return {a.sum + b.sum, a.sumSq + b.sumSq, a.count + b.count};
}

// splitmix64 (Vigna): cheap, well-mixed derivation of an independent RNG
// seed per work item from (baseSeed, workItemIndex). This is what makes
// the parallel-algorithm version reproducible *and* thread-count-agnostic
// -- unlike the old std::thread version, the result no longer depends on
// how many paths happened to land on which worker.
inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

}  // namespace detail

class MonteCarloEngine {
public:
    // Whether the geometric control variate will actually be applied for this
    // request. Public so a caller can report it before running the simulation
    // (the CLI banner does) without re-deriving the rule and drifting out of
    // sync, which is how the Schwartz bias survived as long as it did.
    static bool controlVariateApplies(const MarketParams& mp, const AsianOptionSpec& spec,
                                       const SimulationConfig& cfg) {
        return cfg.controlVariate && spec.averaging == AveragingType::Arithmetic
               && mp.model == ModelType::GeometricBrownianMotion;
    }

    static SimulationResult price(const MarketParams& mp, const AsianOptionSpec& spec,
                                   const SimulationConfig& cfg) {
        using namespace detail;

        if (spec.numAveragingPoints > kMaxAveragingPoints) {
            throw std::runtime_error("numAveragingPoints exceeds MonteCarloEngine::kMaxAveragingPoints");
        }

        const double discount = std::exp(-mp.r * spec.maturity);
        const bool useControlVariate = controlVariateApplies(mp, spec, cfg);
        // The geometric control variate is only valid for GBM, because its
        // analytic anchor (ClosedFormAsian::geometricAsianPrice) is the
        // Kemna-Vorst formula, derived for GBM dynamics specifically.
        //
        // Heston was already excluded: the formula assumes constant sigma.
        // Schwartz is excluded for the same class of reason and used not to be,
        // which was a real bug. Under mean reversion the log-price is an
        // Ornstein-Uhlenbeck process, so the geometric average's mean is not
        // ln S0 + (r - q - sigma^2/2) T/2 and its variance is not sigma^2 T/3.
        // Anchoring to the GBM value therefore does not subtract a mean-zero
        // term: it drags the estimate toward the GBM price. Measured at the
        // defaults, it moved the Schwartz price from 0.1664 to 0.2997, and it
        // flattened the price's response to theta (the equilibrium level) from
        // a 0.312 swing across theta in [3.80, 5.20] down to 0.013 -- that is,
        // it cancelled out the model's defining parameter. See
        // test_engine_properties.cpp::control_variate_does_not_bias_the_price_under_any_model.
        //
        // Both excluded models fall back to plain MC, still with antithetic
        // variates, at the cost of a wider confidence interval for the same
        // path count.
        const double geoClosedForm =
            useControlVariate ? ClosedFormAsian::geometricAsianPrice(mp, spec) : 0.0;
        const double dt = spec.maturity / static_cast<double>(spec.numAveragingPoints);

        // With antithetic on, work items are pairs; soloPaths is then 0 for an
        // even numPaths and exactly 1 for an odd one. That single leftover path
        // contributes a solo (higher-variance) sample to the same accumulator as
        // the pair averages, which is statistically untidy but bounded at 1 item
        // out of numPaths/2 -- negligible in practice, and it disappears entirely
        // whenever numPaths is even.
        const size_t pairPaths = cfg.antithetic ? cfg.numPaths / 2 : 0;
        const size_t soloPaths = cfg.numPaths - pairPaths * 2;
        const size_t numWorkItems = pairPaths + soloPaths;

        std::vector<size_t> workItems(numWorkItems);
        std::iota(workItems.begin(), workItems.end(), size_t{0});

        auto simulateOne = [&](size_t workIndex) -> Accum {
            const uint64_t itemSeed = cfg.seed ^ splitmix64(static_cast<uint64_t>(workIndex));

            if (workIndex < pairPaths) {
                return simulatePair(mp, spec, dt, discount, useControlVariate, geoClosedForm,
                                     itemSeed);
            }
            return simulateSolo(mp, spec, dt, discount, useControlVariate, geoClosedForm, itemSeed);
        };

        Timer timer;
        Accum total;
        if (cfg.numThreads == 1) {
            total = std::transform_reduce(std::execution::seq, workItems.begin(), workItems.end(),
                                           Accum{}, std::plus<>{}, simulateOne);
        } else {
            total = std::transform_reduce(std::execution::par_unseq, workItems.begin(),
                                           workItems.end(), Accum{}, std::plus<>{}, simulateOne);
        }
        const double elapsed = timer.elapsedSeconds();

        const double mean = total.sum / static_cast<double>(total.count);
        const double variance =
            total.count > 1 ? (total.sumSq / static_cast<double>(total.count) - mean * mean) *
                                   static_cast<double>(total.count) / static_cast<double>(total.count - 1)
                             : 0.0;
        const double stdErr = std::sqrt(std::max(variance, 0.0) / static_cast<double>(total.count));

        SimulationResult result;
        result.price = mean;
        result.stdError = stdErr;
        result.confInterval95 = 1.959964 * stdErr;
        result.elapsedSeconds = elapsed;
        result.pathsPerSecond = static_cast<double>(cfg.numPaths) / elapsed;
        result.numPaths = cfg.numPaths;
        result.numThreads =
            cfg.numThreads == 1 ? 1u : std::max(1u, std::thread::hardware_concurrency());
        result.controlVariateApplied = useControlVariate;
        return result;
    }

private:
    static double sampleFromPath(const double* p, const AsianOptionSpec& spec, double discount,
                                  bool useControlVariate, double geoClosedForm) {
        const double avgTarget = averagePrice(p, spec.numAveragingPoints, spec.averaging);
        const double discountedTarget = discount * payoff(avgTarget, spec);
        if (!useControlVariate) return discountedTarget;

        const double avgGeo = averagePrice(p, spec.numAveragingPoints, AveragingType::Geometric);
        const double discountedGeo = discount * payoff(avgGeo, spec);
        return discountedTarget - discountedGeo + geoClosedForm;
    }

    static detail::Accum simulateSolo(const MarketParams& mp, const AsianOptionSpec& spec, double dt,
                                       double discount, bool useControlVariate, double geoClosedForm,
                                       uint64_t seed) {
        std::array<double, detail::kMaxAveragingPoints + 1> path{};
        FastGaussianRNG rng(seed);

        if (mp.model == ModelType::Heston) {
            simulateHestonPath(path.data(), spec.numAveragingPoints, dt, mp, rng);
        } else {
            simulatePath(path.data(), spec.numAveragingPoints, dt, mp, rng);
        }
        const double sample =
            sampleFromPath(path.data(), spec, discount, useControlVariate, geoClosedForm);
        return {sample, sample * sample, 1};
    }

    static detail::Accum simulatePair(const MarketParams& mp, const AsianOptionSpec& spec, double dt,
                                       double discount, bool useControlVariate, double geoClosedForm,
                                       uint64_t seed) {
        std::array<double, detail::kMaxAveragingPoints + 1> path{};
        std::array<double, detail::kMaxAveragingPoints + 1> antiPath{};
        FastGaussianRNG rng(seed);

        double s1, s2;
        if (mp.model == ModelType::Heston) {
            // Two independent normals per step -> twice the buffer.
            std::array<double, 2 * detail::kMaxAveragingPoints> normalPairs{};
            simulateHestonPathRecording(path.data(), normalPairs.data(), spec.numAveragingPoints, dt,
                                         mp, rng);
            simulateHestonPathFromNormals(antiPath.data(), normalPairs.data(), spec.numAveragingPoints,
                                           dt, mp, /*negate=*/true);
        } else {
            std::array<double, detail::kMaxAveragingPoints> normals{};
            simulatePathRecording(path.data(), normals.data(), spec.numAveragingPoints, dt, mp, rng);
            simulatePathFromNormals(antiPath.data(), normals.data(), spec.numAveragingPoints, dt, mp,
                                     /*negate=*/true);
        }
        s1 = sampleFromPath(path.data(), spec, discount, useControlVariate, geoClosedForm);
        s2 = sampleFromPath(antiPath.data(), spec, discount, useControlVariate, geoClosedForm);

        // The antithetic pair is ONE sample of the estimator, not two.
        //
        // s1 and s2 are negatively correlated by construction (s2 reuses the
        // same normals with the sign flipped) -- that negative correlation is
        // the entire point of antithetic sampling. Accumulating them as two
        // independent draws would make the variance formula below measure the
        // *marginal* variance of a single path, which antithetic sampling does
        // not change at all, instead of the variance of the pair average,
        // which is what actually shrinks.
        //
        // Measured on an arithmetic-average GBM call (60 seeds x 60k paths):
        // the true spread of prices across seeds drops from 0.00215 without
        // antithetic to 0.00165 with it, but accumulating pairs as two
        // independent samples reported 0.00205 either way -- i.e. it hid the
        // improvement and overstated the error by ~25% (implied pairwise
        // correlation rho ~ -0.36). Averaging the pair first makes the
        // reported standard error match the error the estimator actually has.
        // The price itself is identical under both conventions.
        const double pairMean = 0.5 * (s1 + s2);
        return {pairMean, pairMean * pairMean, 1};
    }
};
