#include <gtest/gtest.h>

#include "codec/quality_tuner.h"
#include "core/types.h"

namespace omnidesk {

TEST(QualityTuner, TextRegionsGetLowerQP) {
    QualityTuner tuner;
    int baseQP = 30;

    int textQP = tuner.adjustQP(baseQP, ContentType::TEXT);
    // TEXT should get lower QP (better quality), default delta is -8
    EXPECT_EQ(textQP, baseQP - 8);
}

TEST(QualityTuner, MotionRegionsQPUnchanged) {
    QualityTuner tuner;
    int baseQP = 30;

    int motionQP = tuner.adjustQP(baseQP, ContentType::MOTION);
    EXPECT_EQ(motionQP, baseQP);
}

TEST(QualityTuner, StaticRegionsFlaggedForSkip) {
    QualityTuner tuner;
    int baseQP = 30;

    QPAdjustment adj = tuner.adjust(baseQP, ContentType::STATIC);
    EXPECT_TRUE(adj.skip);
}

TEST(QualityTuner, TextQPClampedToValidRange) {
    QualityTuner tuner;

    // Base QP of 3 with delta -8 would give -5, but should clamp to 0
    int textQP = tuner.adjustQP(3, ContentType::TEXT);
    EXPECT_GE(textQP, 0);
    EXPECT_LE(textQP, 51);
}

TEST(QualityTuner, CustomTextDelta) {
    QualityTuner tuner;
    tuner.setTextQPDelta(-12);

    int baseQP = 30;
    int textQP = tuner.adjustQP(baseQP, ContentType::TEXT);
    EXPECT_EQ(textQP, baseQP - 12);
}

} // namespace omnidesk
