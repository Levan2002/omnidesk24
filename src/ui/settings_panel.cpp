#include "ui/settings_panel.h"
#include <imgui.h>

namespace omnidesk {

void SettingsPanel::render(AppConfig& config, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(450, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", open)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // Video tab
        if (ImGui::BeginTabItem("Video")) {
            ImGui::Text("Quality Preset:");
            static int preset = 2; // High
            ImGui::RadioButton("Low (500 kbps)", &preset, 0); ImGui::SameLine();
            ImGui::RadioButton("Medium (1 Mbps)", &preset, 1); ImGui::SameLine();
            ImGui::RadioButton("High (2 Mbps)", &preset, 2); ImGui::SameLine();
            ImGui::RadioButton("Ultra (5 Mbps)", &preset, 3);

            const uint32_t bitratePresets[] = {500000, 1000000, 2000000, 5000000};
            config.encoder.targetBitrateBps = bitratePresets[preset];

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            int maxFps = static_cast<int>(config.encoder.maxFps);
            ImGui::SliderInt("Max FPS", &maxFps, 15, 60);
            config.encoder.maxFps = static_cast<float>(maxFps);

            float bitrateKbps = static_cast<float>(config.encoder.targetBitrateBps) / 1000.0f;
            ImGui::SliderFloat("Target Bitrate (kbps)", &bitrateKbps, 100, 10000, "%.0f");
            config.encoder.targetBitrateBps = static_cast<uint32_t>(bitrateKbps * 1000);

            float maxBitrateKbps = static_cast<float>(config.encoder.maxBitrateBps) / 1000.0f;
            ImGui::SliderFloat("Max Bitrate (kbps)", &maxBitrateKbps, 500, 20000, "%.0f");
            config.encoder.maxBitrateBps = static_cast<uint32_t>(maxBitrateKbps * 1000);

            ImGui::Checkbox("Screen Content Mode", &config.encoder.screenContent);
            ImGui::Checkbox("Adaptive Quantization", &config.encoder.adaptiveQuantization);

            int layers = config.encoder.temporalLayers;
            ImGui::SliderInt("Temporal Layers", &layers, 1, 4);
            config.encoder.temporalLayers = static_cast<uint8_t>(layers);

            ImGui::EndTabItem();
        }

        // Network tab
        if (ImGui::BeginTabItem("Network")) {
            ImGui::Spacing();
            ImGui::Text("FEC Strength:");
            static int fecStrength = 1;
            ImGui::RadioButton("Off", &fecStrength, 0); ImGui::SameLine();
            ImGui::RadioButton("Low", &fecStrength, 1); ImGui::SameLine();
            ImGui::RadioButton("Medium", &fecStrength, 2); ImGui::SameLine();
            ImGui::RadioButton("High", &fecStrength, 3);

            ImGui::EndTabItem();
        }

        // Display tab
        if (ImGui::BeginTabItem("Display")) {
            static bool sharpening = true;
            ImGui::Checkbox("Text Sharpening (CAS)", &sharpening);

            static int scalingFilter = 0;
            ImGui::Combo("Scaling Filter", &scalingFilter, "Bilinear\0Nearest\0");

            static bool showCursor = true;
            ImGui::Checkbox("Show Remote Cursor", &showCursor);

            ImGui::EndTabItem();
        }

        // Advanced tab
        if (ImGui::BeginTabItem("Advanced")) {
            static int encoderChoice = 0;
            ImGui::Combo("Encoder", &encoderChoice, "Auto\0Software (OpenH264)\0Hardware\0");

            static int logLevel = 1;
            ImGui::Combo("Log Level", &logLevel, "Debug\0Info\0Warning\0Error\0");

            static bool statsOverlay = false;
            ImGui::Checkbox("Show Stats Overlay in Session", &statsOverlay);

            int monitorId = config.capture.monitorId;
            ImGui::InputInt("Monitor ID (-1 = primary)", &monitorId);
            config.capture.monitorId = monitorId;

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

} // namespace omnidesk
