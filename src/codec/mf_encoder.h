#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include "codec/decoder.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef OMNIDESK_PLATFORM_WINDOWS

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>

namespace omnidesk {

// H.264 encoder using Windows Media Foundation.
// Automatically picks up Intel QSV, AMD AMF, or any Windows-registered
// H.264 hardware MFT.  Falls back to a software MFT if no HW is found.
// Tries both NV12 and I420 input subtypes for maximum compatibility.
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
    bool findEncoderMFT();
    bool configureMFT();
    bool tryInputSubtype(const GUID& subtype);
    bool createInputSample(const Frame& frame);
    void destroy();

    // Identify the GPU vendor behind the activated MFT.
    void identifyMFT();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    Microsoft::WRL::ComPtr<IMFMediaType> inputType_;
    Microsoft::WRL::ComPtr<IMFMediaType> outputType_;
    Microsoft::WRL::ComPtr<IMFSample> inputSample_;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> inputBuffer_;

    EncoderConfig config_{};
    bool initialized_ = false;
    bool isHardware_ = false;
    bool inputIsNV12_ = true;    // true = NV12 input, false = I420 input
    std::string mftName_;        // e.g. "Intel QSV", "AMD AMF", "MF Software"
    std::atomic<bool> keyFrameRequested_{true};
    uint64_t frameIndex_ = 0;
    DWORD inputStreamId_ = 0;
    DWORD outputStreamId_ = 0;
};

// H.264 decoder using Windows Media Foundation.
// Tries hardware MFT first, then sync software MFT, then async software MFT.
// Handles 0x0 initial dimensions (defaults to 1920x1080 for buffer sizing).
// Gracefully handles MF_E_TRANSFORM_NEED_MORE_INPUT and format changes.
class MFDecoder : public IDecoder {
public:
    MFDecoder();
    ~MFDecoder() override;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    bool configureTypes();
    bool handleFormatChange();
    void destroy();

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    bool initialized_ = false;
    bool typesConfigured_ = false;
    int width_ = 0;
    int height_ = 0;
    uint64_t inputCount_ = 0;   // frames fed — used for log throttling
};

} // namespace omnidesk

#endif // OMNIDESK_PLATFORM_WINDOWS
