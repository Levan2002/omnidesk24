// Codec comparison benchmark: OmniCodec vs OpenH264
// Measures encode speed, decode speed, compressed size, PSNR, and SSIM
// across multiple content types and resolutions.

#include <benchmark/benchmark.h>
#include "codec/omni/omni_encoder.h"
#include "codec/omni/omni_decoder.h"
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"
#include "core/types.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace omnidesk;

// ---------------------------------------------------------------------------
// Test frame generators (BGRA)
// ---------------------------------------------------------------------------

static Frame makeBGRA(int w, int h) {
    Frame f;
    f.allocate(w, h, PixelFormat::BGRA);
    return f;
}

// Text document: white background with black lines (best case for screen codec)
static Frame makeTextFrame(int w, int h) {
    Frame f = makeBGRA(w, h);
    std::memset(f.data.data(), 255, f.data.size());
    for (int y = 30; y + 2 < h - 30; y += 16) {
        for (int dy = 0; dy < 2; ++dy) {
            uint8_t* row = f.data.data() + (y + dy) * f.stride;
            for (int x = 40; x < w - 40; ++x) {
                row[x * 4 + 0] = 0;
                row[x * 4 + 1] = 0;
                row[x * 4 + 2] = 0;
                row[x * 4 + 3] = 255;
            }
        }
    }
    return f;
}

// Code editor: dark background with colored syntax
static Frame makeCodeFrame(int w, int h) {
    Frame f = makeBGRA(w, h);
    for (size_t i = 0; i < f.data.size(); i += 4) {
        f.data[i + 0] = 30;
        f.data[i + 1] = 30;
        f.data[i + 2] = 30;
        f.data[i + 3] = 255;
    }
    struct Color { uint8_t r, g, b; };
    const Color colors[] = {
        {86, 156, 214}, {78, 201, 176}, {206, 145, 120},
        {181, 206, 168}, {220, 220, 220}, {197, 134, 192},
    };
    int lineH = 20, margin = 60, charW = 9;
    for (int li = 0; li * lineH + 10 < h - 10; ++li) {
        int y0 = li * lineH + 10;
        int x = margin;
        for (int ti = 0; ti < 8 && x < w - 20; ++ti) {
            int tokenLen = 3 + ((li * 7 + ti * 13) % 12);
            const auto& c = colors[(li + ti) % 6];
            for (int dy = 3; dy < lineH - 3; ++dy) {
                for (int dx = 0; dx < tokenLen * charW && x + dx < w - 20; ++dx) {
                    if (((dx / charW * 3 + dy) % 4) != 0) {
                        uint8_t* row = f.data.data() + (y0 + dy) * f.stride;
                        row[(x + dx) * 4 + 0] = c.b;
                        row[(x + dx) * 4 + 1] = c.g;
                        row[(x + dx) * 4 + 2] = c.r;
                    }
                }
            }
            x += tokenLen * charW + charW;
        }
    }
    return f;
}

// Smooth gradient
static Frame makeGradientFrame(int w, int h) {
    Frame f = makeBGRA(w, h);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = 128;
            row[x * 4 + 1] = static_cast<uint8_t>(255 * y / std::max(h - 1, 1));
            row[x * 4 + 2] = static_cast<uint8_t>(255 * x / std::max(w - 1, 1));
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// Random noise (worst case)
static Frame makeNoiseFrame(int w, int h, uint32_t seed = 42) {
    Frame f = makeBGRA(w, h);
    uint32_t state = seed;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            row[x * 4 + 0] = static_cast<uint8_t>(state);
            row[x * 4 + 1] = static_cast<uint8_t>(state >> 8);
            row[x * 4 + 2] = static_cast<uint8_t>(state >> 16);
            row[x * 4 + 3] = 255;
        }
    }
    return f;
}

