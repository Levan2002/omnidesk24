#include "codec/nvenc_encoder.h"
#include "core/logger.h"

#ifdef OMNIDESK_HAS_NVENC

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>

namespace omnidesk {

// NVENC API types (minimal subset to avoid SDK header dependency)
// The actual NVENC SDK headers would be needed for a full implementation.
// This implementation uses dlopen/LoadLibrary to load the NVENC library at runtime.

NvencEncoder::NvencEncoder() = default;

NvencEncoder::~NvencEncoder() {
    destroy();
}

bool NvencEncoder::loadLibrary() {
#ifdef _WIN32
    libHandle_ = LoadLibraryA("nvEncodeAPI64.dll");
    if (!libHandle_) {
        libHandle_ = LoadLibraryA("nvEncodeAPI.dll");
    }
#else
    libHandle_ = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
    if (!libHandle_) {
        libHandle_ = dlopen("libnvidia-encode.so", RTLD_LAZY);
    }
#endif
    return libHandle_ != nullptr;
}

bool NvencEncoder::openSession() {
    if (!libHandle_) return false;

    // In a full implementation, we would:
    // 1. Get NvEncodeAPICreateInstance function pointer
    // 2. Create NV_ENCODE_API_FUNCTION_LIST
    // 3. Call nvEncOpenEncodeSession with CUDA device
    // 4. Configure encoder with NV_ENC_INITIALIZE_PARAMS:
    //    - encodeGUID = NV_ENC_CODEC_H264_GUID
    //    - presetGUID = NV_ENC_PRESET_P1_GUID (fastest)
    //    - tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY
    //    - frameRateNum = config_.maxFps
    //    - encodeWidth/Height = config_.width/height

    LOG_INFO("NVENC: session opened (would initialize NVENC SDK here)");
    return true;
}

bool NvencEncoder::initEncoder() {
    // Full implementation would configure:
    // NV_ENC_CONFIG encConfig
    // encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR
    // encConfig.rcParams.averageBitRate = config_.bitrateBps
    // encConfig.rcParams.maxBitRate = config_.bitrateBps * 3 / 2
    // encConfig.rcParams.vbvBufferSize = config_.bitrateBps / config_.maxFps
    // encConfig.encodeCodecConfig.h264Config:
    //   - idrPeriod = 300
    //   - enableIntraRefresh = 1  (gradual IDR)
    //   - intraRefreshPeriod = 60
    //   - sliceMode = 0  (single slice)
    //   - repeatSPSPPS = 1
    //   - disableDeblockingFilter = 0
    //   - enableAQ = 1 (adaptive quantization)
    //   - aqStrength = 8

    return true;
}

bool NvencEncoder::createBuffers() {
    // Full implementation would:
    // 1. Create input buffer: nvEncCreateInputBuffer (NV12 format)
    // 2. Create output bitstream buffer: nvEncCreateBitstreamBuffer
    return true;
}

bool NvencEncoder::init(const EncoderConfig& cfg) {
    config_ = cfg;

    // NVENC is a stub — the SDK integration is not yet implemented.
    // Return false so the codec factory falls through to OpenH264.
    LOG_INFO("NVENC: stub encoder, falling back to software");
    return false;
}

bool NvencEncoder::encode(const Frame& frame,
                           const std::vector<RegionInfo>& /*regions*/,
                           EncodedPacket& out) {
    if (!initialized_) return false;

    // Full implementation would:
    // 1. Lock input buffer: nvEncLockInputBuffer
    // 2. Copy I420 frame to NV12 input buffer
    // 3. Unlock: nvEncUnlockInputBuffer
    // 4. Set encode params:
    //    - inputBuffer, outputBitstream
    //    - pictureStruct = NV_ENC_PIC_STRUCT_FRAME
    //    - encodePicFlags = keyFrameRequested_ ? NV_ENC_PIC_FLAG_FORCEIDR : 0
    // 5. Call nvEncEncodePicture
    // 6. Lock output: nvEncLockBitstream
    // 7. Copy NAL data to out.data
    // 8. Unlock: nvEncUnlockBitstream

    (void)frame;

    out.frameId = frameIndex_;
    out.isKeyFrame = keyFrameRequested_;
    out.temporalLayer = 0;
    out.timestampUs = frame.timestampUs;

    keyFrameRequested_ = false;
    frameIndex_++;

    LOG_WARN("NVENC: encode stub called - needs NVENC SDK integration");
    return false;  // Return false until fully implemented
}

void NvencEncoder::requestKeyFrame() {
    keyFrameRequested_ = true;
}

void NvencEncoder::updateBitrate(uint32_t bps) {
    config_.targetBitrateBps = bps;
    // Full implementation: nvEncReconfigureEncoder with new bitrate
}

EncoderInfo NvencEncoder::getInfo() {
    EncoderInfo info;
    info.name = "NVENC H.264";
    info.isHardware = true;
    info.maxWidth = config_.width;
    info.maxHeight = config_.height;
    return info;
}

void NvencEncoder::destroy() {
    if (encoder_) {
        // nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    if (libHandle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(libHandle_));
#else
        dlclose(libHandle_);
#endif
        libHandle_ = nullptr;
    }
    initialized_ = false;
}

// ---- NvdecDecoder ----

NvdecDecoder::NvdecDecoder() = default;
NvdecDecoder::~NvdecDecoder() { destroy(); }

bool NvdecDecoder::init(int width, int height) {
    width_ = width;
    height_ = height;

    // Full implementation would:
    // 1. Load cuvid library
    // 2. Create CUDA context
    // 3. Create CUVID parser (cudaVideoCodec_H264)
    // 4. Create CUVID decoder

    LOG_WARN("NVDEC: decoder stub - needs CUVID SDK integration");
    return false;
}

bool NvdecDecoder::decode(const uint8_t* /*data*/, size_t /*size*/, Frame& /*out*/) {
    return false;
}

void NvdecDecoder::reset() {}

void NvdecDecoder::destroy() {
    if (libHandle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(libHandle_));
#else
        dlclose(libHandle_);
#endif
        libHandle_ = nullptr;
    }
    initialized_ = false;
}

} // namespace omnidesk

#endif // OMNIDESK_HAS_NVENC
