#include "core/simd_dct.h"

// AVX2 DCT implementation — placeholder.
// Currently falls back to scalar implementation.
// Will be optimized in Phase 5 with SIMD butterflies.

namespace omnidesk {
namespace avx2 {

void dctForward(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    // Delegate to scalar for now
    omnidesk::dctForward(src, srcStride, dst, dstStride, blockSize);
}

void dctInverse(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    omnidesk::dctInverse(src, srcStride, dst, dstStride, blockSize);
}

} // namespace avx2
} // namespace omnidesk
