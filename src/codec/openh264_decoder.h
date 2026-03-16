#pragma once

#include "core/types.h"
#include "codec/decoder.h"
#include <cstdint>
#include <cstddef>

// OpenH264 forward declaration
class ISVCDecoder;

namespace omnidesk {

// H.264 decoder using Cisco OpenH264.
// Configured for low-latency decoding of NAL units to I420 output.
class OpenH264Decoder : public IDecoder {
public:
    OpenH264Decoder();
    ~OpenH264Decoder() override;

    // Non-copyable, movable
    OpenH264Decoder(const OpenH264Decoder&) = delete;
    OpenH264Decoder& operator=(const OpenH264Decoder&) = delete;
    OpenH264Decoder(OpenH264Decoder&& other) noexcept;
    OpenH264Decoder& operator=(OpenH264Decoder&& other) noexcept;

    bool init(int width, int height) override;
    bool decode(const uint8_t* data, size_t size, Frame& out) override;
    void reset() override;

private:
    void destroy();

    ISVCDecoder* decoder_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

} // namespace omnidesk
