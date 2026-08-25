#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    unsigned numThreads = 0;  // 0 = std::thread::hardware_concurrency()
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

class MonteCarloEngine {
public:
    static SimulationResult price(const MarketParams& mp, const AsianOptionSpec& spec,
                                   const SimulationConfig& cfg) {
        const unsigned numThreads =
            cfg.numThreads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : cfg.numThreads;

        const double discount = std::exp(-mp.r * spec.maturity);
        const double geoClosedForm = ClosedFormAsian::geometricAsianPrice(mp, spec);
        const double dt = spec.maturity / static_cast<double>(spec.numAveragingPoints);

        // One accumulator slot per thread. Each worker writes to its own
        // slot exactly once, after finishing its entire chunk, so there is
        // no cross-thread synchronization or false sharing in the hot loop.
        std::vector<double> threadSum(numThreads, 0.0);
        std::vector<double> threadSumSq(numThreads, 0.0);
        std::vector<size_t> threadCount(numThreads, 0);

        const size_t basePaths = cfg.numPaths / numThreads;
        const size_t remainder = cfg.numPaths % numThreads;

        Timer timer;
        std::vector<std::thread> workers;
        workers.reserve(numThreads);

        for (unsigned t = 0; t < numThreads; ++t) {
            const size_t chunk = basePaths + (t < remainder ? 1 : 0);
            workers.emplace_back([&, t, chunk]() {
                runWorker(t, chunk, mp, spec, cfg, dt, discount, geoClosedForm, threadSum[t],
                          threadSumSq[t], threadCount[t]);
            });
        }
        for (auto& w : workers) w.join();
        const double elapsed = timer.elapsedSeconds();

        double totalSum = 0.0, totalSumSq = 0.0;
        size_t totalCount = 0;
        for (unsigned t = 0; t < numThreads; ++t) {
            totalSum += threadSum[t];
            totalSumSq += threadSumSq[t];
            totalCount += threadCount[t];
        }

        const double mean = totalSum / static_cast<double>(totalCount);
        const double variance =
            totalCount > 1
                ? (totalSumSq / static_cast<double>(totalCount) - mean * mean) *
                      static_cast<double>(totalCount) / static_cast<double>(totalCount - 1)
                : 0.0;
        const double stdErr = std::sqrt(std::max(variance, 0.0) / static_cast<double>(totalCount));

        SimulationResult result;
        result.price = mean;
        result.stdError = stdErr;
        result.confInterval95 = 1.959964 * stdErr;
        result.elapsedSeconds = elapsed;
        result.pathsPerSecond = static_cast<double>(cfg.numPaths) / elapsed;
        result.numPaths = cfg.numPaths;
        result.numThreads = numThreads;
        return result;
    }

private:
    static void runWorker(unsigned threadIndex, size_t chunkPaths, const MarketParams& mp,
                           const AsianOptionSpec& spec, const SimulationConfig& cfg, double dt,
                           double discount, double geoClosedForm, double& outSum, double& outSumSq,
                           size_t& outCount) {
        if (chunkPaths == 0) {
            outSum = 0.0;
            outSumSq = 0.0;
            outCount = 0;
            return;
        }

        // Distinct, non-overlapping seed streams per thread.
        FastGaussianRNG rng(cfg.seed + static_cast<uint64_t>(threadIndex) * 0x9E3779B97F4A7C15ULL);

        std::vector<double> path(spec.numAveragingPoints + 1);
        std::vector<double> antiPath;
        std::vector<double> normals;
        const bool useAntithetic = cfg.antithetic;
        if (useAntithetic) {
            antiPath.resize(spec.numAveragingPoints + 1);
            normals.resize(spec.numAveragingPoints);
        }

        double localSum = 0.0;
        double localSumSq = 0.0;
        size_t localCount = 0;

        const bool useControlVariate = cfg.controlVariate && spec.averaging == AveragingType::Arithmetic;

        auto accumulateOnePath = [&](const double* p) {
            const double avgTarget = averagePrice(p, spec.numAveragingPoints, spec.averaging);
            const double discountedTarget = discount * payoff(avgTarget, spec);

            double sample = discountedTarget;
            if (useControlVariate) {
                const double avgGeo = averagePrice(p, spec.numAveragingPoints, AveragingType::Geometric);
                const double discountedGeo = discount * payoff(avgGeo, spec);
                sample = discountedTarget - discountedGeo + geoClosedForm;
            }
            localSum += sample;
            localSumSq += sample * sample;
            ++localCount;
        };

        const size_t pairPaths = useAntithetic ? chunkPaths / 2 : 0;
        const size_t soloPaths = chunkPaths - pairPaths * 2;

        for (size_t p = 0; p < pairPaths; ++p) {
            simulatePathRecording(path.data(), normals.data(), spec.numAveragingPoints, dt, mp, rng);
            accumulateOnePath(path.data());

            simulatePathFromNormals(antiPath.data(), normals.data(), spec.numAveragingPoints, dt, mp,
                                     /*negate=*/true);
            accumulateOnePath(antiPath.data());
        }
        for (size_t p = 0; p < soloPaths; ++p) {
            simulatePath(path.data(), spec.numAveragingPoints, dt, mp, rng);
            accumulateOnePath(path.data());
        }

        outSum = localSum;
        outSumSq = localSumSq;
        outCount = localCount;
    }
};
