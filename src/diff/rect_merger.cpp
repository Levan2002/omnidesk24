#include "diff/rect_merger.h"
#include <algorithm>
#include <limits>

namespace omnidesk {

std::vector<Rect> RectMerger::merge(const std::vector<Rect>& rects, int maxRects) {
    if (rects.empty()) {
        return {};
    }
    if (maxRects <= 0) {
        maxRects = 1;
    }

    // First pass: merge any truly overlapping / contained rects.
    std::vector<Rect> working = mergeOverlapping(rects);

    // Greedy reduction: while we have too many rects, merge the pair whose
    // union introduces the least additional (wasted) area.
    while (static_cast<int>(working.size()) > maxRects) {
        auto [i, j] = findBestMergePair(working);
        working[i] = working[i].united(working[j]);
        working.erase(working.begin() + j);
    }

    return working;
}

int64_t RectMerger::totalArea(const std::vector<Rect>& rects) {
    int64_t sum = 0;
    for (const auto& r : rects) {
        sum += static_cast<int64_t>(r.area());
    }
    return sum;
}

std::pair<int, int> RectMerger::findBestMergePair(const std::vector<Rect>& rects) {
    int bestI = 0;
    int bestJ = 1;
    int64_t bestCost = std::numeric_limits<int64_t>::max();

    for (size_t i = 0; i < rects.size(); ++i) {
        for (size_t j = i + 1; j < rects.size(); ++j) {
            // Cost of merging = area of union - (area_i + area_j)
            // i.e. the wasted pixels introduced by the merge.
            Rect merged = rects[i].united(rects[j]);
            int64_t waste = static_cast<int64_t>(merged.area()) -
                            static_cast<int64_t>(rects[i].area()) -
                            static_cast<int64_t>(rects[j].area());
            if (waste < bestCost) {
                bestCost = waste;
                bestI = static_cast<int>(i);
                bestJ = static_cast<int>(j);
            }
        }
    }

    return {bestI, bestJ};
}

std::vector<Rect> RectMerger::mergeOverlapping(const std::vector<Rect>& rects) {
    std::vector<Rect> result;
    result.reserve(rects.size());

    // Copy non-empty rects.
    for (const auto& r : rects) {
        if (!r.empty()) {
            result.push_back(r);
        }
    }

    // Iterative merging of overlapping pairs until stable.
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < result.size(); ++i) {
            for (size_t j = i + 1; j < result.size(); ++j) {
                if (result[i].intersects(result[j])) {
                    result[i] = result[i].united(result[j]);
                    result.erase(result.begin() + static_cast<ptrdiff_t>(j));
                    merged = true;
                    break;
                }
            }
            if (merged) break;
        }
    }

    return result;
}

} // namespace omnidesk
