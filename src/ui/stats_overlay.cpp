#include "ui/stats_overlay.h"
#include "ui/theme.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include <imgui.h>
#include <cstdio>

namespace omnidesk {

namespace {

// Color-coded quality indicator based on a metric value.
// Returns green for good, yellow for okay, red for bad.
ImVec4 qualityColor(float value, float goodThresh, float warnThresh, bool lowerIsBetter = true) {
    if (lowerIsBetter) {
        if (value <= goodThresh) return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);  // green
        if (value <= warnThresh) return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);  // yellow
        return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);                            // red
    } else {
        if (value >= goodThresh) return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
        if (value >= warnThresh) return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
        return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
    }
}

void statRow(const char* label, const char* value, ImVec4 valueColor = ImVec4(0.82f, 0.84f, 0.86f, 1.0f)) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(120);
    ImGui::TextColored(valueColor, "%s", value);
}

} // anonymous namespace

void StatsOverlay::render(HostSession* host, ViewerSession* viewer) {
    ImGuiIO& io = ImGui::GetIO();

    // Position in bottom-right corner with margin
    float overlayW = 280.0f;
    float overlayH = 0.0f; // auto-size height
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x - overlayW - 16, io.DisplaySize.y - 16),
        ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(overlayW, 0), ImVec2(overlayW, 400));
    ImGui::SetNextWindowBgAlpha(0.82f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.28f, 0.60f));

    ImGui::Begin("##StatsOverlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoScrollbar);

    // Header with role badge
    if (viewer) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.45f, 0.85f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.45f, 0.85f, 0.90f));
        ImGui::SmallButton(" VIEWER ");
        ImGui::PopStyleColor(3);
    } else if (host) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.30f, 0.80f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.30f, 0.80f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.30f, 0.80f, 0.90f));
        ImGui::SmallButton(" HOST ");
        ImGui::PopStyleColor(3);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Performance");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.32f, 0.50f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    char buf[64];

    if (viewer) {
        auto stats = viewer->getStats();

        // FPS
        snprintf(buf, sizeof(buf), "%.0f fps", stats.fps);
        statRow("Frame Rate", buf, qualityColor(stats.fps, 30.0f, 15.0f, false));

        // Bitrate
        snprintf(buf, sizeof(buf), "%.2f Mbps", stats.bitrateMbps);
        statRow("Bitrate", buf);

        // Latency
        snprintf(buf, sizeof(buf), "%.0f ms", stats.latencyMs);
        statRow("Latency", buf, qualityColor(stats.latencyMs, 50.0f, 150.0f));

        // Packet loss
        snprintf(buf, sizeof(buf), "%.1f%%", stats.packetLossPercent);
        statRow("Packet Loss", buf, qualityColor(stats.packetLossPercent, 1.0f, 5.0f));

        // Decode time
        snprintf(buf, sizeof(buf), "%.1f ms", stats.decodeTimeMs);
        statRow("Decode Time", buf, qualityColor(stats.decodeTimeMs, 8.0f, 16.0f));

        // Resolution
        snprintf(buf, sizeof(buf), "%dx%d", stats.width, stats.height);
        statRow("Resolution", buf);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.32f, 0.50f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Codec info
        if (!stats.encoderName.empty()) {
            statRow("Encoder", stats.encoderName.c_str());
        }
        if (!stats.decoderName.empty()) {
            statRow("Decoder", stats.decoderName.c_str());
        } else {
            statRow("Decoder", "Initializing...");
        }

    } else if (host) {
        auto stats = host->getStats();

        // FPS
        snprintf(buf, sizeof(buf), "%.0f fps", stats.fps);
        statRow("Frame Rate", buf, qualityColor(stats.fps, 30.0f, 15.0f, false));

        // Bitrate
        snprintf(buf, sizeof(buf), "%.2f Mbps", stats.bitrateMbps);
        statRow("Bitrate", buf);

        // Encode time
        snprintf(buf, sizeof(buf), "%.1f ms", stats.encodeTimeMs);
        statRow("Encode Time", buf, qualityColor(stats.encodeTimeMs, 8.0f, 16.0f));

        // Capture time
        snprintf(buf, sizeof(buf), "%.1f ms", stats.captureTimeMs);
        statRow("Capture Time", buf, qualityColor(stats.captureTimeMs, 5.0f, 12.0f));

        // Resolution
        snprintf(buf, sizeof(buf), "%dx%d", stats.width, stats.height);
        statRow("Resolution", buf);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.32f, 0.50f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Encoder info
        if (!stats.encoderName.empty()) {
            statRow("Encoder", stats.encoderName.c_str());
        } else {
            statRow("Encoder", "Initializing...");
        }

    } else {
        ImGui::TextDisabled("No active session");
    }

    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace omnidesk
