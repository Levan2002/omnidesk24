#include "codec/omni/rans_codec.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace omnidesk {
namespace omni {

// ---- Frequency Table Construction ----

static void buildFreqTableCore(const uint32_t* counts, int alphabetSize,
                                RANSSymbol* table) {
    // Sum total
    uint64_t total = 0;
    for (int i = 0; i < alphabetSize; ++i) {
        total += counts[i];
    }

    if (total == 0) {
        // Uniform distribution fallback
        uint16_t freqEach = static_cast<uint16_t>(RANS_SCALE / alphabetSize);
        uint16_t remainder = static_cast<uint16_t>(RANS_SCALE - freqEach * alphabetSize);
        uint16_t cum = 0;
        for (int i = 0; i < alphabetSize; ++i) {
            uint16_t f = freqEach + (i < remainder ? 1 : 0);
            table[i].freq = f;
            table[i].cumFreq = cum;
            // Precompute reciprocal: floor(2^32 / freq), clamped
            uint64_t rcp = (static_cast<uint64_t>(1) << 32) / f;
            table[i].rcpFreq = (rcp > UINT32_MAX) ? UINT32_MAX
                                                    : static_cast<uint32_t>(rcp);
            table[i].rcpShift = 32;
            cum += f;
        }
        return;
    }

    // Normalize frequencies to sum to RANS_SCALE.
    uint32_t scaled[256] = {};  // stack-allocated, no heap
    uint32_t scaledSum = 0;

    for (int i = 0; i < alphabetSize; ++i) {
        if (counts[i] == 0) {
            scaled[i] = 0;
        } else {
            scaled[i] = std::max(1u, static_cast<uint32_t>(
                (static_cast<uint64_t>(counts[i]) * RANS_SCALE + total / 2) / total));
        }
        scaledSum += scaled[i];
    }

    // Adjust to exactly RANS_SCALE
    if (scaledSum != RANS_SCALE) {
        int maxIdx = 0;
        for (int i = 1; i < alphabetSize; ++i) {
            if (scaled[i] > scaled[maxIdx]) maxIdx = i;
        }
        int32_t delta = static_cast<int32_t>(RANS_SCALE) - static_cast<int32_t>(scaledSum);
        int32_t newVal = static_cast<int32_t>(scaled[maxIdx]) + delta;
        if (newVal >= 1) {
            scaled[maxIdx] = static_cast<uint32_t>(newVal);
        } else {
            while (scaledSum > RANS_SCALE) {
                for (int i = 0; i < alphabetSize && scaledSum > RANS_SCALE; ++i) {
                    if (scaled[i] > 1) { --scaled[i]; --scaledSum; }
                }
            }
            while (scaledSum < RANS_SCALE) {
                for (int i = 0; i < alphabetSize && scaledSum < RANS_SCALE; ++i) {
                    if (scaled[i] > 0) { ++scaled[i]; ++scaledSum; }
                }
            }
        }
    }

    // Build cumulative frequencies + reciprocals
    uint16_t cum = 0;
    for (int i = 0; i < alphabetSize; ++i) {
        table[i].freq = static_cast<uint16_t>(scaled[i]);
        table[i].cumFreq = cum;
        if (scaled[i] > 0) {
            uint64_t rcp = (static_cast<uint64_t>(1) << 32) / scaled[i];
            table[i].rcpFreq = (rcp > UINT32_MAX) ? UINT32_MAX
                                                    : static_cast<uint32_t>(rcp);
        } else {
            table[i].rcpFreq = 0;
        }
        table[i].rcpShift = 32;
        cum += table[i].freq;
    }
}

std::vector<RANSSymbol> buildFrequencyTable(const uint32_t* counts, int alphabetSize) {
    std::vector<RANSSymbol> table(alphabetSize);
    buildFreqTableCore(counts, alphabetSize, table.data());
    return table;
}

void buildFrequencyTableInPlace(const uint32_t* counts, int alphabetSize,
                                RANSSymbol* table) {
    buildFreqTableCore(counts, alphabetSize, table);
}

std::vector<RANSDecodeEntry> buildDecodeTable(const std::vector<RANSSymbol>& symbols,
                                               int alphabetSize) {
    std::vector<RANSDecodeEntry> table(RANS_SCALE);

    for (int sym = 0; sym < alphabetSize; ++sym) {
        if (symbols[sym].freq == 0) continue;
        for (uint16_t j = 0; j < symbols[sym].freq; ++j) {
            uint16_t idx = symbols[sym].cumFreq + j;
            table[idx].symbol = static_cast<uint16_t>(sym);
            table[idx].freq = symbols[sym].freq;
            table[idx].cumFreq = symbols[sym].cumFreq;
        }
    }

    return table;
}

// ---- RANSEncoder ----
// Uses 8-bit I/O with stack-based byte output (Giesen's rans_byte approach).
// State range: [RANS_L, RANS_L << 8) = [1<<23, 1<<31).

RANSEncoder::RANSEncoder() = default;

void RANSEncoder::reset() {}

// Division-free symbol encoding using precomputed reciprocal.
// Uses: q = (uint64_t)state * rcpFreq >> 32  (approximate quotient)
// Then: r = state - q * freq  (exact remainder via back-multiply)
// This replaces the expensive state/freq and state%freq divisions.
inline void RANSEncoder::putSymbolFast(uint32_t& state, const RANSSymbol& sym,
                                        uint8_t*& stackPtr) {
    // Renormalize: push 8 bits while state >= x_max
    uint32_t x_max = ((RANS_L >> RANS_SCALE_BITS) << 8) * sym.freq;
    while (state >= x_max) {
        *stackPtr++ = static_cast<uint8_t>(state & 0xFF);
        state >>= 8;
    }

    // Division-free encode using reciprocal multiplication
    uint32_t q = static_cast<uint32_t>(
        (static_cast<uint64_t>(state) * sym.rcpFreq) >> 32);
    uint32_t r = state - q * sym.freq;
    // Fix up: reciprocal is a floor, so q might be too small by 1
    if (r >= sym.freq) {
        ++q;
        r -= sym.freq;
    }
    state = (q << RANS_SCALE_BITS) + r + sym.cumFreq;
}

void RANSEncoder::encode(const uint8_t* symbols, size_t count,
                          const RANSSymbol* freqTable, int /*alphabetSize*/,
                          std::vector<uint8_t>& output) {
    if (count == 0) return;

    // Pre-allocate stack buffer
    if (stack_.size() < count * 2) stack_.resize(count * 2);
    uint8_t* stackPtr = stack_.data();

    uint32_t state = RANS_L;

    for (size_t i = count; i > 0; --i) {
        uint8_t sym = symbols[i - 1];
        const RANSSymbol& s = freqTable[sym];
        if (s.freq == 0) continue;
        putSymbolFast(state, s, stackPtr);
    }

    size_t stackSize = static_cast<size_t>(stackPtr - stack_.data());

    // Write final state (4 bytes, big-endian) followed by reversed stack
    size_t stateOffset = output.size();
    output.resize(stateOffset + 4 + stackSize);
    output[stateOffset + 0] = static_cast<uint8_t>((state >> 24) & 0xFF);
    output[stateOffset + 1] = static_cast<uint8_t>((state >> 16) & 0xFF);
    output[stateOffset + 2] = static_cast<uint8_t>((state >> 8) & 0xFF);
    output[stateOffset + 3] = static_cast<uint8_t>(state & 0xFF);

    uint8_t* dst = output.data() + stateOffset + 4;
    for (size_t i = stackSize; i > 0; --i) {
        *dst++ = stack_[i - 1];
    }
}

void RANSEncoder::encodeInterleaved(const uint8_t* symbols, size_t count,
                                     const RANSSymbol* freqTable, int /*alphabetSize*/,
                                     std::vector<uint8_t>& output) {
    if (count == 0) return;

    // Pre-allocate stack buffer (reuses across calls)
    if (stack_.size() < count * 2) stack_.resize(count * 2);
    uint8_t* stackPtr = stack_.data();

    uint32_t state[4] = {RANS_L, RANS_L, RANS_L, RANS_L};

    // Encode in reverse order (rANS is LIFO)
    for (size_t i = count; i > 0; --i) {
        size_t idx = i - 1;
        uint8_t sym = symbols[idx];
        const RANSSymbol& s = freqTable[sym];
        if (s.freq == 0) continue;
        putSymbolFast(state[idx & 3], s, stackPtr);
    }

    size_t stackSize = static_cast<size_t>(stackPtr - stack_.data());

    // Write 4 states (16 bytes, big-endian) followed by reversed stack
    size_t offset = output.size();
    output.resize(offset + 16 + stackSize);
    for (int i = 0; i < 4; ++i) {
        output[offset + i * 4 + 0] = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
        output[offset + i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
        output[offset + i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
        output[offset + i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
    }

    uint8_t* dst = output.data() + offset + 16;
    for (size_t i = stackSize; i > 0; --i) {
        *dst++ = stack_[i - 1];
    }
}

// ---- RANSDecoder ----

RANSDecoder::RANSDecoder() = default;

void RANSDecoder::reset() {}

uint32_t RANSDecoder::getSymbol(uint32_t state, const RANSDecodeEntry* table,
                                 RANSDecodeEntry& entry) {
    uint32_t slot = state & (RANS_SCALE - 1);
    entry = table[slot];
    return state;
}

uint32_t RANSDecoder::advance(uint32_t state, const RANSDecodeEntry& entry,
                               const uint8_t*& ptr) {
    state = static_cast<uint32_t>(entry.freq) * (state >> RANS_SCALE_BITS) +
            (state & (RANS_SCALE - 1)) - entry.cumFreq;

    while (state < RANS_L) {
        state = (state << 8) | static_cast<uint32_t>(*ptr++);
    }

    return state;
}

bool RANSDecoder::decode(const uint8_t* data, size_t dataSize,
                          const RANSDecodeEntry* decodeTable,
                          size_t count, uint8_t* output) {
    if (dataSize < 4 || count == 0) return false;

    const uint8_t* ptr = data;
    uint32_t state = (static_cast<uint32_t>(ptr[0]) << 24) |
                     (static_cast<uint32_t>(ptr[1]) << 16) |
                     (static_cast<uint32_t>(ptr[2]) << 8) |
                      static_cast<uint32_t>(ptr[3]);
    ptr += 4;

    for (size_t i = 0; i < count; ++i) {
        RANSDecodeEntry entry;
        getSymbol(state, decodeTable, entry);
        output[i] = static_cast<uint8_t>(entry.symbol);
        state = advance(state, entry, ptr);
    }

    return true;
}

bool RANSDecoder::decodeInterleaved(const uint8_t* data, size_t dataSize,
                                     const RANSDecodeEntry* decodeTable,
                                     size_t count, uint8_t* output) {
    if (dataSize < 16 || count == 0) return false;

    const uint8_t* ptr = data;
    uint32_t state[4];
    for (int i = 0; i < 4; ++i) {
        state[i] = (static_cast<uint32_t>(ptr[0]) << 24) |
                   (static_cast<uint32_t>(ptr[1]) << 16) |
                   (static_cast<uint32_t>(ptr[2]) << 8) |
                    static_cast<uint32_t>(ptr[3]);
        ptr += 4;
    }

    size_t i = 0;
    size_t aligned = count & ~static_cast<size_t>(3);
    for (; i < aligned; i += 4) {
        RANSDecodeEntry e0, e1, e2, e3;
        getSymbol(state[0], decodeTable, e0);
        getSymbol(state[1], decodeTable, e1);
        getSymbol(state[2], decodeTable, e2);
        getSymbol(state[3], decodeTable, e3);

        output[i + 0] = static_cast<uint8_t>(e0.symbol);
        output[i + 1] = static_cast<uint8_t>(e1.symbol);
        output[i + 2] = static_cast<uint8_t>(e2.symbol);
        output[i + 3] = static_cast<uint8_t>(e3.symbol);

        state[0] = advance(state[0], e0, ptr);
        state[1] = advance(state[1], e1, ptr);
        state[2] = advance(state[2], e2, ptr);
        state[3] = advance(state[3], e3, ptr);
    }

    for (; i < count; ++i) {
        RANSDecodeEntry entry;
        getSymbol(state[i & 3], decodeTable, entry);
        output[i] = static_cast<uint8_t>(entry.symbol);
        state[i & 3] = advance(state[i & 3], entry, ptr);
    }

    return true;
}

} // namespace omni
} // namespace omnidesk
