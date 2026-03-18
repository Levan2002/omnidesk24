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
#include "webrtc/webrtc_session.h"
#include "input/input_handler.h"
#include "input/input_injector.h"
#include "core/logger.h"

#include "render/gl_proc.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstring>

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

    // Register callbacks BEFORE connecting so no incoming message can
    // arrive between connect/registerUser and the callback being set.
    signaling_->onConnectionRequest([this](const ConnectionRequest& req) {
        UserID fromId = req.fromId;
        queueAction([this, fromId]() { handleConnectionRequest(fromId); });
    });
    signaling_->onConnectionRejected([this](const ConnectionRejection& /*rej*/) {
        queueAction([this]() {
            statusMessage_ = "Connection rejected by host";
            state_ = AppState::DASHBOARD;
            connectTimeoutSec_ = 0.0f;
        });
    });
    signaling_->onUserOffline([this](const UserID& /*targetId*/) {
        queueAction([this]() {
            statusMessage_ = "User is offline or not found";
            state_ = AppState::DASHBOARD;
            connectTimeoutSec_ = 0.0f;
        });
    });
    signaling_->onDisconnected([this]() {
        queueAction([this]() {
            if (state_ == AppState::CONNECTING) {
                statusMessage_ = "Lost connection to relay server";
                state_ = AppState::DASHBOARD;
                connectTimeoutSec_ = 0.0f;
            }
        });
    });
    signaling_->onRegistered([this](bool success) {
        queueAction([this, success]() {
            if (!success) {
                statusMessage_ = "Failed to register with relay server";
                LOG_ERROR("Signaling registration rejected by server");
            } else {
                LOG_INFO("Registered with relay server as %s", myId_.id.c_str());
            }
        });
    });

    // WebRTC signaling: wire SDP/ICE callbacks to the WebRtcSession.
    signaling_->onSdpOffer([this](const SdpMessage& msg) {
        queueAction([this, msg]() {
            if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "offer");
        });
    });
    signaling_->onSdpAnswer([this](const SdpMessage& msg) {
        queueAction([this, msg]() {
            if (webrtcSession_) webrtcSession_->onRemoteDescription(msg.sdp, "answer");
        });
    });
    signaling_->onIceCandidate([this](const IceCandidateMsg& ice) {
        queueAction([this, ice]() {
            if (webrtcSession_) webrtcSession_->onRemoteCandidate(ice.candidate, ice.sdpMid);
        });
    });

    // Try primary port, then each fallback in order.
    appRunning_ = true;
    tryConnectSignaling();

    // Start background reconnect thread — it will keep retrying if the
    // initial connection failed or if we get disconnected later.
    startReconnectThread();

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

void App::queueAction(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(pendingActionsMutex_);
    pendingActions_.push_back(std::move(action));
}

