#pragma once

#include <cstdint>

namespace omnidesk {

// Client-side cursor position prediction.
// Immediately applies local mouse deltas, then corrects when server confirms.
// This eliminates perceived cursor latency (20-50ms improvement).
class CursorPredictor {
public:
    // Apply a local mouse delta immediately
    void applyLocalDelta(int dx, int dy);

    // Server confirmed cursor position — blend towards it
    void onServerUpdate(int serverX, int serverY);

    // Get the predicted cursor position for rendering
    void getPosition(int& x, int& y) const;

    void reset(int x, int y);

private:
    float predictedX_ = 0;
    float predictedY_ = 0;
    float serverX_ = 0;
    float serverY_ = 0;
    float correctionRate_ = 0.3f; // Blend speed towards server position
};

} // namespace omnidesk
