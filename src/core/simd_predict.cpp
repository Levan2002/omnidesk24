#include "core/simd_predict.h"
#include "core/simd_utils.h"  // cpuSupportsAVX2()

#ifdef OMNIDESK_X86_64
#include <immintrin.h>
#endif

#include <cmath>
#include <cstdlib>

namespace omnidesk {

// ---- Scalar implementations ----

static void predictDC_scalar(const int16_t* top, const int16_t* left,
                              int width, int height, int16_t* out) {
    int32_t sum = 0;
    int count = 0;

    if (top) {
        for (int x = 0; x < width; ++x) {
            sum += top[x];
        }
        count += width;
    }
    if (left) {
        for (int y = 0; y < height; ++y) {
            sum += left[y];
        }
        count += height;
    }

    int16_t dc = (count > 0) ? static_cast<int16_t>(sum / count) : 0;

    for (int i = 0; i < width * height; ++i) {
        out[i] = dc;
    }
}

static void predictH_scalar(const int16_t* left, int width, int height, int16_t* out) {
    for (int y = 0; y < height; ++y) {
        int16_t val = left ? left[y] : 0;
        for (int x = 0; x < width; ++x) {
            out[y * width + x] = val;
        }
    }
}

static void predictV_scalar(const int16_t* top, int width, int height, int16_t* out) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            out[y * width + x] = top ? top[x] : 0;
        }
    }
}

#ifdef OMNIDESK_X86_64
// ---- SSE2 implementations ----

static void predictDC_sse2(const int16_t* top, const int16_t* left,
                             int width, int height, int16_t* out) {
    // Use scalar for DC computation (border is small), SIMD for fill
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
    __m128i dcVec = _mm_set1_epi16(dc);

    int total = width * height;
    int i = 0;
    for (; i + 7 < total; i += 8) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), dcVec);
    }
    for (; i < total; ++i) {
        out[i] = dc;
    }
}

static void predictV_sse2(const int16_t* top, int width, int height, int16_t* out) {
    if (!top) {
        __m128i z = _mm_setzero_si128();
        int total = width * height;
        int i = 0;
        for (; i + 7 < total; i += 8) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), z);
        }
        for (; i < total; ++i) out[i] = 0;
        return;
    }

    for (int y = 0; y < height; ++y) {
        int x = 0;
        for (; x + 7 < width; x += 8) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(top + x));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(out + y * width + x), v);
        }
        for (; x < width; ++x) {
            out[y * width + x] = top[x];
        }
    }
}

static uint64_t computeSAD_sse2(const int16_t* a, const int16_t* b, int count) {
    __m128i sumLo = _mm_setzero_si128();
    __m128i sumHi = _mm_setzero_si128();

    int i = 0;
    for (; i + 7 < count; i += 8) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
        __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));
        __m128i diff = _mm_sub_epi16(va, vb);

        // Absolute value of 16-bit: abs = max(x, -x) but use (x ^ (x>>15)) - (x>>15)
        __m128i sign = _mm_srai_epi16(diff, 15);
        __m128i absDiff = _mm_sub_epi16(_mm_xor_si128(diff, sign), sign);

        // Accumulate into 32-bit
        __m128i lo = _mm_unpacklo_epi16(absDiff, _mm_setzero_si128());
        __m128i hi = _mm_unpackhi_epi16(absDiff, _mm_setzero_si128());
        sumLo = _mm_add_epi32(sumLo, lo);
        sumHi = _mm_add_epi32(sumHi, hi);
    }

    // Horizontal sum
    __m128i total = _mm_add_epi32(sumLo, sumHi);
    total = _mm_add_epi32(total, _mm_shuffle_epi32(total, _MM_SHUFFLE(2, 3, 0, 1)));
    total = _mm_add_epi32(total, _mm_shuffle_epi32(total, _MM_SHUFFLE(1, 0, 3, 2)));
    uint64_t result = static_cast<uint32_t>(_mm_cvtsi128_si32(total));

    // Scalar remainder
    for (; i < count; ++i) {
        result += std::abs(a[i] - b[i]);
    }

    return result;
}
#endif

// ---- Public API (dispatch) ----

void predictDC(const int16_t* top, const int16_t* left,
               int width, int height, int16_t* out) {
#ifdef OMNIDESK_X86_64
    predictDC_sse2(top, left, width, height, out);
#else
    predictDC_scalar(top, left, width, height, out);
#endif
}

void predictH(const int16_t* left, int width, int height, int16_t* out) {
    // H prediction is inherently sequential per row; SSE2 helps with fill
    predictH_scalar(left, width, height, out);
}

void predictV(const int16_t* top, int width, int height, int16_t* out) {
#ifdef OMNIDESK_X86_64
    predictV_sse2(top, width, height, out);
#else
    predictV_scalar(top, width, height, out);
#endif
}

void predictPlanar(const int16_t* top, const int16_t* left, int16_t topLeft,
                   int width, int height, int16_t* out) {
    // Planar: bilinear prediction
    // P(x,y) = (top[x] * (height - y) + left[y] * (width - x)
    //           + topLeft * (x + y - width - height) + (width + height)) / (width + height)
    // Simplified: weighted blend from top and left borders
    int wh = width + height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int topVal = top ? top[x] : 0;
            int leftVal = left ? left[y] : 0;
            int pred = ((height - 1 - y) * topVal + (width - 1 - x) * leftVal +
                        (y + x) * topLeft + wh / 2) / std::max(wh - 2, 1);
            out[y * width + x] = static_cast<int16_t>(pred);
        }
    }
}

void predictLeftPixel(const int16_t* src, int16_t* residual, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            int16_t predicted;
            if (x > 0) {
                predicted = src[idx - 1];
            } else if (y > 0) {
                predicted = src[idx - width];
            } else {
                predicted = 0;
            }
            residual[idx] = src[idx] - predicted;
        }
    }
}

void inversePredictLeftPixel(const int16_t* residual, int16_t* out, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            int16_t predicted;
            if (x > 0) {
                predicted = out[idx - 1];
            } else if (y > 0) {
                predicted = out[idx - width];
            } else {
                predicted = 0;
            }
            out[idx] = residual[idx] + predicted;
        }
    }
}

uint64_t computeSAD(const int16_t* a, const int16_t* b, int count) {
#ifdef OMNIDESK_X86_64
    return computeSAD_sse2(a, b, count);
#else
    uint64_t sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += std::abs(a[i] - b[i]);
    }
    return sum;
#endif
}

} // namespace omnidesk
