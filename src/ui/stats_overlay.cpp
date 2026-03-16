#include "ui/stats_overlay.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include <imgui.h>

namespace omnidesk {

void StatsOverlay::render(HostSession* host, ViewerSession* viewer) {
    ImGui::SetNextWindowPos(ImVec2(10, 50), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.6f);

    ImGui::Begin("Stats", nullptr,
                 ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav);

    if (viewer) {
        auto stats = viewer->getStats();
        ImGui::Text("Role: Viewer");
        ImGui::Separator();
        ImGui::Text("FPS:         %.1f", stats.fps);
        ImGui::Text("Bitrate:     %.2f Mbps", stats.bitrateMbps);
        ImGui::Text("Latency:     %.1f ms", stats.latencyMs);
        ImGui::Text("Packet Loss: %.1f%%", stats.packetLossPercent);
        ImGui::Text("Decode Time: %.1f ms", stats.decodeTimeMs);
        ImGui::Text("Resolution:  %dx%d", stats.width, stats.height);
        ImGui::Text("Encoder:     %s", stats.encoderName.c_str());
    } else if (host) {
        auto stats = host->getStats();
        ImGui::Text("Role: Host");
        ImGui::Separator();
        ImGui::Text("FPS:          %.1f", stats.fps);
        ImGui::Text("Bitrate:      %.2f Mbps", stats.bitrateMbps);
        ImGui::Text("Encode Time:  %.1f ms", stats.encodeTimeMs);
        ImGui::Text("Capture Time: %.1f ms", stats.captureTimeMs);
        ImGui::Text("Resolution:   %dx%d", stats.width, stats.height);
        ImGui::Text("Encoder:      %s", stats.encoderName.c_str());
    } else {
        ImGui::Text("No active session");
    }

    ImGui::End();
}

} // namespace omnidesk