// Desktop with taskbar and icons
static Frame makeDesktopFrame(int w, int h) {
    Frame f = makeBGRA(w, h);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        uint8_t blue = static_cast<uint8_t>(180 + 50 * y / std::max(h - 1, 1));
        uint8_t green = static_cast<uint8_t>(80 + 30 * y / std::max(h - 1, 1));
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = blue;
            row[x * 4 + 1] = green;
            row[x * 4 + 2] = 40;
            row[x * 4 + 3] = 255;
        }
    }
    for (int y = h - 48; y < h; ++y) {
        uint8_t* row = f.data.data() + y * f.stride;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = 40;
            row[x * 4 + 1] = 40;
            row[x * 4 + 2] = 40;
        }
    }
    for (int ir = 0; ir < 5; ++ir) {
        for (int ic = 0; ic < 2; ++ic) {
            int ix = 40 + ic * 90, iy = 40 + ir * 100;
            for (int dy = 0; dy < 48 && iy + dy < h - 48; ++dy) {
                uint8_t* row = f.data.data() + (iy + dy) * f.stride;
                for (int dx = 0; dx < 48 && ix + dx < w; ++dx) {
                    row[(ix + dx) * 4 + 0] = 180;
                    row[(ix + dx) * 4 + 1] = static_cast<uint8_t>(100 + ic * 80);
                    row[(ix + dx) * 4 + 2] = static_cast<uint8_t>(60 + ir * 40);
                }
            }
        }
    }
    return f;
}

// ---------------------------------------------------------------------------
// Color conversion helpers
// ---------------------------------------------------------------------------

