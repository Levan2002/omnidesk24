#include "codec/omni/bitstream.h"
#include <algorithm>
#include <cstring>

namespace omnidesk {
namespace omni {

// ---- BitstreamWriter ----

BitstreamWriter::BitstreamWriter() {
    buf_.reserve(4096);
}

BitstreamWriter::BitstreamWriter(size_t reserveBytes) {
    buf_.reserve(reserveBytes);
}

void BitstreamWriter::writeU8(uint8_t val) {
    flushBits();
    buf_.push_back(val);
}

void BitstreamWriter::writeU16(uint16_t val) {
    flushBits();
    buf_.push_back(static_cast<uint8_t>(val >> 8));
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
}

void BitstreamWriter::writeU32(uint32_t val) {
    flushBits();
    buf_.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>(val & 0xFF));
}

void BitstreamWriter::writeBytes(const uint8_t* data, size_t count) {
    flushBits();
    buf_.insert(buf_.end(), data, data + count);
}

void BitstreamWriter::writeBits(uint8_t val, int numBits) {
    for (int i = numBits - 1; i >= 0; --i) {
        bitBuf_ = (bitBuf_ << 1) | ((val >> i) & 1);
        ++bitPos_;
        if (bitPos_ == 8) {
            buf_.push_back(bitBuf_);
            bitBuf_ = 0;
            bitPos_ = 0;
        }
    }
}

void BitstreamWriter::flushBits() {
    if (bitPos_ > 0) {
        bitBuf_ <<= (8 - bitPos_);
        buf_.push_back(bitBuf_);
        bitBuf_ = 0;
        bitPos_ = 0;
    }
}

void BitstreamWriter::clear() {
    buf_.clear();
    bitBuf_ = 0;
    bitPos_ = 0;
}

// ---- BitstreamReader ----

BitstreamReader::BitstreamReader(const uint8_t* data, size_t size)
    : data_(data), size_(size) {}

uint8_t BitstreamReader::readU8() {
    alignToByte();
    if (pos_ >= size_) { error_ = true; return 0; }
    return data_[pos_++];
}

uint16_t BitstreamReader::readU16() {
    alignToByte();
    if (pos_ + 2 > size_) { error_ = true; return 0; }
    uint16_t val = (static_cast<uint16_t>(data_[pos_]) << 8) |
                    static_cast<uint16_t>(data_[pos_ + 1]);
    pos_ += 2;
    return val;
}

uint32_t BitstreamReader::readU32() {
    alignToByte();
    if (pos_ + 4 > size_) { error_ = true; return 0; }
    uint32_t val = (static_cast<uint32_t>(data_[pos_]) << 24) |
                   (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                   (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
                    static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ += 4;
    return val;
}

void BitstreamReader::readBytes(uint8_t* dst, size_t count) {
    alignToByte();
    if (pos_ + count > size_) { error_ = true; return; }
    std::memcpy(dst, data_ + pos_, count);
    pos_ += count;
}

uint8_t BitstreamReader::readBits(int numBits) {
    uint8_t result = 0;
    for (int i = 0; i < numBits; ++i) {
        if (bitPos_ == 0) {
            if (pos_ >= size_) { error_ = true; return 0; }
            bitBuf_ = data_[pos_++];
            bitPos_ = 8;
        }
        --bitPos_;
        result = (result << 1) | ((bitBuf_ >> bitPos_) & 1);
    }
    return result;
}

void BitstreamReader::alignToByte() {
    bitPos_ = 0;
}

size_t BitstreamReader::remaining() const {
    return (pos_ < size_) ? (size_ - pos_) : 0;
}

} // namespace omni
} // namespace omnidesk
