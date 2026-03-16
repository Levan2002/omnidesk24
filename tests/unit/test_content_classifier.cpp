#include <gtest/gtest.h>

#include "diff/content_classifier.h"
#include "core/types.h"

#include <cstdlib>
#include <vector>

namespace omnidesk {

TEST(ContentClassifier, SolidColorBlock_StaticOrText) {
    // A solid-color frame should classify as STATIC or TEXT (few colors, no motion).
    Frame frame;
    frame.allocate(64, 64, PixelFormat::BGRA);
    // Fill with a single solid color (blue)
    for (size_t i = 0; i < frame.data.size(); i += 4) {
        frame.data[i + 0] = 200;  // B
        frame.data[i + 1] = 50;   // G
        frame.data[i + 2] = 50;   // R
        frame.data[i + 3] = 255;  // A
    }

    Rect region = {0, 0, 64, 64};
    ContentClassifier classifier;
    ContentType result = classifier.classify(frame, region);

    // Solid color should be either STATIC or TEXT (low edge density, few colors)
    EXPECT_TRUE(result == ContentType::STATIC || result == ContentType::TEXT)
        << "Expected STATIC or TEXT, got " << static_cast<int>(result);
}

TEST(ContentClassifier, RandomNoise_Motion) {
    // A frame filled with random noise should classify as MOTION (many colors, high activity).
    Frame curr;
    curr.allocate(64, 64, PixelFormat::BGRA);
    std::srand(12345);
    for (size_t i = 0; i < curr.data.size(); i += 4) {
        curr.data[i + 0] = static_cast<uint8_t>(std::rand() % 256);
        curr.data[i + 1] = static_cast<uint8_t>(std::rand() % 256);
        curr.data[i + 2] = static_cast<uint8_t>(std::rand() % 256);
        curr.data[i + 3] = 255;
    }

    // Create a different previous frame to trigger temporal activity
    Frame prev;
    prev.allocate(64, 64, PixelFormat::BGRA);
    for (size_t i = 0; i < prev.data.size(); i += 4) {
        prev.data[i + 0] = static_cast<uint8_t>(std::rand() % 256);
        prev.data[i + 1] = static_cast<uint8_t>(std::rand() % 256);
        prev.data[i + 2] = static_cast<uint8_t>(std::rand() % 256);
        prev.data[i + 3] = 255;
    }

    ContentClassifier classifier;
    classifier.updateTemporalState(prev, curr);

    Rect region = {0, 0, 64, 64};
    ContentType result = classifier.classify(curr, region);

    EXPECT_EQ(result, ContentType::MOTION);
}

TEST(ContentClassifier, ClassifyReturnsValidContentType) {
    Frame frame;
    frame.allocate(32, 32, PixelFormat::BGRA);
    std::memset(frame.data.data(), 128, frame.data.size());

    Rect region = {0, 0, 32, 32};
    ContentClassifier classifier;
    ContentType result = classifier.classify(frame, region);

    EXPECT_TRUE(result == ContentType::UNKNOWN ||
                result == ContentType::TEXT ||
                result == ContentType::MOTION ||
                result == ContentType::STATIC);
}

} // namespace omnidesk
