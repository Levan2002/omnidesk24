#include "codec/omni/rans_codec.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace omnidesk {
namespace omni {

// ---- Frequency Table Construction ----

std::vector<RANSSymbol> buildFrequencyTable(const uint32_t* counts, int alphabetSize) {
    std::vector<RANSSymbol> table(alphabetSize);

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
            cum += f;
        }
        return table;
    }

    // Normalize frequencies to sum to RANS_SCALE.
    std::vector<uint32_t> scaled(alphabetSize);
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

    // Adjust to exactly RANS_SCALE — O(alphabetSize) single-pass approach:
    // Find the symbol with max frequency and adjust it by the full delta.
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
            // Fallback: distribute across multiple symbols
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

    // Build cumulative frequencies
    uint16_t cum = 0;
    for (int i = 0; i < alphabetSize; ++i) {
        table[i].freq = static_cast<uint16_t>(scaled[i]);
        table[i].cumFreq = cum;
        cum += table[i].freq;
    }

    return table;
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

void RANSEncoder::putSymbol(uint32_t& state, const RANSSymbol& sym,
                             std::vector<uint8_t>& output) {
    // Renormalize: push 8 bits while state >= x_max
    // x_max = ((RANS_L >> SCALE_BITS) << 8) * freq
    uint32_t x_max = ((RANS_L >> RANS_SCALE_BITS) << 8) * sym.freq;
    while (state >= x_max) {
        output.push_back(static_cast<uint8_t>(state & 0xFF));
        state >>= 8;
    }

    // Encode: state = (state / freq) * M + (state % freq) + cumFreq
    state = ((state / sym.freq) << RANS_SCALE_BITS) +
            (state % sym.freq) + sym.cumFreq;
}

void RANSEncoder::flush(uint32_t /*state*/, std::vector<uint8_t>& /*output*/) {}

void RANSEncoder::encode(const uint8_t* symbols, size_t count,
                          const RANSSymbol* freqTable, int /*alphabetSize*/,
                          std::vector<uint8_t>& output) {
    if (count == 0) return;

    // rANS encodes in reverse order (LIFO).
    // Renorm bytes are pushed to a stack; we reverse them for the decoder.
    uint32_t state = RANS_L;

    std::vector<uint8_t> stack;
    stack.reserve(count);

    for (size_t i = count; i > 0; --i) {
        uint8_t sym = symbols[i - 1];
        const RANSSymbol& s = freqTable[sym];
        if (s.freq == 0) continue;
        putSymbol(state, s, stack);
    }

    // Write final state (4 bytes, big-endian) followed by renorm bytes.
    // Reserve space and write state + reversed stack in one pass.
    size_t stateOffset = output.size();
    output.resize(stateOffset + 4 + stack.size());
    output[stateOffset + 0] = static_cast<uint8_t>((state >> 24) & 0xFF);
    output[stateOffset + 1] = static_cast<uint8_t>((state >> 16) & 0xFF);
    output[stateOffset + 2] = static_cast<uint8_t>((state >> 8) & 0xFF);
    output[stateOffset + 3] = static_cast<uint8_t>(state & 0xFF);

    // Copy stack in reverse directly into output (avoids extra allocation)
    uint8_t* dst = output.data() + stateOffset + 4;
    for (size_t i = stack.size(); i > 0; --i) {
        *dst++ = stack[i - 1];
    }
}

void RANSEncoder::encodeInterleaved(const uint8_t* symbols, size_t count,
                                     const RANSSymbol* freqTable, int /*alphabetSize*/,
                                     std::vector<uint8_t>& output) {
    if (count == 0) return;

    // 4 independent rANS states for interleaved encoding.
    // Symbols are assigned round-robin: symbol[i] uses state[i & 3].
    uint32_t state[4] = {RANS_L, RANS_L, RANS_L, RANS_L};

    std::vector<uint8_t> stack;
    stack.reserve(count);

    // Encode in reverse order (rANS is LIFO)
    for (size_t i = count; i > 0; --i) {
        size_t idx = i - 1;
        uint8_t sym = symbols[idx];
        const RANSSymbol& s = freqTable[sym];
        if (s.freq == 0) continue;
        putSymbol(state[idx & 3], s, stack);
    }

    // Write 4 states (16 bytes, big-endian) followed by reversed stack
    size_t offset = output.size();
    output.resize(offset + 16 + stack.size());
    for (int i = 0; i < 4; ++i) {
        output[offset + i * 4 + 0] = static_cast<uint8_t>((state[i] >> 24) & 0xFF);
        output[offset + i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xFF);
        output[offset + i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xFF);
        output[offset + i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFF);
    }

    uint8_t* dst = output.data() + offset + 16;
    for (size_t i = stack.size(); i > 0; --i) {
        *dst++ = stack[i - 1];
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
    // Decode step: state = freq * (state >> SCALE_BITS) + (state & (SCALE-1)) - cumFreq
    state = static_cast<uint32_t>(entry.freq) * (state >> RANS_SCALE_BITS) +
            (state & (RANS_SCALE - 1)) - entry.cumFreq;

    // Renormalize: read 8 bits while state < RANS_L
    while (state < RANS_L) {
        state = (state << 8) | static_cast<uint32_t>(*ptr++);
    }

    return state;
}

bool RANSDecoder::decode(const uint8_t* data, size_t dataSize,
                          const RANSDecodeEntry* decodeTable,
                          size_t count, uint8_t* output) {
    if (dataSize < 4 || count == 0) return false;

    // Read initial state from the start of the stream (big-endian)
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

    // Read 4 initial states (big-endian)
    const uint8_t* ptr = data;
    uint32_t state[4];
    for (int i = 0; i < 4; ++i) {
        state[i] = (static_cast<uint32_t>(ptr[0]) << 24) |
                   (static_cast<uint32_t>(ptr[1]) << 16) |
                   (static_cast<uint32_t>(ptr[2]) << 8) |
                    static_cast<uint32_t>(ptr[3]);
        ptr += 4;
    }

    // Decode in groups of 4 for maximum pipeline utilization.
    // Table lookups for all 4 states are independent → CPU can pipeline them.
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

    // Handle remainder (0-3 symbols)
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
