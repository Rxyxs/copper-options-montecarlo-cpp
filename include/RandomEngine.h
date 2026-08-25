#pragma once
#include <cmath>
#include <cstdint>
#include <random>

// Standard-normal generator using the Marsaglia polar method.
//
// Unlike Box-Muller, polar avoids sin()/cos() entirely (only sqrt/log),
// which is measurably cheaper per draw on this toolchain. Each accepted
// (u, v) pair yields two independent normals; the second is cached so
// two draws cost one sqrt/log/division, not two. Acceptance rate is
// pi/4 (~78.5%), so the average cost per accepted pair is still O(1).
class FastGaussianRNG {
public:
    explicit FastGaussianRNG(uint64_t seed)
        : engine_(seed), uniform_(-1.0, 1.0), hasSpare_(false), spare_(0.0) {}

    double next() {
        if (hasSpare_) {
            hasSpare_ = false;
            return spare_;
        }

        double u, v, s;
        do {
            u = uniform_(engine_);
            v = uniform_(engine_);
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);

        const double mul = std::sqrt(-2.0 * std::log(s) / s);
        spare_ = v * mul;
        hasSpare_ = true;
        return u * mul;
    }

private:
    std::mt19937_64 engine_;
    std::uniform_real_distribution<double> uniform_;
    bool hasSpare_;
    double spare_;
};
