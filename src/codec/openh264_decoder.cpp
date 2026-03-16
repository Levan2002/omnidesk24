#include "codec/openh264_decoder.h"
#include "core/logger.h"

#include "codec/openh264_loader.h"
#include <wels/codec_def.h>
#include <cstring>

namespace omnidesk {

OpenH264Decoder::OpenH264Decoder() = default;

OpenH264Decoder::~OpenH264Decoder() {
    destroy();
}

OpenH264Decoder::OpenH264Decoder(OpenH264Decoder&& other) noexcept
    : decoder_(other.decoder_)
    , width_(other.width_)
    , height_(other.height_) {
    other.decoder_ = nullptr;
}

OpenH264Decoder& OpenH264Decoder::operator=(OpenH264Decoder&& other) noexcept {
    if (this != &other) {
        destroy();
        decoder_ = other.decoder_;
        width_ = other.width_;
        height_ = other.height_;
        other.decoder_ = nullptr;
    }
    return *this;
}

void OpenH264Decoder::destroy() {
    if (decoder_) {
        decoder_->Uninitialize();
        WelsDestroyDecoder(decoder_);
        decoder_ = nullptr;
    }
}

bool OpenH264Decoder::init(int width, int height) {
    destroy();
    width_ = width;
    height_ = height;

    if (!openh264_load()) {
        LOG_WARN("OpenH264 library not available");
        return false;
    }

    if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) {
        return false;
    }

    SDecodingParam param;
    std::memset(&param, 0, sizeof(param));

    // Low-latency parsing mode: decode NAL-by-NAL without buffering.
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly = false;

    if (decoder_->Initialize(&param) != cmResultSuccess) {
        destroy();
        return false;
    }

    // Enable low-latency mode: output frames immediately.
    int32_t latencyFlag = 1;
    decoder_->SetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &latencyFlag);

    return true;
}

bool OpenH264Decoder::decode(const uint8_t* data, size_t size, Frame& out) {
    if (!decoder_ || !data || size == 0) {
        return false;
    }

    uint8_t* yuvData[3] = {nullptr, nullptr, nullptr};
    SBufferInfo bufInfo;
    std::memset(&bufInfo, 0, sizeof(bufInfo));

    DECODING_STATE state = decoder_->DecodeFrameNoDelay(
        data, static_cast<int>(size), yuvData, &bufInfo);

    if (state != dsErrorFree) {
        return false;
    }

    if (bufInfo.iBufferStatus != 1) {
        // No frame output yet (e.g., buffering SPS/PPS).
        return false;
    }

    // Extract decoded I420 frame.
    const int decodedWidth = bufInfo.UsrData.sSystemBuffer.iWidth;
    const int decodedHeight = bufInfo.UsrData.sSystemBuffer.iHeight;
    const int yStride = bufInfo.UsrData.sSystemBuffer.iStride[0];
    const int uvStride = bufInfo.UsrData.sSystemBuffer.iStride[1];

    out.allocate(decodedWidth, decodedHeight, PixelFormat::I420);

    // Copy Y plane
    const uint8_t* srcY = yuvData[0];
    uint8_t* dstY = out.plane(0);
    for (int y = 0; y < decodedHeight; ++y) {
        std::memcpy(dstY + y * out.stride, srcY + y * yStride,
                    static_cast<size_t>(decodedWidth));
    }

    // Copy U plane
    const int uvHeight = decodedHeight / 2;
    const int uvWidth = decodedWidth / 2;
    const uint8_t* srcU = yuvData[1];
    uint8_t* dstU = out.plane(1);
    for (int y = 0; y < uvHeight; ++y) {
        std::memcpy(dstU + y * (out.stride / 2), srcU + y * uvStride,
                    static_cast<size_t>(uvWidth));
    }

    // Copy V plane
    const uint8_t* srcV = yuvData[2];
    uint8_t* dstV = out.plane(2);
    for (int y = 0; y < uvHeight; ++y) {
        std::memcpy(dstV + y * (out.stride / 2), srcV + y * uvStride,
                    static_cast<size_t>(uvWidth));
    }

    return true;
}

void OpenH264Decoder::reset() {
    if (decoder_) {
        // Re-initialize the decoder to flush all internal state.
        int w = width_;
        int h = height_;
        destroy();
        init(w, h);
    }
}

} // namespace omnidesk
