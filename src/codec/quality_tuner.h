#pragma once

#include "core/types.h"
#include <cstdint>

namespace omnidesk {

// Per-region quality adjustment result.
struct QPAdjustment {
    int qp = 0;         // Adjusted QP value
    bool skip = false;   // If true, skip encoding this region entirely (STATIC)
};

// Adjusts encoder quantization parameter (QP) based on content type.
//
// Strategy:
//   TEXT:   Lower QP (higher quality) to preserve sharp edges and readability.
//   MOTION: Keep base QP (standard quality, motion hides artifacts).
//   STATIC: Set skip flag (no need to re-encode unchanged regions).
class QualityTuner {
public:
    QualityTuner() = default;
    ~QualityTuner() = default;

    // Adjust QP for a region given its content classification.
    // Returns the adjusted QP, clamped to valid H.264 range [0, 51].
    int adjustQP(int baseQP, ContentType type) const;

    // Extended version that also returns skip flag for STATIC regions.
    QPAdjustment adjust(int baseQP, ContentType type) const;

    // Tuning parameters
    void setTextQPDelta(int delta) { textQPDelta_ = delta; }
    void setMotionQPDelta(int delta) { motionQPDelta_ = delta; }

private:
    int textQPDelta_ = -15;   // Maximum quality for text (sharp edges, perfect readability)
    int motionQPDelta_ = 4;   // Higher QP for motion (perceptual masking hides artifacts, saves bandwidth)
};

} // namespace omnidesk