void App::run() {
    auto lastFrame = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        // Delta time for timers
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        // Process signaling messages
        if (signaling_) {
            signaling_->poll();
        }

        // Upload decoded video frames to GL textures (must be on GL thread)
        if (viewerSession_) {
            viewerSession_->processOnGlThread();
        }

        // Drain actions posted from the signaling receive thread
        {
            std::vector<std::function<void()>> toRun;
            {
                std::lock_guard<std::mutex> lock(pendingActionsMutex_);
                toRun.swap(pendingActions_);
            }
            for (auto& action : toRun) action();
        }

        // --- Connection timeout: if stuck in CONNECTING for >30 s, bail out ---
        if (state_ == AppState::CONNECTING) {
            connectTimeoutSec_ += dt;
            if (connectTimeoutSec_ > 30.0f) {
                connectTimeoutSec_ = 0.0f;
                statusMessage_ = "Connection timed out";
                state_ = AppState::DASHBOARD;
                LOG_WARN("Connect attempt timed out after 30 seconds");
            }
        } else {
            connectTimeoutSec_ = 0.0f;
        }

        // --- Status message auto-clear after 5 s ---
        if (!statusMessage_.empty()) {
            statusMessageTimer_ += dt;
            if (statusMessageTimer_ > 5.0f) {
                statusMessage_.clear();
                statusMessageTimer_ = 0.0f;
            }
        } else {
            statusMessageTimer_ = 0.0f;
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
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
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

                // Start host session (capture + encode pipeline)
                hostSession_ = std::make_unique<HostSession>();
                if (!hostSession_->start(config_.encoder, config_.capture)) {
                    statusMessage_ = "Failed to start host session";
                    signaling_->rejectConnection(pendingConnectionFrom_);
                    return;
                }

                // Close any previous WebRTC session before creating a new one.
                // This prevents the old session's onDisconnected callback from
                // firing during destruction and killing the new session.
                if (webrtcSession_) {
                    webrtcSession_->setOnDisconnected(nullptr);
                    webrtcSession_->close();
                    webrtcSession_.reset();
                }

                // Create WebRTC session for this peer
                WebRtcConfig wrtcCfg;
                wrtcCfg.turnServer = config_.turnServer;
                wrtcCfg.turnUser = config_.turnUser;
                wrtcCfg.turnPassword = config_.turnPassword;

                webrtcSession_ = std::make_unique<WebRtcSession>(
                    signaling_.get(), pendingConnectionFrom_, wrtcCfg);

                // Wire encoded packets from the host encode loop to the WebRTC video track
                hostSession_->setSendCallback([this](const EncodedPacket& pkt) {
                    if (webrtcSession_) {
                        webrtcSession_->sendVideo(pkt.data.data(), pkt.data.size());
                    }
                });

                webrtcSession_->setOnConnected([this]() {
                    queueAction([this]() {
                        state_ = AppState::SESSION_HOST;
                        if (hostSession_) hostSession_->requestKeyFrame();
                    });
                });
                webrtcSession_->setOnDisconnected([this]() {
                    queueAction([this]() { disconnectSession(); });
                });
                // Create input injector for remote control
                inputInjector_ = std::make_unique<InputInjector>();
                inputInjector_->setScreenSize(config_.encoder.width,
                                              config_.encoder.height);

                webrtcSession_->setOnData([this](const uint8_t* data, size_t size) {
                    // Message type 0x01 = input event
                    if (size >= 1 + InputEvent::SIZE && data[0] == 0x01) {
                        InputEvent ev = InputEvent::deserialize(data + 1);
                        if (inputInjector_) {
                            inputInjector_->inject(ev);
                        }
                    }
                });

                signaling_->acceptConnection(pendingConnectionFrom_);
                webrtcSession_->startAsHost();
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
    ImGuiIO& io = ImGui::GetIO();

    // Full-screen dim background
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.50f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##ConnBg", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Connecting card
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(320, 150));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.16f, 0.98f));

    ImGui::Begin("##Connecting", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // Animated dots based on frame time
    int dots = (static_cast<int>(ImGui::GetTime() * 2.0f) % 4);
    const char* dotStr[] = {"", ".", "..", "..."};

    ImGui::TextDisabled("Connecting to");
    ImGui::Spacing();

    // Target ID
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextColored(ImVec4(0.26f, 0.52f, 0.96f, 1.0f), "%s%s", connectIdInput_, dotStr[dots]);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Spacing();
    ImGui::Spacing();

    // Cancel button (full width, subdued)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.32f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    if (ImGui::Button("Cancel", ImVec2(-1, 34))) {
        state_ = AppState::DASHBOARD;
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
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
    if (!signaling_->isRegistered()) {
        statusMessage_ = "Not yet registered with server, please wait";
        return;
    }

    // Normalize to uppercase so old mixed-case IDs still match.
    std::string normalizedId = peerId;
    for (char& c : normalizedId) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    UserID targetId{normalizedId};

    // Set connection-result callbacks before sending the request to avoid races.
    signaling_->onConnectionAccepted([this, targetId](const ConnectionAcceptance& acc) {
        queueAction([this, targetId, acc]() {
            connectTimeoutSec_ = 0.0f;

            // Create viewer session (decoder + renderer)
            viewerSession_ = std::make_unique<ViewerSession>();
            if (!viewerSession_->start()) {
                statusMessage_ = "Failed to start viewer session";
                state_ = AppState::DASHBOARD;
                return;
            }

            // Close any previous WebRTC session cleanly
            if (webrtcSession_) {
                webrtcSession_->setOnDisconnected(nullptr);
                webrtcSession_->close();
                webrtcSession_.reset();
            }

            // Create WebRTC session for this peer
            WebRtcConfig wrtcCfg;
            wrtcCfg.turnServer = config_.turnServer;
            wrtcCfg.turnUser = config_.turnUser;
            wrtcCfg.turnPassword = config_.turnPassword;

            webrtcSession_ = std::make_unique<WebRtcSession>(
                signaling_.get(), acc.fromId, wrtcCfg);

            webrtcSession_->setOnConnected([this]() {
                queueAction([this]() {
                    state_ = AppState::SESSION_VIEWER;

                    // Set up input forwarding: capture local input and send to host
                    inputHandler_ = std::make_unique<InputHandler>();
                    inputHandler_->init(window_,
                                        config_.encoder.width,
                                        config_.encoder.height);

                    inputHandler_->setMouseCallback([this](const MouseEvent& mev) {
                        if (!webrtcSession_) return;

                        // Determine event type from the MouseEvent fields
                        InputEvent ev;
                        ev.x = mev.x;
                        ev.y = mev.y;

                        if (mev.scrollX != 0 || mev.scrollY != 0) {
                            ev.type = InputType::MOUSE_SCROLL;
                            ev.y = static_cast<int32_t>(mev.scrollY);  // scroll delta
                        } else if (mev.buttons != 0) {
                            ev.type = mev.pressed ? InputType::MOUSE_DOWN
                                                  : InputType::MOUSE_UP;
                            // Decode button bitmask to button index
                            if (mev.buttons & 1) ev.button = 0;       // left
                            else if (mev.buttons & 2) ev.button = 1;  // right
                            else if (mev.buttons & 4) ev.button = 2;  // middle
                        } else {
                            ev.type = InputType::MOUSE_MOVE;
                        }

                        uint8_t buf[1 + InputEvent::SIZE];
                        buf[0] = 0x01;  // message type: input event
                        ev.serialize(buf + 1);
                        webrtcSession_->sendData(buf, sizeof(buf));
                    });

                    inputHandler_->setKeyCallback([this](const KeyEvent& kev) {
                        if (!webrtcSession_) return;

                        InputEvent ev;
                        ev.type = kev.pressed ? InputType::KEY_DOWN : InputType::KEY_UP;
                        ev.scancode = kev.scancode;
                        ev.pressed = kev.pressed;

                        uint8_t buf[1 + InputEvent::SIZE];
                        buf[0] = 0x01;  // message type: input event
                        ev.serialize(buf + 1);
                        webrtcSession_->sendData(buf, sizeof(buf));
                    });

                    inputHandler_->setEnabled(true);
                });
            });
            webrtcSession_->setOnDisconnected([this]() {
                queueAction([this]() { disconnectSession(); });
            });
            webrtcSession_->setOnVideo([this](const uint8_t* data, size_t size) {
                if (viewerSession_) viewerSession_->onNalUnit(data, size);
            });
            webrtcSession_->setOnData([this](const uint8_t* /*data*/, size_t /*size*/) {
                // TODO: handle cursor updates from host
            });

            webrtcSession_->startAsViewer();
        });
    });

    if (!signaling_->requestConnection(targetId)) {
        statusMessage_ = "Failed to send connection request";
        return;
    }

    connectTimeoutSec_ = 0.0f;
    state_ = AppState::CONNECTING;
}

