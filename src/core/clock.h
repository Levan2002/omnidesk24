#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace omnidesk {

class Clock {
public:
    // High-resolution monotonic timestamp in microseconds
    static uint64_t nowUs() {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count());
    }

    // Milliseconds
    static double nowMs() {
        return static_cast<double>(nowUs()) / 1000.0;
    }

    // Simple RTT measurement helper
    struct RttEstimator {
        double smoothedRttMs = 0;
        double rttVarianceMs = 0;
        bool initialized = false;

        void update(double sampleMs) {
            if (!initialized) {
                smoothedRttMs = sampleMs;
                rttVarianceMs = sampleMs / 2.0;
                initialized = true;
            } else {
                // TCP-style EWMA
                double err = sampleMs - smoothedRttMs;
                smoothedRttMs += 0.125 * err;
                rttVarianceMs += 0.25 * (std::abs(err) - rttVarianceMs);
            }
        }

        double rto() const {
            return smoothedRttMs + 4.0 * rttVarianceMs;
        }
    };
};

} // namespace omnidesk
