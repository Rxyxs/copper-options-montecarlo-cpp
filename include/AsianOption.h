#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

enum class OptionType { Call, Put };
enum class AveragingType { Arithmetic, Geometric };

struct AsianOptionSpec {
    double strike;
    double maturity;            // years
    size_t numAveragingPoints;  // number of fixings (path[1..N], path[0] is t=0 spot)
    OptionType type;
    AveragingType averaging = AveragingType::Arithmetic;
};

inline double averagePrice(const double* path, size_t numAveragingPoints, AveragingType avg) {
    if (avg == AveragingType::Arithmetic) {
        double sum = 0.0;
        for (size_t i = 1; i <= numAveragingPoints; ++i) sum += path[i];
        return sum / static_cast<double>(numAveragingPoints);
    }
    double sumLog = 0.0;
    for (size_t i = 1; i <= numAveragingPoints; ++i) sumLog += std::log(path[i]);
    return std::exp(sumLog / static_cast<double>(numAveragingPoints));
}

inline double payoff(double avgPrice, const AsianOptionSpec& spec) {
    const double intrinsic = (spec.type == OptionType::Call) ? avgPrice - spec.strike
                                                               : spec.strike - avgPrice;
    return std::max(intrinsic, 0.0);
}
