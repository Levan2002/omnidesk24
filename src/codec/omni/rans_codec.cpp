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

    // Adjust to exactly RANS_SCALE
    while (scaledSum != RANS_SCALE) {
        if (scaledSum > RANS_SCALE) {
            int maxIdx = 0;
            for (int i = 1; i < alphabetSize; ++i) {
                if (scaled[i] > scaled[maxIdx]) maxIdx = i;
            }
            if (scaled[maxIdx] > 1) {
                --scaled[maxIdx];
                --scaledSum;
            } else {
                break;
            }
        } else {
            int maxIdx = 0;
            for (int i = 1; i < alphabetSize; ++i) {
                if (scaled[i] > scaled[maxIdx]) maxIdx = i;
            }
            ++scaled[maxIdx];
            ++scaledSum;
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

    // Write final state (4 bytes, big-endian) followed by renorm bytes (reversed)
    output.push_back(static_cast<uint8_t>((state >> 24) & 0xFF));
    output.push_back(static_cast<uint8_t>((state >> 16) & 0xFF));
    output.push_back(static_cast<uint8_t>((state >> 8) & 0xFF));
    output.push_back(static_cast<uint8_t>(state & 0xFF));

    // Stack bytes were emitted in reverse decode order; reverse them.
    output.insert(output.end(), stack.rbegin(), stack.rend());
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

} // namespace omni
} // namespace omnidesk
