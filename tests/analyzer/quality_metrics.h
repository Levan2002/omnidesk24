#pragma once

#include "core/types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace omnidesk {
namespace metrics {

// Compute PSNR between two I420 frames (Y-plane only).
inline double computePSNR_Y(const Frame& a, const Frame& b) {
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
inline double computeSSIM_Y(const Frame& a, const Frame& b) {
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

// Compression ratio: encoded bytes / raw frame bytes
inline double compressionRatio(size_t encodedBytes, int width, int height, PixelFormat fmt) {
    size_t rawBytes;
    switch (fmt) {
        case PixelFormat::BGRA:
        case PixelFormat::RGBA:
            rawBytes = static_cast<size_t>(width) * height * 4;
            break;
        case PixelFormat::I420:
        case PixelFormat::NV12:
            rawBytes = static_cast<size_t>(width) * height * 3 / 2;
            break;
    }
    if (encodedBytes == 0) return 0.0;
    return static_cast<double>(rawBytes) / encodedBytes;
}

// Bits per pixel
inline double bitsPerPixel(size_t encodedBytes, int width, int height) {
    double totalPixels = static_cast<double>(width) * height;
    if (totalPixels < 1.0) return 0.0;
    return static_cast<double>(encodedBytes * 8) / totalPixels;
}

// Per-frame quality result
struct FrameMetrics {
    uint64_t frameId = 0;
    double psnr = 0.0;
    double ssim = 0.0;
    size_t encodedBytes = 0;
    double bpp = 0.0;
    double encodeTimeMs = 0.0;
    double decodeTimeMs = 0.0;
    double colorConvertTimeMs = 0.0;
    bool isKeyFrame = false;
};

// Aggregate quality statistics
struct QualityStats {
    double avgPsnr = 0.0;
    double minPsnr = 0.0;
    double maxPsnr = 0.0;
    double stddevPsnr = 0.0;

    double avgSsim = 0.0;
    double minSsim = 0.0;
    double maxSsim = 0.0;

    double avgBpp = 0.0;
    size_t totalEncodedBytes = 0;
    double avgCompressionRatio = 0.0;

    static QualityStats compute(const std::vector<FrameMetrics>& frames, int width, int height) {
        QualityStats s;
        if (frames.empty()) return s;

        // Filter out frames where decoder didn't produce output (PSNR=0, SSIM=0)
        std::vector<const FrameMetrics*> decoded;
        for (const auto& f : frames) {
            s.totalEncodedBytes += f.encodedBytes;
            if (f.psnr > 0.0 || f.ssim > 0.0) {
                decoded.push_back(&f);
            }
        }

        if (decoded.empty()) return s;

        double psnrSum = 0, ssimSum = 0, bppSum = 0;
        s.minPsnr = 1e9;
        s.maxPsnr = -1e9;
        s.minSsim = 1e9;

        for (const auto* f : decoded) {
            psnrSum += f->psnr;
            ssimSum += f->ssim;
            bppSum += f->bpp;
            s.minPsnr = std::min(s.minPsnr, f->psnr);
            s.maxPsnr = std::max(s.maxPsnr, f->psnr);
            s.minSsim = std::min(s.minSsim, f->ssim);
        }

        double n = static_cast<double>(decoded.size());
        s.avgPsnr = psnrSum / n;
        s.avgSsim = ssimSum / n;
        s.avgBpp = bppSum / static_cast<double>(frames.size());
        s.avgCompressionRatio = compressionRatio(
            s.totalEncodedBytes / frames.size(), width, height, PixelFormat::I420);

        // Standard deviation of PSNR
        double variance = 0;
        for (const auto* f : decoded) {
            double diff = f->psnr - s.avgPsnr;
            variance += diff * diff;
        }
        s.stddevPsnr = std::sqrt(variance / n);

        return s;
    }
};

} // namespace metrics
} // namespace omnidesk
