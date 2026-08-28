#pragma once
#include <cmath>
#include <cstddef>
#include <utility>
#include "MarketModel.h"
#include "RandomEngine.h"

// Advances one price path in place using the *exact* transition density
// of the chosen model (not an Euler approximation), so simulation error
// comes only from the number of Monte Carlo paths, never from dt.
//
// `nextZ(i)` supplies the standard normal for step i (0-based). Taking it
// as a template parameter rather than a std::function lets the compiler
// inline the source under /O2 with zero call overhead, whether that
// source is a live RNG draw, a recorded draw, or a negated recorded draw
// (the three variants below).
template <typename NormalSource>
inline void simulatePathGeneric(double* path, size_t numSteps, double dt,
                                 const MarketParams& mp, NormalSource&& nextZ) {
    path[0] = mp.S0;

    if (mp.model == ModelType::GeometricBrownianMotion) {
        const double drift = (mp.r - mp.q - 0.5 * mp.sigma * mp.sigma) * dt;
        const double diffusion = mp.sigma * std::sqrt(dt);
        double S = mp.S0;
        for (size_t i = 1; i <= numSteps; ++i) {
            S *= std::exp(drift + diffusion * nextZ(i - 1));
            path[i] = S;
        }
    } else { // SchwartzMeanReverting: exact OU transition on ln(S)
        const double decay = std::exp(-mp.kappa * dt);
        const double meanShift = mp.theta * (1.0 - decay);
        const double stdShift =
            mp.sigma * std::sqrt((1.0 - decay * decay) / (2.0 * mp.kappa));
        double x = std::log(mp.S0);
        for (size_t i = 1; i <= numSteps; ++i) {
            x = x * decay + meanShift + stdShift * nextZ(i - 1);
            path[i] = std::exp(x);
        }
    }
}

// Draws fresh normals from `rng` and discards them.
inline void simulatePath(double* path, size_t numSteps, double dt,
                          const MarketParams& mp, FastGaussianRNG& rng) {
    simulatePathGeneric(path, numSteps, dt, mp, [&](size_t) { return rng.next(); });
}

// Draws fresh normals from `rng` and records them into `normalsOut`
// (size numSteps) so an antithetic partner path can reuse them.
inline void simulatePathRecording(double* path, double* normalsOut, size_t numSteps,
                                   double dt, const MarketParams& mp, FastGaussianRNG& rng) {
    simulatePathGeneric(path, numSteps, dt, mp, [&](size_t i) {
        const double z = rng.next();
        normalsOut[i] = z;
        return z;
    });
}

// Replays a previously recorded normal sequence, optionally negated.
// negate=true gives the antithetic partner of the path that produced
// `normals`, at zero extra RNG cost and with the negative correlation
// that makes antithetic variates reduce estimator variance.
inline void simulatePathFromNormals(double* path, const double* normals, size_t numSteps,
                                     double dt, const MarketParams& mp, bool negate) {
    const double sign = negate ? -1.0 : 1.0;
    simulatePathGeneric(path, numSteps, dt, mp, [&](size_t i) { return sign * normals[i]; });
}

// --- Heston stochastic-volatility path simulation -------------------------
//
// Needs two correlated normals per step (one drives the price, one the
// variance), so it doesn't fit the single-normal-per-step `NormalSource`
// abstraction above -- it gets its own small family of functions instead,
// mirroring the fresh/recording/replay pattern for antithetic support.
// `normalPairSource(i)` returns a `std::pair<double,double>` of
// *independent* standard normals (zIndepPrice, zIndepVol) for step i; the
// correlation `rho` is applied inside via zVol = rho*zPrice + sqrt(1-rho^2)*zIndepVol.
template <typename NormalPairSource>
inline void simulateHestonPathGeneric(double* path, size_t numSteps, double dt,
                                       const MarketParams& mp, NormalPairSource&& nextZPair) {
    path[0] = mp.S0;
    const double sqrtDt = std::sqrt(dt);
    const double sqrtOneMinusRho2 = std::sqrt(std::max(0.0, 1.0 - mp.rho * mp.rho));

    double S = mp.S0;
    double V = mp.v0;

    for (size_t i = 1; i <= numSteps; ++i) {
        const auto [zPrice, zVolIndep] = nextZPair(i - 1);
        const double zVol = mp.rho * zPrice + sqrtOneMinusRho2 * zVolIndep;

        const double Vplus = std::max(V, 0.0);  // full-truncation: use max(V,0) in drift/diffusion
        const double sqrtVplus = std::sqrt(Vplus);

        S *= std::exp((mp.r - mp.q - 0.5 * Vplus) * dt + sqrtVplus * sqrtDt * zPrice);
        V = V + mp.kappaV * (mp.thetaV - Vplus) * dt + mp.xiV * sqrtVplus * sqrtDt * zVol;

        path[i] = S;
    }
}

inline void simulateHestonPath(double* path, size_t numSteps, double dt, const MarketParams& mp,
                                FastGaussianRNG& rng) {
    simulateHestonPathGeneric(path, numSteps, dt, mp,
                               [&](size_t) { return std::pair{rng.next(), rng.next()}; });
}

// Records the *independent* normal pairs (not the already-correlated zVol)
// so the antithetic replay below can negate both underlying shocks.
inline void simulateHestonPathRecording(double* path, double* normalPairsOut, size_t numSteps,
                                         double dt, const MarketParams& mp, FastGaussianRNG& rng) {
    simulateHestonPathGeneric(path, numSteps, dt, mp, [&](size_t i) {
        const double zPrice = rng.next();
        const double zVolIndep = rng.next();
        normalPairsOut[2 * i] = zPrice;
        normalPairsOut[2 * i + 1] = zVolIndep;
        return std::pair{zPrice, zVolIndep};
    });
}

inline void simulateHestonPathFromNormals(double* path, const double* normalPairs, size_t numSteps,
                                           double dt, const MarketParams& mp, bool negate) {
    const double sign = negate ? -1.0 : 1.0;
    simulateHestonPathGeneric(path, numSteps, dt, mp, [&](size_t i) {
        return std::pair{sign * normalPairs[2 * i], sign * normalPairs[2 * i + 1]};
    });
}
