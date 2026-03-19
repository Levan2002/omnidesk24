#pragma once

#include "analyzer/analyzer_engine.h"
#include <ostream>
#include <string>
#include <vector>

namespace omnidesk {
namespace analyzer {

enum class ReportFormat {
    Console,
    JSON,
};

class ReportGenerator {
public:
    // Print a single result to the given stream.
    static void printSingle(std::ostream& out, const AnalyzerResult& result,
                            ReportFormat format = ReportFormat::Console);

    // Print a comparison table of multiple results.
    static void printComparison(std::ostream& out,
                                const std::vector<AnalyzerResult>& results,
                                ReportFormat format = ReportFormat::Console);

private:
    static void printConsole(std::ostream& out, const AnalyzerResult& result);
    static void printJSON(std::ostream& out, const AnalyzerResult& result);
    static void printComparisonConsole(std::ostream& out,
                                       const std::vector<AnalyzerResult>& results);
    static void printComparisonJSON(std::ostream& out,
                                     const std::vector<AnalyzerResult>& results);

    static void printLatencyStats(std::ostream& out, const char* name,
                                   const LatencyStats& stats);
    static std::string escapeJSON(const std::string& s);
};

} // namespace analyzer
} // namespace omnidesk
