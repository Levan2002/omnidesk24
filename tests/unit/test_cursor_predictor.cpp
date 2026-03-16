#include <gtest/gtest.h>

#include "input/cursor_predictor.h"

namespace omnidesk {

TEST(CursorPredictor, ResetPositionMatchesResetValue) {
    CursorPredictor predictor;
    predictor.reset(100, 200);

    int x = 0, y = 0;
    predictor.getPosition(x, y);
    EXPECT_EQ(x, 100);
    EXPECT_EQ(y, 200);
}

TEST(CursorPredictor, LocalDeltaReflectedInPosition) {
    CursorPredictor predictor;
    predictor.reset(100, 200);

    predictor.applyLocalDelta(10, -5);

    int x = 0, y = 0;
    predictor.getPosition(x, y);
    EXPECT_EQ(x, 110);
    EXPECT_EQ(y, 195);
}

TEST(CursorPredictor, ServerUpdateBlendsTowardServerPosition) {
    CursorPredictor predictor;
    predictor.reset(100, 100);

    // Apply a local delta to move the predicted position away
    predictor.applyLocalDelta(50, 50);

    int xBefore = 0, yBefore = 0;
    predictor.getPosition(xBefore, yBefore);
    EXPECT_EQ(xBefore, 150);
    EXPECT_EQ(yBefore, 150);

    // Server says cursor is actually at (120, 120)
    predictor.onServerUpdate(120, 120);

    int xAfter = 0, yAfter = 0;
    predictor.getPosition(xAfter, yAfter);

    // After blending, position should move toward (120, 120)
    // but not necessarily be exactly there after one update
    EXPECT_LT(xAfter, xBefore);
    EXPECT_LT(yAfter, yBefore);
}

TEST(CursorPredictor, MultipleServerUpdatesConverge) {
    CursorPredictor predictor;
    predictor.reset(0, 0);

    // Apply a large local delta
    predictor.applyLocalDelta(1000, 1000);

    // Repeatedly apply server updates at (500, 500)
    for (int i = 0; i < 100; ++i) {
        predictor.onServerUpdate(500, 500);
    }

    int x = 0, y = 0;
    predictor.getPosition(x, y);

    // After many corrections, position should converge to server position
    EXPECT_NEAR(x, 500, 2);
    EXPECT_NEAR(y, 500, 2);
}

} // namespace omnidesk
