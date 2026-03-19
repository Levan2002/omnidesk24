#pragma once

#include "core/types.h"
#include <cstdint>
#include <vector>

namespace omnidesk {

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool init(const EncoderConfig& cfg) = 0;
    virtual bool encode(const Frame& frame, const std::vector<RegionInfo>& regions, EncodedPacket& out) = 0;
    virtual void requestKeyFrame() = 0;
    virtual void updateBitrate(uint32_t bps) = 0;
    virtual EncoderInfo getInfo() = 0;

    // Lightweight resolution change without full encoder teardown/rebuild.
    // Returns true if the encoder was successfully reconfigured in-place.
    // Default returns false — callers fall back to init().
    virtual bool reconfigure(int /*width*/, int /*height*/) { return false; }
};

} // namespace omnidesk
