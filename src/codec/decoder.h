#pragma once

#include "core/types.h"
#include <cstdint>
#include <cstddef>

namespace omnidesk {

class IDecoder {
public:
    virtual ~IDecoder() = default;
    virtual bool init(int width, int height) = 0;
    virtual bool decode(const uint8_t* data, size_t size, Frame& out) = 0;
    virtual void reset() = 0;
};

} // namespace omnidesk
