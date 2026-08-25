#pragma once
#include <chrono>

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}

    void reset() { start_ = std::chrono::high_resolution_clock::now(); }

    double elapsedSeconds() const {
        return std::chrono::duration<double>(
                   std::chrono::high_resolution_clock::now() - start_)
            .count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
};
