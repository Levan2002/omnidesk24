#pragma once

#include "core/types.h"
#include "codec/encoder.h"
#include <cstdint>
#include <memory>
#include <vector>

// OpenH264 forward declarations
class ISVCEncoder;

namespace omnidesk {

// H.264 encoder using Cisco OpenH264.
// Configured for screen content: Constrained Baseline profile, single slice,
// no B-frames, CBR rate control, adaptive quantization, LTR frames,
// and SVC temporal layers for graceful degradation.
class OpenH264Encoder : public IEncoder {
public:
    OpenH264Encoder();
    ~OpenH264Encoder() override;

    // Non-copyable, movable
    OpenH264Encoder(const OpenH264Encoder&) = delete;
    OpenH264Encoder& operator=(const OpenH264Encoder&) = delete;
    OpenH264Encoder(OpenH264Encoder&& other) noexcept;
    OpenH264Encoder& operator=(OpenH264Encoder&& other) noexcept;

    bool init(const EncoderConfig& cfg) override;
    bool encode(const Frame& frame, const std::vector<RegionInfo>& regions,
                EncodedPacket& out) override;
    void requestKeyFrame() override;
    void updateBitrate(uint32_t bps) override;
    EncoderInfo getInfo() override;

private:
    void destroy();

    ISVCEncoder* encoder_ = nullptr;
    EncoderConfig config_{};
    bool keyFrameRequested_ = false;
    uint64_t frameIndex_ = 0;
};

} // namespace omnidesk
