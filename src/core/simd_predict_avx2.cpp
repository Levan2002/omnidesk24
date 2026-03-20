#include "core/simd_predict.h"

#ifdef OMNIDESK_X86_64
#include <immintrin.h>
#endif

namespace omnidesk {
namespace avx2 {

#ifdef OMNIDESK_X86_64

void predictDC(const int16_t* top, const int16_t* left,
               int width, int height, int16_t* out) {
    // Compute DC value (scalar — border is small)
    int32_t sum = 0;
    int count = 0;
    if (top) {
        for (int x = 0; x < width; ++x) sum += top[x];
        count += width;
    }
    if (left) {
        for (int y = 0; y < height; ++y) sum += left[y];
        count += height;
    }

    int16_t dc = (count > 0) ? static_cast<int16_t>(sum / count) : 0;
    __m256i dcVec = _mm256_set1_epi16(dc);

    int total = width * height;
    int i = 0;
    for (; i + 15 < total; i += 16) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), dcVec);
    }
    for (; i < total; ++i) {
        out[i] = dc;
    }
}

void predictH(const int16_t* left, int width, int height, int16_t* out) {
    for (int y = 0; y < height; ++y) {
        int16_t val = left ? left[y] : 0;
        __m256i v = _mm256_set1_epi16(val);
        int x = 0;
        for (; x + 15 < width; x += 16) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + y * width + x), v);
        }
        for (; x < width; ++x) {
            out[y * width + x] = val;
        }
    }
}

void predictV(const int16_t* top, int width, int height, int16_t* out) {
    if (!top) {
        __m256i z = _mm256_setzero_si256();
        int total = width * height;
        int i = 0;
        for (; i + 15 < total; i += 16) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), z);
        }
        for (; i < total; ++i) out[i] = 0;
        return;
    }

    for (int y = 0; y < height; ++y) {
        int x = 0;
        for (; x + 15 < width; x += 16) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(top + x));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + y * width + x), v);
        }
        for (; x < width; ++x) {
            out[y * width + x] = top[x];
        }
    }
}

uint64_t computeSAD(const int16_t* a, const int16_t* b, int count) {
    __m256i sumLo = _mm256_setzero_si256();
    __m256i sumHi = _mm256_setzero_si256();

    int i = 0;
    for (; i + 15 < count; i += 16) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i diff = _mm256_sub_epi16(va, vb);
        __m256i absDiff = _mm256_abs_epi16(diff);

        // Accumulate to 32-bit
        __m256i lo = _mm256_unpacklo_epi16(absDiff, _mm256_setzero_si256());
        __m256i hi = _mm256_unpackhi_epi16(absDiff, _mm256_setzero_si256());
        sumLo = _mm256_add_epi32(sumLo, lo);
        sumHi = _mm256_add_epi32(sumHi, hi);
    }

    // Horizontal sum
    __m256i total = _mm256_add_epi32(sumLo, sumHi);
    __m128i lo128 = _mm256_castsi256_si128(total);
    __m128i hi128 = _mm256_extracti128_si256(total, 1);
    __m128i sum128 = _mm_add_epi32(lo128, hi128);
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(2, 3, 0, 1)));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(1, 0, 3, 2)));
    uint64_t result = static_cast<uint32_t>(_mm_cvtsi128_si32(sum128));

    // Scalar remainder
    for (; i < count; ++i) {
        result += std::abs(a[i] - b[i]);
    }

    return result;
}

#else
// No AVX2 support
void predictDC(const int16_t*, const int16_t*, int, int, int16_t*) {}
void predictH(const int16_t*, int, int, int16_t*) {}
void predictV(const int16_t*, int, int, int16_t*) {}
uint64_t computeSAD(const int16_t*, const int16_t*, int) { return 0; }
#endif

} // namespace avx2
} // namespace omnidesk
