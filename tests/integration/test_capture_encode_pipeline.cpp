// Integration test: capture -> diff -> encode -> decode pipeline
// Uses synthetic frames (no actual screen capture required).

#include <gtest/gtest.h>

#include "core/types.h"
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"
#include "codec/codec_factory.h"
#include "diff/content_classifier.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace omnidesk {
namespace {

// ---------------------------------------------------------------------------
// Helpers: synthetic frame generation
// ---------------------------------------------------------------------------

// Create a BGRA frame with a horizontal gradient (R increases left-to-right,
// G increases top-to-bottom, B fixed).
Frame makeBGRAGradient(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t r = static_cast<uint8_t>(255 * x / std::max(w - 1, 1));
            uint8_t g = static_cast<uint8_t>(255 * y / std::max(h - 1, 1));
            uint8_t b = 128;
            row[x * 4 + 0] = b;  // B
            row[x * 4 + 1] = g;  // G
            row[x * 4 + 2] = r;  // R
            row[x * 4 + 3] = 255; // A
        }
    }
    return f;
}

// Create a BGRA frame with colored rectangles (4 quadrants: red, green, blue, white).
Frame makeBGRAColoredRects(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    int halfW = w / 2;
    int halfH = h / 2;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            if (x < halfW && y < halfH) {
                r = 255;               // top-left: red
            } else if (x >= halfW && y < halfH) {
                g = 255;               // top-right: green
            } else if (x < halfW && y >= halfH) {
                b = 255;               // bottom-left: blue
            } else {
                r = g = b = 255;       // bottom-right: white
            }
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// Convert a BGRA frame to I420 (simple BT.601 conversion).
Frame bgraToI420(const Frame& bgra) {
    Frame out;
    out.allocate(bgra.width, bgra.height, PixelFormat::I420);
    out.timestampUs = bgra.timestampUs;
    out.frameId = bgra.frameId;

    int w = bgra.width;
    int h = bgra.height;
    uint8_t* yPlane = out.plane(0);
    uint8_t* uPlane = out.plane(1);
    uint8_t* vPlane = out.plane(2);

    int yStride = out.stride;
    int uvStride = out.stride / 2;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = bgra.data.data() + y * bgra.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];

            // BT.601 Y
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * yStride + x] = static_cast<uint8_t>(std::clamp(yVal, 0, 255));

            // Subsample U,V at 2x2
            if ((y % 2 == 0) && (x % 2 == 0)) {
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                uPlane[(y / 2) * uvStride + (x / 2)] =
                    static_cast<uint8_t>(std::clamp(uVal, 0, 255));
                vPlane[(y / 2) * uvStride + (x / 2)] =
                    static_cast<uint8_t>(std::clamp(vVal, 0, 255));
            }
        }
    }
    return out;
}

