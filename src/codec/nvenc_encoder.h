#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include "codec/decoder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef OMNIDESK_HAS_NVENC

namespace omnidesk {

// Forward-declare our minimal NVENC API wrappers (defined in the .cpp)
struct NvencFunctions;

// H.264 hardware encoder using NVIDIA NVENC.
// Uses low-latency preset with CBR rate control.
// Loads the NVENC library at runtime via dlopen/LoadLibrary
// to avoid requiring the NVENC SDK headers at compile time.
class NvencEncoder : public IEncoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    bool init(const EncoderConfig& cfg) override;
    bool encode(const Frame& frame, const std::vector<RegionInfo>& regions,
                EncodedPacket& out) override;
    void requestKeyFrame() override;
    void updateBitrate(uint32_t bps) override;
    EncoderInfo getInfo() override;

private:
    bool loadLibrary();
    bool initCuda();
    bool openSession();
    bool configureEncoder();
    bool createBuffers();
    void uploadFrame(const Frame& frame);
    void destroy();

    // CUDA
    void* cuContext_ = nullptr;

    // NVENC library and function table
    void* nvencLib_ = nullptr;
    void* cudaLib_ = nullptr;
    std::unique_ptr<NvencFunctions> fn_;
    void* encoder_ = nullptr;

    // Registered input/output resources
    void* registeredInput_ = nullptr;
    void* mappedInput_ = nullptr;
    void* bitstreamBuffer_ = nullptr;

    // System-memory input buffer (NV12)
    std::vector<uint8_t> nv12Buffer_;
    uint32_t nv12Pitch_ = 0;

    EncoderConfig config_{};
    bool initialized_ = false;
    std::atomic<bool> keyFrameRequested_{true};
    uint64_t frameIndex_ = 0;
};

// Decoder using NVIDIA CUVID/NVDEC.
class NvdecDecoder : public IDecoder {
public:
    NvdecDecoder();
    ~NvdecDecoder() override;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    void destroy();

    void* libHandle_ = nullptr;
    void* decoder_ = nullptr;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace omnidesk

#endif // OMNIDESK_HAS_NVENC
