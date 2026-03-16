#include "input/cursor_predictor.h"
#include <cmath>

namespace omnidesk {

void CursorPredictor::applyLocalDelta(int dx, int dy) {
    predictedX_ += static_cast<float>(dx);
    predictedY_ += static_cast<float>(dy);
}

void CursorPredictor::onServerUpdate(int serverX, int serverY) {
    serverX_ = static_cast<float>(serverX);
    serverY_ = static_cast<float>(serverY);

    // Blend predicted position towards server truth
    predictedX_ += (serverX_ - predictedX_) * correctionRate_;
    predictedY_ += (serverY_ - predictedY_) * correctionRate_;
}

void CursorPredictor::getPosition(int& x, int& y) const {
    x = static_cast<int>(std::round(predictedX_));
    y = static_cast<int>(std::round(predictedY_));
}

void CursorPredictor::reset(int x, int y) {
    predictedX_ = static_cast<float>(x);
    predictedY_ = static_cast<float>(y);
    serverX_ = predictedX_;
    serverY_ = predictedY_;
}

} // namespace omnidesk