static Frame bgraToI420(const Frame& bgra) {
    Frame out;
    out.allocate(bgra.width, bgra.height, PixelFormat::I420);
    out.timestampUs = bgra.timestampUs;
    out.frameId = bgra.frameId;

    int w = bgra.width, h = bgra.height;
    uint8_t* yP = out.plane(0);
    uint8_t* uP = out.plane(1);
    uint8_t* vP = out.plane(2);
    int yS = out.stride, uvS = out.stride / 2;

    for (int y = 0; y < h; ++y) {
        const uint8_t* src = bgra.data.data() + y * bgra.stride;
        for (int x = 0; x < w; ++x) {
            uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
            yP[y * yS + x] = static_cast<uint8_t>(
                std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
            if ((y % 2 == 0) && (x % 2 == 0)) {
                uP[(y / 2) * uvS + (x / 2)] = static_cast<uint8_t>(
                    std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
                vP[(y / 2) * uvS + (x / 2)] = static_cast<uint8_t>(
                    std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
            }
        }
    }
    return out;
}

// Convert I420 decoded output back to BGRA for apples-to-apples comparison
static Frame i420ToBGRA(const Frame& yuv) {
    Frame out;
    out.allocate(yuv.width, yuv.height, PixelFormat::BGRA);
    int w = yuv.width, h = yuv.height;
    int yS = yuv.stride, uvS = yuv.stride / 2;
    const uint8_t* yP = yuv.plane(0);
    const uint8_t* uP = yuv.plane(1);
    const uint8_t* vP = yuv.plane(2);

    for (int y = 0; y < h; ++y) {
        uint8_t* dst = out.data.data() + y * out.stride;
        for (int x = 0; x < w; ++x) {
            int Y = yP[y * yS + x] - 16;
            int U = uP[(y / 2) * uvS + (x / 2)] - 128;
            int V = vP[(y / 2) * uvS + (x / 2)] - 128;
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            dst[x * 4 + 3] = 255;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Quality metrics (computed on BGRA for fair comparison)
// ---------------------------------------------------------------------------

// PSNR across all RGB channels of a BGRA frame (ignoring alpha)
static double computePSNR_BGRA(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
    double mse = 0.0;
    int64_t count = 0;
    for (int y = 0; y < a.height; ++y) {
        const uint8_t* ra = a.data.data() + y * a.stride;
        const uint8_t* rb = b.data.data() + y * b.stride;
        for (int x = 0; x < a.width; ++x) {
            for (int c = 0; c < 3; ++c) {  // B, G, R only
                int d = static_cast<int>(ra[x * 4 + c]) - static_cast<int>(rb[x * 4 + c]);
                mse += d * d;
            }
            count += 3;
        }
    }
    mse /= static_cast<double>(count);
    if (mse < 1e-10) return 100.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// SSIM on luminance derived from BGRA (8x8 blocks)
static double computeSSIM_BGRA(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height) return 0.0;
    constexpr int kB = 8;
    constexpr double C1 = 6.5025, C2 = 58.5225;
    double ssimSum = 0.0;
    int blocks = 0;

    for (int by = 0; by + kB <= a.height; by += kB) {
        for (int bx = 0; bx + kB <= a.width; bx += kB) {
            double sA = 0, sB = 0, sA2 = 0, sB2 = 0, sAB = 0;
            for (int dy = 0; dy < kB; ++dy) {
                const uint8_t* ra = a.data.data() + (by + dy) * a.stride;
                const uint8_t* rb = b.data.data() + (by + dy) * b.stride;
                for (int dx = 0; dx < kB; ++dx) {
                    // BT.601 luma from BGR
                    double la = 0.114 * ra[(bx + dx) * 4 + 0] +
                                0.587 * ra[(bx + dx) * 4 + 1] +
                                0.299 * ra[(bx + dx) * 4 + 2];
                    double lb = 0.114 * rb[(bx + dx) * 4 + 0] +
                                0.587 * rb[(bx + dx) * 4 + 1] +
                                0.299 * rb[(bx + dx) * 4 + 2];
                    sA += la; sB += lb;
                    sA2 += la * la; sB2 += lb * lb;
                    sAB += la * lb;
                }
            }
            double n = kB * kB;
            double muA = sA / n, muB = sB / n;
            double sigA2 = sA2 / n - muA * muA;
            double sigB2 = sB2 / n - muB * muB;
            double sigAB = sAB / n - muA * muB;
            double num = (2.0 * muA * muB + C1) * (2.0 * sigAB + C2);
            double den = (muA * muA + muB * muB + C1) * (sigA2 + sigB2 + C2);
            ssimSum += num / den;
            ++blocks;
        }
    }
    return blocks > 0 ? ssimSum / blocks : 0.0;
}

// ---------------------------------------------------------------------------
// Content type enum for parameterized benchmarks
// ---------------------------------------------------------------------------

enum class TestContent : int {
    TEXT = 0,
    CODE_EDITOR = 1,
    GRADIENT = 2,
    NOISE = 3,
    DESKTOP = 4,
};

static const char* contentName(TestContent c) {
    switch (c) {
        case TestContent::TEXT:        return "text";
        case TestContent::CODE_EDITOR: return "code_editor";
        case TestContent::GRADIENT:    return "gradient";
        case TestContent::NOISE:       return "noise";
        case TestContent::DESKTOP:     return "desktop";
    }
    return "unknown";
}

static Frame makeTestFrame(TestContent c, int w, int h) {
    switch (c) {
        case TestContent::TEXT:        return makeTextFrame(w, h);
        case TestContent::CODE_EDITOR: return makeCodeFrame(w, h);
        case TestContent::GRADIENT:    return makeGradientFrame(w, h);
        case TestContent::NOISE:       return makeNoiseFrame(w, h);
        case TestContent::DESKTOP:     return makeDesktopFrame(w, h);
    }
    return makeTextFrame(w, h);
}

// ---------------------------------------------------------------------------
// OmniCodec encode benchmark
// ---------------------------------------------------------------------------

static void BM_OmniCodec_Encode(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    omni::OmniCodecEncoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    if (!enc.init(cfg)) {
        state.SkipWithError("OmniCodec encoder init failed");
        return;
    }

    Frame frame = makeTestFrame(content, w, h);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    // Measure keyframe encoding — request keyframe each iteration
    // to benchmark actual encoding work (not just SKIP detection).
    size_t totalEncoded = 0;
    for (auto _ : state) {
        enc.requestKeyFrame();
        frame.timestampUs += 16667;
        enc.encode(frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
        totalEncoded += pkt.data.size();
    }

    // Report metrics
    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.counters["compressed_KB"] = benchmark::Counter(
        static_cast<double>(pkt.data.size()) / 1024.0);
    double rawSize = static_cast<double>(w) * h * 4;
    state.counters["ratio"] = benchmark::Counter(rawSize / std::max(1.0, static_cast<double>(pkt.data.size())));

    // Quality: decode the last keyframe and measure
    omni::OmniCodecDecoder dec;
    dec.init(w, h);
    Frame decoded;
    if (dec.decode(pkt.data.data(), pkt.data.size(), decoded)) {
        state.counters["PSNR"] = benchmark::Counter(computePSNR_BGRA(frame, decoded));
        state.counters["SSIM"] = benchmark::Counter(computeSSIM_BGRA(frame, decoded));
    }

    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// OpenH264 encode benchmark
// ---------------------------------------------------------------------------

static void BM_OpenH264_Encode(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    cfg.screenContent = true;
    if (!enc.init(cfg)) {
        state.SkipWithError("OpenH264 encoder init failed");
        return;
    }

    Frame bgraFrame = makeTestFrame(content, w, h);
    Frame i420Frame = bgraToI420(bgraFrame);
    i420Frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    // Warm up
    enc.requestKeyFrame();
    enc.encode(i420Frame, regions, pkt);

    for (auto _ : state) {
        i420Frame.timestampUs += 16667;
        enc.encode(i420Frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.counters["compressed_KB"] = benchmark::Counter(
        static_cast<double>(pkt.data.size()) / 1024.0);
    double rawSize = static_cast<double>(w) * h * 4;
    state.counters["ratio"] = benchmark::Counter(rawSize / std::max(1.0, static_cast<double>(pkt.data.size())));

    // Quality: decode and convert back to BGRA for comparison
    OpenH264Decoder dec;
    dec.init(w, h);
    enc.requestKeyFrame();
    i420Frame.timestampUs += 16667;
    enc.encode(i420Frame, regions, pkt);
    Frame decodedI420;
    if (dec.decode(pkt.data.data(), pkt.data.size(), decodedI420)) {
        Frame decodedBGRA = i420ToBGRA(decodedI420);
        state.counters["PSNR"] = benchmark::Counter(computePSNR_BGRA(bgraFrame, decodedBGRA));
        state.counters["SSIM"] = benchmark::Counter(computeSSIM_BGRA(bgraFrame, decodedBGRA));
    }

    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// OmniCodec decode benchmark
// ---------------------------------------------------------------------------

static void BM_OmniCodec_Decode(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    omni::OmniCodecEncoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    if (!enc.init(cfg)) {
        state.SkipWithError("OmniCodec encoder init failed");
        return;
    }

    Frame frame = makeTestFrame(content, w, h);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;
    enc.requestKeyFrame();
    enc.encode(frame, regions, pkt);

    omni::OmniCodecDecoder dec;
    dec.init(w, h);
    Frame decoded;

    for (auto _ : state) {
        dec.reset();
        dec.init(w, h);
        dec.decode(pkt.data.data(), pkt.data.size(), decoded);
        benchmark::DoNotOptimize(decoded);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// OpenH264 decode benchmark
// ---------------------------------------------------------------------------

static void BM_OpenH264_Decode(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    cfg.screenContent = true;
    if (!enc.init(cfg)) {
        state.SkipWithError("OpenH264 encoder init failed");
        return;
    }

    Frame bgraFrame = makeTestFrame(content, w, h);
    Frame i420Frame = bgraToI420(bgraFrame);
    i420Frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;
    enc.requestKeyFrame();
    enc.encode(i420Frame, regions, pkt);

    OpenH264Decoder dec;
    if (!dec.init(w, h)) {
        state.SkipWithError("OpenH264 decoder init failed");
        return;
    }

    Frame decoded;
    for (auto _ : state) {
        dec.decode(pkt.data.data(), pkt.data.size(), decoded);
        benchmark::DoNotOptimize(decoded);
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// OmniCodec multi-frame encode (measures skip detection benefit)
// ---------------------------------------------------------------------------

static void BM_OmniCodec_MultiFrame(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    omni::OmniCodecEncoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    if (!enc.init(cfg)) {
        state.SkipWithError("OmniCodec encoder init failed");
        return;
    }

    Frame frame = makeTestFrame(content, w, h);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    // First frame is keyframe
    enc.requestKeyFrame();
    enc.encode(frame, regions, pkt);
    size_t keyFrameSize = pkt.data.size();

    // Subsequent frames of identical content should use SKIP tiles
    size_t pFrameSize = 0;
    for (auto _ : state) {
        frame.timestampUs += 16667;
        enc.encode(frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
        pFrameSize = pkt.data.size();
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.counters["keyframe_KB"] = benchmark::Counter(
        static_cast<double>(keyFrameSize) / 1024.0);
    state.counters["pframe_KB"] = benchmark::Counter(
        static_cast<double>(pFrameSize) / 1024.0);
    // Skip efficiency: how much smaller P-frames are vs keyframe
    if (keyFrameSize > 0) {
        state.counters["skip_savings_%"] = benchmark::Counter(
            100.0 * (1.0 - static_cast<double>(pFrameSize) / keyFrameSize));
    }
    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// OpenH264 multi-frame encode (P-frame efficiency)
// ---------------------------------------------------------------------------

static void BM_OpenH264_MultiFrame(benchmark::State& state) {
    int w = static_cast<int>(state.range(0));
    int h = static_cast<int>(state.range(1));
    auto content = static_cast<TestContent>(state.range(2));
    uint32_t bitrate = static_cast<uint32_t>(state.range(3));

    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = w;
    cfg.height = h;
    cfg.targetBitrateBps = bitrate;
    cfg.maxBitrateBps = bitrate * 2;
    cfg.maxFps = 60;
    cfg.screenContent = true;
    if (!enc.init(cfg)) {
        state.SkipWithError("OpenH264 encoder init failed");
        return;
    }

    Frame bgraFrame = makeTestFrame(content, w, h);
    Frame i420Frame = bgraToI420(bgraFrame);
    i420Frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    enc.requestKeyFrame();
    enc.encode(i420Frame, regions, pkt);
    size_t keyFrameSize = pkt.data.size();

    size_t pFrameSize = 0;
    for (auto _ : state) {
        i420Frame.timestampUs += 16667;
        enc.encode(i420Frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
        pFrameSize = pkt.data.size();
    }

    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
    state.counters["keyframe_KB"] = benchmark::Counter(
        static_cast<double>(keyFrameSize) / 1024.0);
    state.counters["pframe_KB"] = benchmark::Counter(
        static_cast<double>(pFrameSize) / 1024.0);
    if (keyFrameSize > 0) {
        state.counters["skip_savings_%"] = benchmark::Counter(
            100.0 * (1.0 - static_cast<double>(pFrameSize) / keyFrameSize));
    }
    state.SetLabel(contentName(content));
}

// ---------------------------------------------------------------------------
// Register benchmarks: {width, height, content, bitrate}
// ---------------------------------------------------------------------------

// Helper to register all content types for a given codec benchmark function
#define REGISTER_CODEC_BENCH(fn, w, h, bitrate) \
    BENCHMARK(fn)->Args({w, h, (int)TestContent::TEXT,        bitrate})->Unit(benchmark::kMillisecond); \
    BENCHMARK(fn)->Args({w, h, (int)TestContent::CODE_EDITOR, bitrate})->Unit(benchmark::kMillisecond); \
    BENCHMARK(fn)->Args({w, h, (int)TestContent::GRADIENT,    bitrate})->Unit(benchmark::kMillisecond); \
    BENCHMARK(fn)->Args({w, h, (int)TestContent::NOISE,       bitrate})->Unit(benchmark::kMillisecond); \
    BENCHMARK(fn)->Args({w, h, (int)TestContent::DESKTOP,     bitrate})->Unit(benchmark::kMillisecond);

// --- 1080p @ 2 Mbps ---
REGISTER_CODEC_BENCH(BM_OmniCodec_Encode,  1920, 1080, 2000000)
REGISTER_CODEC_BENCH(BM_OpenH264_Encode,    1920, 1080, 2000000)

REGISTER_CODEC_BENCH(BM_OmniCodec_Decode,  1920, 1080, 2000000)
REGISTER_CODEC_BENCH(BM_OpenH264_Decode,    1920, 1080, 2000000)

REGISTER_CODEC_BENCH(BM_OmniCodec_MultiFrame,  1920, 1080, 2000000)
REGISTER_CODEC_BENCH(BM_OpenH264_MultiFrame,    1920, 1080, 2000000)

// --- 720p @ 1 Mbps ---
REGISTER_CODEC_BENCH(BM_OmniCodec_Encode,  1280, 720, 1000000)
REGISTER_CODEC_BENCH(BM_OpenH264_Encode,    1280, 720, 1000000)

// --- 4K @ 8 Mbps ---
REGISTER_CODEC_BENCH(BM_OmniCodec_Encode,  3840, 2160, 8000000)
REGISTER_CODEC_BENCH(BM_OpenH264_Encode,    3840, 2160, 8000000)
