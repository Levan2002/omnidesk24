#include "ui/app.h"
#include "ui/theme.h"
#include "ui/dashboard.h"
#include "ui/session_view.h"
#include "ui/settings_panel.h"
#include "ui/connection_dialog.h"
#include "ui/stats_overlay.h"
#include "ui/history_panel.h"
#include "signaling/signaling_client.h"
#include "signaling/user_id.h"
#include "session/host_session.h"
#include "session/viewer_session.h"
#include "core/logger.h"

#include "render/gl_proc.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

namespace omnidesk {

App::App() = default;
App::~App() { shutdown(); }

bool App::init(const AppConfig& config) {
    config_ = config;

    // Initialize GLFW
    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    window_ = glfwCreateWindow(config_.windowWidth, config_.windowHeight,
                               "OmniDesk24", nullptr, nullptr);
    if (!window_) {
        LOG_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1); // VSync

    loadGLProcs();
    initImGui();

    // Generate or load user ID
    UserIdGenerator idGen;
    myId_ = idGen.loadOrGenerate();
    LOG_INFO("User ID: %s", myId_.id.c_str());

    // Connect to signaling server
    signaling_ = std::make_unique<SignalingClient>();
    if (!signaling_->connect(config_.signalingHost, config_.signalingPort)) {
        LOG_WARN("Could not connect to signaling server at %s:%d",
                 config_.signalingHost.c_str(), config_.signalingPort);
        statusMessage_ = "Signaling server offline";
    } else {
        signaling_->registerUser(myId_);
        signaling_->onConnectionRequest([this](const ConnectionRequest& req) {
            handleConnectionRequest(req.fromId);
        });
    }

    return true;
}

void App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    float dpiScale = xscale > 0.0f ? xscale : 1.0f;

    ImFontConfig fontCfg;
    fontCfg.SizePixels = 16.0f * dpiScale;
    io.Fonts->AddFontDefault(&fontCfg);

    Theme::apply();
    ImGui::GetStyle().ScaleAllSizes(dpiScale);

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");
}

void App::run() {
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        // Process signaling messages
        if (signaling_) {
            signaling_->poll();
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        renderFrame();

        // Render
        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window_, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);
    }
}

void App::renderFrame() {
    switch (state_) {
        case AppState::DASHBOARD:
            renderDashboard();
            break;
        case AppState::CONNECTING:
            renderConnecting();
            break;
        case AppState::SESSION_HOST:
        case AppState::SESSION_VIEWER:
            renderSession();
            break;
    }

    // Connection dialog (modal)
    if (showConnectionDialog_) {
        ConnectionDialog::render(pendingConnectionFrom_,
            [this]() { // Accept
                showConnectionDialog_ = false;
                // Start host session
                hostSession_ = std::make_unique<HostSession>();
                if (hostSession_->start(config_.encoder, config_.capture)) {
                    state_ = AppState::SESSION_HOST;
                    signaling_->acceptConnection(pendingConnectionFrom_);
                } else {
                    statusMessage_ = "Failed to start host session";
                    signaling_->rejectConnection(pendingConnectionFrom_);
                }
            },
            [this]() { // Reject
                showConnectionDialog_ = false;
                signaling_->rejectConnection(pendingConnectionFrom_);
            }
        );
    }

    // Settings panel
    if (showSettings_) {
        SettingsPanel::render(config_, &showSettings_);
    }

    // Stats overlay
    if (showStats_ && (state_ == AppState::SESSION_HOST || state_ == AppState::SESSION_VIEWER)) {
        StatsOverlay::render(hostSession_.get(), viewerSession_.get());
    }
}

void App::renderDashboard() {
    Dashboard::render(myId_, connectIdInput_, sizeof(connectIdInput_),
        [this]() { connectToPeer(connectIdInput_); },
        [this]() { showSettings_ = true; },
        signaling_ && signaling_->isConnected(),
        statusMessage_
    );
    HistoryPanel::render();
}

void App::renderConnecting() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                    ImGui::GetIO().DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 120));
    ImGui::Begin("Connecting", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Connecting to %s...", connectIdInput_);
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
        state_ = AppState::DASHBOARD;
    }
    ImGui::End();
}

void App::renderSession() {
    // Session view with toolbar
    SessionView::render(
        state_ == AppState::SESSION_HOST ? hostSession_.get() : nullptr,
        state_ == AppState::SESSION_VIEWER ? viewerSession_.get() : nullptr,
        [this]() { disconnectSession(); },
        [this]() { showStats_ = !showStats_; },
        [this]() { showSettings_ = !showSettings_; }
    );
}

void App::handleConnectionRequest(const UserID& fromUser) {
    pendingConnectionFrom_ = fromUser;
    showConnectionDialog_ = true;
    LOG_INFO("Connection request from %s", fromUser.id.c_str());
}

void App::connectToPeer(const std::string& peerId) {
    if (peerId.empty() || peerId.length() != 8) {
        statusMessage_ = "Invalid ID (must be 8 characters)";
        return;
    }
    if (!signaling_ || !signaling_->isConnected()) {
        statusMessage_ = "Not connected to signaling server";
        return;
    }

    UserID targetId{peerId};
    state_ = AppState::CONNECTING;

    signaling_->requestConnection(targetId);
    signaling_->onConnectionAccepted([this](const ConnectionAcceptance& /*acc*/) {
        // Start viewer session
        viewerSession_ = std::make_unique<ViewerSession>();
        if (viewerSession_->start()) {
            state_ = AppState::SESSION_VIEWER;
        } else {
            statusMessage_ = "Failed to start viewer session";
            state_ = AppState::DASHBOARD;
        }
    });
    signaling_->onConnectionRejected([this](const ConnectionRejection& /*rej*/) {
        statusMessage_ = "Connection rejected";
        state_ = AppState::DASHBOARD;
    });
}

void App::disconnectSession() {
    hostSession_.reset();
    viewerSession_.reset();
    state_ = AppState::DASHBOARD;
    statusMessage_ = "Disconnected";
    LOG_INFO("Session disconnected");
}

void App::shutdown() {
    hostSession_.reset();
    viewerSession_.reset();
    signaling_.reset();

    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        window_ = nullptr;
    }
}

} // namespace omnidesk
