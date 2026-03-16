#include <gtest/gtest.h>

#include "diff/rect_merger.h"
#include "core/types.h"

#include <vector>

namespace omnidesk {

TEST(RectMerger, SingleRect) {
    std::vector<Rect> input = {{10, 20, 100, 50}};
    auto result = RectMerger::merge(input);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], input[0]);
}

TEST(RectMerger, TwoOverlapping) {
    std::vector<Rect> input = {
        {0, 0, 100, 100},
        {50, 50, 100, 100},
    };
    auto result = RectMerger::merge(input);
    ASSERT_EQ(result.size(), 1u);

    // The merged rect should cover both: (0,0) to (150,150)
    EXPECT_LE(result[0].x, 0);
    EXPECT_LE(result[0].y, 0);
    EXPECT_GE(result[0].right(), 150);
    EXPECT_GE(result[0].bottom(), 150);
}

TEST(RectMerger, TwoAdjacent) {
    // Adjacent rects (touching edges)
    std::vector<Rect> input = {
        {0, 0, 50, 100},
        {50, 0, 50, 100},
    };
    auto result = RectMerger::merge(input);
    // Adjacent rects should merge into one
    ASSERT_EQ(result.size(), 1u);
    EXPECT_LE(result[0].x, 0);
    EXPECT_GE(result[0].right(), 100);
}

TEST(RectMerger, ManySmallRects_MergedDownToMaxRects) {
    // Create 20 small rects in a grid
    std::vector<Rect> input;
    for (int i = 0; i < 20; ++i) {
        input.push_back({i * 10, 0, 8, 8});
    }

    int maxRects = 4;
    auto result = RectMerger::merge(input, maxRects);
    EXPECT_LE(static_cast<int>(result.size()), maxRects);

    // All original rects should be covered by the result
    for (const auto& orig : input) {
        bool covered = false;
        for (const auto& merged : result) {
            if (merged.x <= orig.x && merged.right() >= orig.right() &&
                merged.y <= orig.y && merged.bottom() >= orig.bottom()) {
                covered = true;
                break;
            }
        }
        EXPECT_TRUE(covered) << "Rect (" << orig.x << "," << orig.y
                             << "," << orig.width << "," << orig.height
                             << ") not covered";
    }
}

TEST(RectMerger, NonOverlapping) {
    // Two rects far apart should remain separate (if maxRects allows)
    std::vector<Rect> input = {
        {0, 0, 10, 10},
        {1000, 1000, 10, 10},
    };
    auto result = RectMerger::merge(input, 8);
    // With maxRects=8, two non-overlapping rects should stay separate
    EXPECT_EQ(result.size(), 2u);
}

TEST(RectMerger, EmptyInput) {
    std::vector<Rect> input;
    auto result = RectMerger::merge(input);
    EXPECT_TRUE(result.empty());
}

} // namespace omnidesk
