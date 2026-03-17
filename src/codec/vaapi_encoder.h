#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include "codec/decoder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef OMNIDESK_HAS_VAAPI

#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_drm.h>

namespace omnidesk {

// H.264 hardware encoder using VA-API (Intel/AMD on Linux).
// Low-power encoding path for minimal latency.
// CBR rate control, no B-frames, single reference frame.
class VaapiEncoder : public IEncoder {
public:
    VaapiEncoder();
    ~VaapiEncoder() override;

    VaapiEncoder(const VaapiEncoder&) = delete;
    VaapiEncoder& operator=(const VaapiEncoder&) = delete;

    bool init(const EncoderConfig& cfg) override;
    bool encode(const Frame& frame, const std::vector<RegionInfo>& regions,
                EncodedPacket& out) override;
    void requestKeyFrame() override;
    void updateBitrate(uint32_t bps) override;
    EncoderInfo getInfo() override;

private:
    bool openDevice();
    bool createConfig();
    bool createSurfaces();
    bool createContext();
    bool createCodedBuffer();
    void uploadFrame(const Frame& frame);
    bool renderPicture(bool isIDR);
    bool extractNALs(EncodedPacket& out);
    void destroy();

    int drmFd_ = -1;
    VADisplay display_ = nullptr;
    VAConfigID configId_ = VA_INVALID_ID;
    VAContextID contextId_ = VA_INVALID_ID;
    VASurfaceID inputSurface_ = VA_INVALID_ID;
    VASurfaceID refSurface_ = VA_INVALID_ID;
    VASurfaceID reconSurface_ = VA_INVALID_ID;
    VABufferID codedBuf_ = VA_INVALID_ID;

    EncoderConfig config_{};
    bool initialized_ = false;
    std::atomic<bool> keyFrameRequested_{true};
    uint64_t frameIndex_ = 0;
    uint32_t idrPeriod_ = 300;
};

// H.264 hardware decoder using VA-API.
class VaapiDecoder : public IDecoder {
public:
    VaapiDecoder();
    ~VaapiDecoder() override;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    void destroy();

    int drmFd_ = -1;
    VADisplay display_ = nullptr;
    VAConfigID configId_ = VA_INVALID_ID;
    VAContextID contextId_ = VA_INVALID_ID;
    VASurfaceID surface_ = VA_INVALID_ID;
    VABufferID sliceBuf_ = VA_INVALID_ID;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace omnidesk

#endif // OMNIDESK_HAS_VAAPI
