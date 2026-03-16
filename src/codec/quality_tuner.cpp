#include "codec/quality_tuner.h"
#include <algorithm>

namespace omnidesk {

int QualityTuner::adjustQP(int baseQP, ContentType type) const {
    return adjust(baseQP, type).qp;
}

QPAdjustment QualityTuner::adjust(int baseQP, ContentType type) const {
    QPAdjustment result;

    switch (type) {
        case ContentType::TEXT:
            // Lower QP for text to preserve sharp edges and readability.
            result.qp = std::clamp(baseQP + textQPDelta_, 0, 51);
            result.skip = false;
            break;

        case ContentType::MOTION:
            // Keep base QP for motion content; perceptual masking hides artifacts.
            result.qp = std::clamp(baseQP + motionQPDelta_, 0, 51);
            result.skip = false;
            break;

        case ContentType::STATIC:
            // Static regions: set skip flag. QP is irrelevant but set to base.
            result.qp = std::clamp(baseQP, 0, 51);
            result.skip = true;
            break;

        case ContentType::UNKNOWN:
        default:
            // Unknown: use base QP, do not skip.
            result.qp = std::clamp(baseQP, 0, 51);
            result.skip = false;
            break;
    }

    return result;
}

} // namespace omnidesk