void App::tryConnectSignaling() {
    if (signaling_->isConnected()) return;

    if (!signaling_->connect(config_.signalingHost, config_.signalingPort,
                             config_.signalingFallbackPorts)) {
        LOG_WARN("Could not reach relay server %s on any port",
                 config_.signalingHost.c_str());
        queueAction([this]() {
            statusMessage_ = "Relay server unreachable — retrying...";
        });
    } else {
        LOG_INFO("Connected to relay %s:%d",
                 config_.signalingHost.c_str(), signaling_->connectedPort());

        // Register with the signaling server. With WebRTC, ICE handles
        // NAT traversal, so we don't need to pass a local address.
        signaling_->registerUser(myId_);
    }
}

void App::startReconnectThread() {
    reconnectThread_ = std::thread([this]() {
        // Wait a few seconds before first retry to give the initial
        // connection attempt time to fully settle.
        for (int i = 0; i < 50 && appRunning_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        while (appRunning_) {
            if (!signaling_->isConnected()) {
                LOG_INFO("Reconnect thread: signaling offline, retrying...");
                tryConnectSignaling();
            }

            // Wait 5 seconds between attempts.
            for (int i = 0; i < 50 && appRunning_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void App::disconnectSession() {
    if (inputHandler_) {
        inputHandler_->setEnabled(false);
        inputHandler_.reset();
    }
    inputInjector_.reset();
    if (webrtcSession_) webrtcSession_->close();
    webrtcSession_.reset();
    hostSession_.reset();
    viewerSession_.reset();
    state_ = AppState::DASHBOARD;
    statusMessage_ = "Disconnected";
    LOG_INFO("Session disconnected");
}

void App::shutdown() {
    appRunning_ = false;

    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }

    if (inputHandler_) {
        inputHandler_->setEnabled(false);
        inputHandler_.reset();
    }
    inputInjector_.reset();
    if (webrtcSession_) webrtcSession_->close();
    webrtcSession_.reset();
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
