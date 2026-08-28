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
    static SimulationResult price(const MarketParams& mp, const AsianOptionSpec& spec,
                                   const SimulationConfig& cfg) {
        using namespace detail;

        if (spec.numAveragingPoints > kMaxAveragingPoints) {
            throw std::runtime_error("numAveragingPoints exceeds MonteCarloEngine::kMaxAveragingPoints");
        }

        const double discount = std::exp(-mp.r * spec.maturity);
        const bool useControlVariate = cfg.controlVariate && spec.averaging == AveragingType::Arithmetic
                                        && mp.model != ModelType::Heston;
        // Heston has no closed-form geometric-Asian anchor (ClosedFormAsian
        // assumes constant sigma), so the geometric control variate is only
        // applied for GBM/Schwartz -- Heston pricing falls back to plain MC
        // (still with antithetic variates), documented in the README.
        const double geoClosedForm =
            useControlVariate ? ClosedFormAsian::geometricAsianPrice(mp, spec) : 0.0;
        const double dt = spec.maturity / static_cast<double>(spec.numAveragingPoints);

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
        return {s1 + s2, s1 * s1 + s2 * s2, 2};
    }
};
