#include <gtest/gtest.h>

#include "codec/quality_tuner.h"
#include "core/types.h"

namespace omnidesk {

TEST(QualityTuner, TextRegionsGetLowerQP) {
    QualityTuner tuner;
    int baseQP = 30;

    int textQP = tuner.adjustQP(baseQP, ContentType::TEXT);
    // TEXT should get lower QP (better quality), default delta is -15
    EXPECT_EQ(textQP, baseQP - 15);
}

TEST(QualityTuner, MotionRegionsGetHigherQP) {
    QualityTuner tuner;
    int baseQP = 30;

    int motionQP = tuner.adjustQP(baseQP, ContentType::MOTION);
    // MOTION gets slightly higher QP (perceptual masking), default delta is +4
    EXPECT_EQ(motionQP, baseQP + 4);
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
