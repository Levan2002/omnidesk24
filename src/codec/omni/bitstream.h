#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace omnidesk {
namespace omni {

// Byte-aligned bitstream writer for frame assembly.
class BitstreamWriter {
public:
    BitstreamWriter();
    explicit BitstreamWriter(size_t reserveBytes);

    void writeU8(uint8_t val);
    void writeU16(uint16_t val);
    void writeU32(uint32_t val);
    void writeBytes(const uint8_t* data, size_t count);

    // Write N bits (1-8) from the least significant bits of val.
    void writeBits(uint8_t val, int numBits);

    // Flush any partial bit byte (pads with zeros).
    void flushBits();

    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }
    size_t size() const { return buf_.size(); }

    void clear();

private:
    std::vector<uint8_t> buf_;
    uint8_t bitBuf_ = 0;
    int bitPos_ = 0;  // bits written in current byte (0-7)
};

// Byte-aligned bitstream reader for frame parsing.
class BitstreamReader {
public:
    BitstreamReader(const uint8_t* data, size_t size);

    uint8_t readU8();
    uint16_t readU16();
    uint32_t readU32();
    void readBytes(uint8_t* dst, size_t count);

    // Read N bits (1-8), returned in the least significant bits.
    uint8_t readBits(int numBits);

    // Skip to next byte boundary.
    void alignToByte();

    size_t remaining() const;
    size_t position() const { return pos_; }
    bool hasError() const { return error_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    uint8_t bitBuf_ = 0;
    int bitPos_ = 0;  // bits remaining in current cached byte
    bool error_ = false;
};

} // namespace omni
} // namespace omnidesk
