#include "core/simd_dct.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace omnidesk {

// Fixed-point scale factor for integer DCT.
// We use 12-bit fixed point: multiply by 4096, then shift right by 12 after.
constexpr int FP_SHIFT = 12;
constexpr int FP_ROUND = 1 << (FP_SHIFT - 1);

// Precomputed orthogonal DCT-II table.
// table[k][n] = round(alpha[k] * cos(pi*(2*n+1)*k / (2*N)) * 4096)
// where alpha[0] = sqrt(1/N), alpha[k>0] = sqrt(2/N)
// This makes A orthogonal: A * A^T = I (in continuous math).
// Forward: Y[k] = sum_n x[n] * A[k][n]  (then >> 12 for fixed-point)
// Inverse: x[n] = sum_k Y[k] * A[k][n]  (then >> 12 for fixed-point)

static int32_t cosTable4[4][4];
static int32_t cosTable8[8][8];
static int32_t cosTable16[16][16];
static bool tablesInitialized = false;

static void initCosTables() {
    if (tablesInitialized) return;

    auto fillTable = [](int32_t* table, int N) {
        for (int k = 0; k < N; ++k) {
            double alpha = (k == 0) ? std::sqrt(1.0 / N) : std::sqrt(2.0 / N);
            for (int n = 0; n < N; ++n) {
                double val = alpha * std::cos(M_PI * (2.0 * n + 1.0) * k / (2.0 * N));
                table[k * N + n] = static_cast<int32_t>(std::round(val * (1 << FP_SHIFT)));
            }
        }
    };

    fillTable(&cosTable4[0][0], 4);
    fillTable(&cosTable8[0][0], 8);
    fillTable(&cosTable16[0][0], 16);
    tablesInitialized = true;
}

// 1D forward DCT-II (orthogonal): Y[k] = (sum_n x[n] * A[k][n] + round) >> shift
static void dct1D(const int16_t* src, int srcStep, int16_t* dst, int dstStep,
                   int N, const int32_t* cosTable) {
    for (int k = 0; k < N; ++k) {
        int64_t sum = 0;
        for (int n = 0; n < N; ++n) {
            sum += static_cast<int64_t>(src[n * srcStep]) * cosTable[k * N + n];
        }
        sum = (sum + FP_ROUND) >> FP_SHIFT;
        dst[k * dstStep] = static_cast<int16_t>(std::max((int64_t)-32768, std::min((int64_t)32767, sum)));
    }
}

// 1D inverse DCT-II (orthogonal): x[n] = (sum_k Y[k] * A[k][n] + round) >> shift
static void idct1D(const int16_t* src, int srcStep, int16_t* dst, int dstStep,
                    int N, const int32_t* cosTable) {
    for (int n = 0; n < N; ++n) {
        int64_t sum = 0;
        for (int k = 0; k < N; ++k) {
            sum += static_cast<int64_t>(src[k * srcStep]) * cosTable[k * N + n];
        }
        sum = (sum + FP_ROUND) >> FP_SHIFT;
        dst[n * dstStep] = static_cast<int16_t>(std::max((int64_t)-32768, std::min((int64_t)32767, sum)));
    }
}

// 2D DCT via separable 1D transforms: first rows, then columns.
void dctForward(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    initCosTables();

    const int32_t* cosTable;
    switch (blockSize) {
        case 4:  cosTable = &cosTable4[0][0]; break;
        case 8:  cosTable = &cosTable8[0][0]; break;
        case 16: cosTable = &cosTable16[0][0]; break;
        default: return;
    }

    // Temp buffer for intermediate results
    int16_t temp[16 * 16];

    // Transform rows: src -> temp
    for (int y = 0; y < blockSize; ++y) {
        dct1D(src + y * srcStride, 1, temp + y * blockSize, 1, blockSize, cosTable);
    }

    // Transform columns: temp -> dst
    for (int x = 0; x < blockSize; ++x) {
        dct1D(temp + x, blockSize, dst + x, dstStride, blockSize, cosTable);
    }
}

void dctInverse(const int16_t* src, int srcStride,
                int16_t* dst, int dstStride, int blockSize) {
    initCosTables();

    const int32_t* cosTable;
    switch (blockSize) {
        case 4:  cosTable = &cosTable4[0][0]; break;
        case 8:  cosTable = &cosTable8[0][0]; break;
        case 16: cosTable = &cosTable16[0][0]; break;
        default: return;
    }

    int16_t temp[16 * 16];

    // Inverse transform columns: src -> temp
    for (int x = 0; x < blockSize; ++x) {
        idct1D(src + x, srcStride, temp + x, blockSize, blockSize, cosTable);
    }

    // Inverse transform rows: temp -> dst
    for (int y = 0; y < blockSize; ++y) {
        idct1D(temp + y * blockSize, 1, dst + y * dstStride, 1, blockSize, cosTable);
    }
}

// Quantization: coeffs[k] = coeffs[k] / (qStep * weight[k])
// where qStep depends on QP and weight increases for higher frequency coefficients.
void quantize(int16_t* coeffs, int stride, int blockSize, int qp) {
    // QP to step size (H.264-like): step = 2^(qp/6)
    // Simplified: step = max(1, qp / 4)
    int step = std::max(1, qp / 4);

    for (int y = 0; y < blockSize; ++y) {
        for (int x = 0; x < blockSize; ++x) {
            // Higher-frequency coefficients get coarser quantization
            int weight = 1 + (x + y) / 2;
            int qStep = step * weight;
            int val = coeffs[y * stride + x];
            // Rounding-toward-zero quantization
            if (val >= 0) {
                coeffs[y * stride + x] = static_cast<int16_t>(val / qStep);
            } else {
                coeffs[y * stride + x] = static_cast<int16_t>(-((-val) / qStep));
            }
        }
    }
}

void dequantize(int16_t* coeffs, int stride, int blockSize, int qp) {
    int step = std::max(1, qp / 4);

    for (int y = 0; y < blockSize; ++y) {
        for (int x = 0; x < blockSize; ++x) {
            int weight = 1 + (x + y) / 2;
            int qStep = step * weight;
            coeffs[y * stride + x] = static_cast<int16_t>(coeffs[y * stride + x] * qStep);
        }
    }
}

} // namespace omnidesk