// Compute PSNR between two I420 frames (Y-plane only).
double computePSNR_Y(const Frame& a, const Frame& b) {
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    EXPECT_EQ(a.format, PixelFormat::I420);
    EXPECT_EQ(b.format, PixelFormat::I420);

    int w = a.width;
    int h = a.height;
    double mse = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int diff = static_cast<int>(a.plane(0)[y * a.stride + x]) -
                       static_cast<int>(b.plane(0)[y * b.stride + x]);
            mse += diff * diff;
        }
    }
    mse /= (w * h);
    if (mse < 1e-10) return 100.0;  // identical
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class CaptureEncodePipelineTest : public ::testing::Test {
protected:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 480;

    void SetUp() override {
        encoder_ = std::make_unique<OpenH264Encoder>();
        decoder_ = std::make_unique<OpenH264Decoder>();

        EncoderConfig cfg;
        cfg.width = kWidth;
        cfg.height = kHeight;
        cfg.targetBitrateBps = 1000000;
        cfg.maxBitrateBps = 2000000;
        cfg.maxFps = 30.0f;
        cfg.screenContent = true;
        cfg.temporalLayers = 1;
        ASSERT_TRUE(encoder_->init(cfg));
        ASSERT_TRUE(decoder_->init(kWidth, kHeight));
    }

    std::unique_ptr<OpenH264Encoder> encoder_;
    std::unique_ptr<OpenH264Decoder> decoder_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(CaptureEncodePipelineTest, GradientFrame_EncodeDecode_DimensionsMatch) {
    Frame bgra = makeBGRAGradient(kWidth, kHeight);
    Frame i420 = bgraToI420(bgra);
    i420.frameId = 1;
    i420.timestampUs = 33333;

    // Classify content (just exercises the API, no strict assertions)
    ContentClassifier classifier;
    Rect fullFrame{0, 0, kWidth, kHeight};
    ContentType ct = classifier.classify(bgra, fullFrame);
    // Gradient tends to be classified as UNKNOWN or MOTION; just ensure no crash
    (void)ct;

    // Build a region list covering the full frame
    std::vector<RegionInfo> regions;
    regions.push_back({fullFrame, ContentType::UNKNOWN});

    EncodedPacket packet;
    ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    EXPECT_GT(packet.data.size(), 0u);

    Frame decoded;
    ASSERT_TRUE(decoder_->decode(packet.data.data(), packet.data.size(), decoded));
    EXPECT_EQ(decoded.width, kWidth);
    EXPECT_EQ(decoded.height, kHeight);
    EXPECT_EQ(decoded.format, PixelFormat::I420);
}

TEST_F(CaptureEncodePipelineTest, ColoredRects_EncodeDecode_DimensionsMatch) {
    Frame bgra = makeBGRAColoredRects(kWidth, kHeight);
    Frame i420 = bgraToI420(bgra);
    i420.frameId = 1;
    i420.timestampUs = 33333;

    std::vector<RegionInfo> regions;
    regions.push_back({{0, 0, kWidth, kHeight}, ContentType::TEXT});

    EncodedPacket packet;
    ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    EXPECT_GT(packet.data.size(), 0u);

    Frame decoded;
    ASSERT_TRUE(decoder_->decode(packet.data.data(), packet.data.size(), decoded));
    EXPECT_EQ(decoded.width, kWidth);
    EXPECT_EQ(decoded.height, kHeight);
}

TEST_F(CaptureEncodePipelineTest, Roundtrip_DecodedFrameSimilarToInput) {
    // Encode a gradient frame as a key frame, decode, and check PSNR is reasonable.
    Frame bgra = makeBGRAGradient(kWidth, kHeight);
    Frame i420 = bgraToI420(bgra);
    i420.frameId = 1;
    i420.timestampUs = 33333;

    encoder_->requestKeyFrame();

    std::vector<RegionInfo> regions;
    regions.push_back({{0, 0, kWidth, kHeight}, ContentType::UNKNOWN});

    EncodedPacket packet;
    ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    EXPECT_TRUE(packet.isKeyFrame);

    Frame decoded;
    ASSERT_TRUE(decoder_->decode(packet.data.data(), packet.data.size(), decoded));

    // PSNR sanity: H.264 on a smooth gradient at 1 Mbps / 640x480 should
    // produce something clearly recognizable (> 20 dB at minimum).
    double psnr = computePSNR_Y(i420, decoded);
    std::cout << "[Roundtrip] PSNR Y-plane = " << psnr << " dB" << std::endl;
    EXPECT_GT(psnr, 20.0) << "Decoded frame is too different from input";
}

TEST_F(CaptureEncodePipelineTest, MultipleFrames_SequentialEncoding) {
    // Encode several frames in sequence (simulating a capture loop).
    // Verifies that the encoder handles a stream of frames without errors.
    constexpr int kFrameCount = 10;

    for (int i = 0; i < kFrameCount; ++i) {
        Frame bgra = makeBGRAGradient(kWidth, kHeight);
        Frame i420 = bgraToI420(bgra);
        i420.frameId = static_cast<uint64_t>(i);
        i420.timestampUs = static_cast<uint64_t>(i) * 33333;

        std::vector<RegionInfo> regions;
        regions.push_back({{0, 0, kWidth, kHeight}, ContentType::UNKNOWN});

        EncodedPacket packet;
        ASSERT_TRUE(encoder_->encode(i420, regions, packet))
            << "Encode failed at frame " << i;
        EXPECT_GT(packet.data.size(), 0u);

        Frame decoded;
        ASSERT_TRUE(decoder_->decode(packet.data.data(), packet.data.size(), decoded))
            << "Decode failed at frame " << i;
        EXPECT_EQ(decoded.width, kWidth);
        EXPECT_EQ(decoded.height, kHeight);
    }
}

TEST_F(CaptureEncodePipelineTest, KeyFrameRequest_ProducesKeyFrame) {
    // First frame is always a key frame.
    Frame bgra = makeBGRAGradient(kWidth, kHeight);
    Frame i420 = bgraToI420(bgra);
    i420.frameId = 0;
    i420.timestampUs = 0;

    std::vector<RegionInfo> regions;
    regions.push_back({{0, 0, kWidth, kHeight}, ContentType::UNKNOWN});

    EncodedPacket packet;
    ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    EXPECT_TRUE(packet.isKeyFrame) << "First frame should be a key frame";

    // Encode a few P-frames
    for (int i = 1; i <= 3; ++i) {
        i420.frameId = static_cast<uint64_t>(i);
        i420.timestampUs = static_cast<uint64_t>(i) * 33333;
        ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    }

    // Request a key frame explicitly
    encoder_->requestKeyFrame();
    i420.frameId = 4;
    i420.timestampUs = 4 * 33333;
    ASSERT_TRUE(encoder_->encode(i420, regions, packet));
    EXPECT_TRUE(packet.isKeyFrame) << "Key frame was requested but not produced";
}

TEST_F(CaptureEncodePipelineTest, CodecFactory_CreatesOpenH264) {
    // Verify the factory can create an OpenH264 encoder and decoder.
    auto enc = CodecFactory::createEncoder(CodecBackend::OpenH264);
    ASSERT_NE(enc, nullptr);

    auto dec = CodecFactory::createDecoder(CodecBackend::OpenH264);
    ASSERT_NE(dec, nullptr);

    EncoderConfig cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.targetBitrateBps = 500000;
    ASSERT_TRUE(enc->init(cfg));

    ASSERT_TRUE(dec->init(kWidth, kHeight));

    EncoderInfo info = enc->getInfo();
    EXPECT_FALSE(info.name.empty());
}

TEST_F(CaptureEncodePipelineTest, ContentClassifier_SmokeTest) {
    // Ensure the content classifier runs on synthetic frames without crashing,
    // and returns plausible results.
    ContentClassifier classifier;

    // A solid white frame should be STATIC or UNKNOWN (not TEXT or MOTION).
    Frame solidBgra;
    solidBgra.allocate(kWidth, kHeight, PixelFormat::BGRA);
    std::memset(solidBgra.data.data(), 255, solidBgra.data.size());
    Rect fullRect{0, 0, kWidth, kHeight};
    ContentType solidType = classifier.classify(solidBgra, fullRect);
    EXPECT_NE(solidType, ContentType::MOTION)
        << "Solid frame should not be classified as MOTION";

    // A gradient frame
    Frame gradBgra = makeBGRAGradient(kWidth, kHeight);
    ContentType gradType = classifier.classify(gradBgra, fullRect);
    (void)gradType; // no strict assertion, just ensure no crash
}

} // namespace
} // namespace omnidesk
