#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include "codec/decoder.h"

#include <cstdint>
#include <memory>
#include <vector>

#ifdef OMNIDESK_PLATFORM_WINDOWS

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>

namespace omnidesk {

// H.264 hardware encoder using Windows Media Foundation.
// Uses the platform's hardware MFT (Intel QSV, AMD AMF, etc.).
// CBR rate control with low-latency mode.
class MFEncoder : public IEncoder {
public:
    MFEncoder();
    ~MFEncoder() override;

    MFEncoder(const MFEncoder&) = delete;
    MFEncoder& operator=(const MFEncoder&) = delete;

    bool init(const EncoderConfig& cfg) override;
    bool encode(const Frame& frame, const std::vector<RegionInfo>& regions,
                EncodedPacket& out) override;
    void requestKeyFrame() override;
    void updateBitrate(uint32_t bps) override;
    EncoderInfo getInfo() override;

private:
    bool findHardwareMFT();
    bool configureMFT();
    bool createInputSample(const Frame& frame);
    void destroy();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaType> inputType_;
    Microsoft::WRL::ComPtr<IMFMediaType> outputType_;
    Microsoft::WRL::ComPtr<IMFSample> inputSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inputBuffer_;

    EncoderConfig config_{};
    bool initialized_ = false;
    bool keyFrameRequested_ = true;
    uint64_t frameIndex_ = 0;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
};

// H.264 hardware decoder using Windows Media Foundation.
class MFDecoder : public IDecoder {
public:
    MFDecoder();
    ~MFDecoder() override;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    void destroy();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;
};

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
