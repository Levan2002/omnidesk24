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

// Build a decode lookup table (RANS_SCALE entries) for O(1) decoding.
std::vector<RANSDecodeEntry> buildDecodeTable(const std::vector<RANSSymbol>& symbols,
                                               int alphabetSize);

class RANSEncoder {
public:
    RANSEncoder();

    // Encode a sequence of byte symbols.
    // symbols: input byte array
    // count: number of symbols
    // freqTable: normalized frequency table (alphabetSize entries)
    // alphabetSize: number of symbols (typically 256)
    // output: encoded bytes appended here
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
    void putSymbol(uint32_t& state, const RANSSymbol& sym,
                   std::vector<uint8_t>& output);
    void flush(uint32_t state, std::vector<uint8_t>& output);
};

class RANSDecoder {
public:
    RANSDecoder();

    // Decode a sequence of byte symbols.
    // data: encoded byte stream
    // dataSize: encoded stream length
    // decodeTable: precomputed decode table (RANS_SCALE entries)
    // count: number of symbols to decode
    // output: decoded symbols written here (must have space for count bytes)
    // Returns: true on success, false on error.
    bool decode(const uint8_t* data, size_t dataSize,
                const RANSDecodeEntry* decodeTable,
                size_t count, uint8_t* output);

    // 4-way interleaved decode (matches encodeInterleaved).
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
