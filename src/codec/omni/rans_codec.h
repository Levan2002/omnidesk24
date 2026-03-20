#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace omnidesk {
namespace omni {

// rANS (range Asymmetric Numeral Systems) entropy coder.
//
// Near-optimal compression at high speed. Faster than CABAC, better
// compression ratio than Huffman for skewed distributions.
//
// Scale bits = 12 => frequency denominator M = 4096.

constexpr int RANS_SCALE_BITS = 12;
constexpr uint32_t RANS_SCALE = 1 << RANS_SCALE_BITS;  // 4096
constexpr uint32_t RANS_L = 1 << 23;  // lower bound of rANS state

// Symbol info for encoding (precomputed frequency table)
struct RANSSymbol {
    uint16_t freq;      // normalized frequency in [1, RANS_SCALE]
    uint16_t cumFreq;   // cumulative frequency (exclusive prefix sum)
    uint32_t rcpFreq;   // reciprocal: ceil(2^32 / freq) for division-free encode
    uint8_t  rcpShift;  // always 32 for our approach
};

// Decode table entry (precomputed for O(1) symbol lookup)
struct RANSDecodeEntry {
    uint16_t symbol;
    uint16_t freq;
    uint16_t cumFreq;
};

// Build a normalized frequency table from raw symbol counts.
// alphabetSize: number of distinct symbols (e.g., 256 for bytes).
// counts: raw frequency of each symbol.
// Returns: normalized RANSSymbol table where frequencies sum to RANS_SCALE.
std::vector<RANSSymbol> buildFrequencyTable(const uint32_t* counts, int alphabetSize);

// Build into pre-allocated table (avoids heap allocation in hot path).
void buildFrequencyTableInPlace(const uint32_t* counts, int alphabetSize,
                                RANSSymbol* table);

// Build a decode lookup table (RANS_SCALE entries) for O(1) decoding.
std::vector<RANSDecodeEntry> buildDecodeTable(const std::vector<RANSSymbol>& symbols,
                                               int alphabetSize);

class RANSEncoder {
public:
    RANSEncoder();

    // Encode a sequence of byte symbols.
    void encode(const uint8_t* symbols, size_t count,
                const RANSSymbol* freqTable, int alphabetSize,
                std::vector<uint8_t>& output);

    // 4-way interleaved encode for higher throughput.
    // Uses 4 independent rANS states to hide decode latency.
    void encodeInterleaved(const uint8_t* symbols, size_t count,
                           const RANSSymbol* freqTable, int alphabetSize,
                           std::vector<uint8_t>& output);

    void reset();

private:
    // Division-free putSymbol using precomputed reciprocal
    inline void putSymbolFast(uint32_t& state, const RANSSymbol& sym,
                              uint8_t*& stackPtr);

    // Pre-allocated stack buffer for encode (avoids per-call heap allocation)
    std::vector<uint8_t> stack_;
};

class RANSDecoder {
public:
    RANSDecoder();

    bool decode(const uint8_t* data, size_t dataSize,
                const RANSDecodeEntry* decodeTable,
                size_t count, uint8_t* output);

    bool decodeInterleaved(const uint8_t* data, size_t dataSize,
                           const RANSDecodeEntry* decodeTable,
                           size_t count, uint8_t* output);

    void reset();

private:
    uint32_t getSymbol(uint32_t state, const RANSDecodeEntry* table,
                       RANSDecodeEntry& entry);
    uint32_t advance(uint32_t state, const RANSDecodeEntry& entry,
                     const uint8_t*& ptr);
};

} // namespace omni
} // namespace omnidesk
