// Visual quality benchmarks: encode synthetic test patterns with OpenH264,
// decode, and measure PSNR/SSIM against the originals.
// Prints quality metrics for human review; uses relaxed thresholds for assertions.

#include <gtest/gtest.h>

#include "core/types.h"
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace omnidesk {
namespace {

// ---------------------------------------------------------------------------
// Color conversion: BGRA -> I420 (BT.601)
// ---------------------------------------------------------------------------

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

            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * yStride + x] = static_cast<uint8_t>(std::clamp(yVal, 0, 255));

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

// ---------------------------------------------------------------------------
// Quality metrics
// ---------------------------------------------------------------------------

// Compute PSNR between two I420 frames (Y-plane only).
double computePSNR_Y(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
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
    mse /= static_cast<double>(w) * h;
    if (mse < 1e-10) return 100.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Compute simplified SSIM on Y-plane using an 8x8 block-based approach.
// Returns the mean SSIM across all 8x8 blocks.
double computeSSIM_Y(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
    int w = a.width;
    int h = a.height;

    constexpr int kBlockSize = 8;
    constexpr double C1 = 6.5025;    // (0.01 * 255)^2
    constexpr double C2 = 58.5225;   // (0.03 * 255)^2

    double ssimSum = 0.0;
    int blockCount = 0;

    for (int by = 0; by + kBlockSize <= h; by += kBlockSize) {
        for (int bx = 0; bx + kBlockSize <= w; bx += kBlockSize) {
            double sumA = 0, sumB = 0;
            double sumA2 = 0, sumB2 = 0, sumAB = 0;

            for (int dy = 0; dy < kBlockSize; ++dy) {
                for (int dx = 0; dx < kBlockSize; ++dx) {
                    double pa = a.plane(0)[(by + dy) * a.stride + (bx + dx)];
                    double pb = b.plane(0)[(by + dy) * b.stride + (bx + dx)];
                    sumA += pa;
                    sumB += pb;
                    sumA2 += pa * pa;
                    sumB2 += pb * pb;
                    sumAB += pa * pb;
                }
            }

            double n = kBlockSize * kBlockSize;
            double muA = sumA / n;
            double muB = sumB / n;
            double sigA2 = sumA2 / n - muA * muA;
            double sigB2 = sumB2 / n - muB * muB;
            double sigAB = sumAB / n - muA * muB;

            double num = (2.0 * muA * muB + C1) * (2.0 * sigAB + C2);
            double den = (muA * muA + muB * muB + C1) * (sigA2 + sigB2 + C2);
            ssimSum += num / den;
            ++blockCount;
        }
    }

    return (blockCount > 0) ? ssimSum / blockCount : 0.0;
}

// ---------------------------------------------------------------------------
// Synthetic test frame generators (BGRA)
// ---------------------------------------------------------------------------

// "text_document": white background with black horizontal lines simulating text.
Frame makeTextDocument(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    // Fill white.
    std::memset(f.data.data(), 255, f.data.size());

    // Draw horizontal "text lines": 2px tall black lines with 16px spacing,
    // offset from left/right by 40px to simulate margins.
    int margin = 40;
    int lineSpacing = 16;
    int lineHeight = 2;
    for (int y = 30; y + lineHeight < h - 30; y += lineSpacing) {
        for (int dy = 0; dy < lineHeight; ++dy) {
            uint8_t* row = f.data.data() + (y + dy) * f.stride;
            for (int x = margin; x < w - margin; ++x) {
                row[x * 4 + 0] = 0;   // B
                row[x * 4 + 1] = 0;   // G
                row[x * 4 + 2] = 0;   // R
                row[x * 4 + 3] = 255; // A
            }
        }
    }
    return f;
}

// "gradient": smooth color gradient (R increases left-to-right, G top-to-bottom).
Frame makeGradient(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t r = static_cast<uint8_t>(255 * x / std::max(w - 1, 1));
            uint8_t g = static_cast<uint8_t>(255 * y / std::max(h - 1, 1));
            uint8_t b = 128;
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// "checkerboard": alternating black/white 8x8 blocks (high spatial frequency).
Frame makeCheckerboard(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    constexpr int kBlockSize = 8;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            bool isWhite = ((x / kBlockSize) + (y / kBlockSize)) % 2 == 0;
            uint8_t val = isWhite ? 255 : 0;
            row[x * 4 + 0] = val;
            row[x * 4 + 1] = val;
            row[x * 4 + 2] = val;
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// "solid": uniform mid-gray (trivial case, should compress near-losslessly).
Frame makeSolid(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = 128;  // B
            row[x * 4 + 1] = 128;  // G
            row[x * 4 + 2] = 128;  // R
            row[x * 4 + 3] = 255;  // A
        }
    }
    return f;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class EncodeQualityTest : public ::testing::Test {
protected:
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;
    static constexpr uint32_t kTargetBitrate = 1000000;  // 1 Mbps

    void SetUp() override {
        initCodec();
    }

    void initCodec() {
        encoder_ = std::make_unique<OpenH264Encoder>();
        decoder_ = std::make_unique<OpenH264Decoder>();

        EncoderConfig cfg;
        cfg.width = kWidth;
        cfg.height = kHeight;
        cfg.targetBitrateBps = kTargetBitrate;
        cfg.maxBitrateBps = kTargetBitrate * 2;
        cfg.maxFps = 30.0f;
        cfg.screenContent = true;
        cfg.temporalLayers = 1;
        ASSERT_TRUE(encoder_->init(cfg));
        ASSERT_TRUE(decoder_->init(kWidth, kHeight));
    }

    struct QualityResult {
        std::string name;
        double psnr = 0.0;
        double ssim = 0.0;
        size_t encodedBytes = 0;
        double bpp = 0.0;  // bits per pixel
    };

    // Encode a BGRA frame as a key frame, decode, and measure quality.
    QualityResult measureQuality(const std::string& name, const Frame& bgra) {
        Frame i420 = bgraToI420(bgra);
        i420.frameId = 0;
        i420.timestampUs = 0;

        encoder_->requestKeyFrame();

        std::vector<RegionInfo> regions;
        regions.push_back({{0, 0, kWidth, kHeight}, ContentType::UNKNOWN});

        EncodedPacket packet;
        bool encOk = encoder_->encode(i420, regions, packet);
        EXPECT_TRUE(encOk) << "Encode failed for " << name;
        if (!encOk) return {name};

        Frame decoded;
        bool decOk = decoder_->decode(packet.data.data(), packet.data.size(), decoded);
        EXPECT_TRUE(decOk) << "Decode failed for " << name;
        if (!decOk) return {name};

        // Re-init encoder/decoder for the next test pattern (clean state).
        initCodec();

        QualityResult result;
        result.name = name;
        result.psnr = computePSNR_Y(i420, decoded);
        result.ssim = computeSSIM_Y(i420, decoded);
        result.encodedBytes = packet.data.size();
        result.bpp = static_cast<double>(packet.data.size() * 8) /
                     (static_cast<double>(kWidth) * kHeight);
        return result;
    }

    void printResults(const std::vector<QualityResult>& results) {
        std::cout << "\n";
        std::cout << "========================================================\n";
        std::cout << "  Encode Quality Results  (1080p @ "
                  << kTargetBitrate / 1000 << " kbps)\n";
        std::cout << "========================================================\n";
        std::cout << std::left << std::setw(18) << "Pattern"
                  << std::right << std::setw(10) << "PSNR(dB)"
                  << std::setw(10) << "SSIM"
                  << std::setw(12) << "Size(KB)"
                  << std::setw(10) << "BPP"
                  << "\n";
        std::cout << "--------------------------------------------------------\n";
        for (const auto& r : results) {
            std::cout << std::left << std::setw(18) << r.name
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(10) << r.psnr
                      << std::setw(10) << r.ssim
                      << std::setw(12) << (r.encodedBytes / 1024.0)
                      << std::setw(10) << r.bpp
                      << "\n";
        }
        std::cout << "========================================================\n\n";
    }

    std::unique_ptr<OpenH264Encoder> encoder_;
    std::unique_ptr<OpenH264Decoder> decoder_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(EncodeQualityTest, TextDocument_HighPSNR) {
    Frame bgra = makeTextDocument(kWidth, kHeight);
    auto result = measureQuality("text_document", bgra);
    std::cout << "[text_document] PSNR=" << result.psnr
              << " dB  SSIM=" << result.ssim << std::endl;

    // Text content with screen-content optimization should encode well.
    EXPECT_GT(result.psnr, 35.0)
        << "text_document PSNR below 35 dB threshold";
    EXPECT_GT(result.ssim, 0.95);
}

TEST_F(EncodeQualityTest, Gradient_ReasonablePSNR) {
    Frame bgra = makeGradient(kWidth, kHeight);
    auto result = measureQuality("gradient", bgra);
    std::cout << "[gradient] PSNR=" << result.psnr
              << " dB  SSIM=" << result.ssim << std::endl;

    // Smooth gradients are moderately compressible.
    EXPECT_GT(result.psnr, 30.0)
        << "gradient PSNR below 30 dB threshold";
    EXPECT_GT(result.ssim, 0.90);
}

TEST_F(EncodeQualityTest, Checkerboard_MinimumQuality) {
    Frame bgra = makeCheckerboard(kWidth, kHeight);
    auto result = measureQuality("checkerboard", bgra);
    std::cout << "[checkerboard] PSNR=" << result.psnr
              << " dB  SSIM=" << result.ssim << std::endl;

    // Checkerboard is the worst case (high frequency, hard to compress).
    // We use a very relaxed threshold here; mainly checking for encoder
    // misconfiguration rather than expecting high quality.
    EXPECT_GT(result.psnr, 15.0)
        << "checkerboard PSNR catastrophically low (possible encoder issue)";
}

TEST_F(EncodeQualityTest, Solid_VeryHighPSNR) {
    Frame bgra = makeSolid(kWidth, kHeight);
    auto result = measureQuality("solid", bgra);
    std::cout << "[solid] PSNR=" << result.psnr
              << " dB  SSIM=" << result.ssim << std::endl;

    // Solid color should be near-lossless.
    EXPECT_GT(result.psnr, 45.0)
        << "solid PSNR below 45 dB threshold";
    EXPECT_GT(result.ssim, 0.99);
}

TEST_F(EncodeQualityTest, AllPatterns_Summary) {
    // Run all patterns and print a summary table.
    std::vector<QualityResult> results;

    results.push_back(measureQuality("text_document", makeTextDocument(kWidth, kHeight)));
    results.push_back(measureQuality("gradient", makeGradient(kWidth, kHeight)));
    results.push_back(measureQuality("checkerboard", makeCheckerboard(kWidth, kHeight)));
    results.push_back(measureQuality("solid", makeSolid(kWidth, kHeight)));

    printResults(results);

    // Sanity: all encodings should produce non-empty output.
    for (const auto& r : results) {
        EXPECT_GT(r.encodedBytes, 0u) << r.name << " produced empty encoding";
    }

    // Relative ordering: solid should have highest PSNR, checkerboard lowest.
    EXPECT_GT(results[3].psnr, results[2].psnr)
        << "Expected solid PSNR > checkerboard PSNR";
}

TEST_F(EncodeQualityTest, MultiFrame_QualityStability) {
    // Encode 5 consecutive frames of the same content and verify that
    // quality remains stable (not degrading over time).
    Frame bgra = makeTextDocument(kWidth, kHeight);
    Frame i420 = bgraToI420(bgra);

    std::vector<double> psnrValues;

    for (int i = 0; i < 5; ++i) {
        i420.frameId = static_cast<uint64_t>(i);
        i420.timestampUs = static_cast<uint64_t>(i) * 33333;

        if (i == 0) encoder_->requestKeyFrame();

        std::vector<RegionInfo> regions;
        regions.push_back({{0, 0, kWidth, kHeight}, ContentType::TEXT});

        EncodedPacket packet;
        ASSERT_TRUE(encoder_->encode(i420, regions, packet))
            << "Encode failed at frame " << i;

        Frame decoded;
        ASSERT_TRUE(decoder_->decode(packet.data.data(), packet.data.size(), decoded))
            << "Decode failed at frame " << i;

        double psnr = computePSNR_Y(i420, decoded);
        psnrValues.push_back(psnr);
    }

    std::cout << "[MultiFrame stability] PSNR per frame:";
    for (size_t i = 0; i < psnrValues.size(); ++i) {
        std::cout << " f" << i << "=" << std::fixed << std::setprecision(2)
                  << psnrValues[i];
    }
    std::cout << " dB" << std::endl;

    // The key frame (frame 0) may differ from P-frames; compare P-frames only.
    if (psnrValues.size() >= 3) {
        double minP = *std::min_element(psnrValues.begin() + 1, psnrValues.end());
        double maxP = *std::max_element(psnrValues.begin() + 1, psnrValues.end());
        // PSNR should not vary by more than 10 dB across P-frames of identical content.
        EXPECT_LT(maxP - minP, 10.0)
            << "PSNR varies too much across P-frames of identical content";
    }
}

} // namespace
} // namespace omnidesk
