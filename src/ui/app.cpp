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
#include <chrono>
#include <cstring>

namespace omnidesk {

App::App() = default;
App::~App() { shutdown(); }

// Detect the local LAN IP by creating a UDP socket and reading bound address.
std::string App::getLocalIp() {
    SocketInitializer::initialize();

#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "127.0.0.1";
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "127.0.0.1";
#endif

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &server.sin_addr);

    ::connect(s, reinterpret_cast<sockaddr*>(&server), sizeof(server));

    sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);

    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));

#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif

    return std::string(buf);
}

// Serialize an EncodedPacket into a byte buffer for TCP transmission.
static std::vector<uint8_t> serializePacket(const EncodedPacket& pkt) {
    // Layout: frameId(4) + timestampUs(8) + flags(1) + rectCount(1) +
    //         rects(rectCount * 16) + h264 data
    uint8_t rectCount = static_cast<uint8_t>(
        pkt.dirtyRects.size() > 255 ? 255 : pkt.dirtyRects.size());
    size_t headerSize = 4 + 8 + 1 + 1 + rectCount * 16;
    std::vector<uint8_t> buf(headerSize + pkt.data.size());

    uint8_t* p = buf.data();
    writeU32(p, static_cast<uint32_t>(pkt.frameId)); p += 4;
    // Write 8-byte timestamp as two 32-bit words (high, low)
    writeU32(p, static_cast<uint32_t>(pkt.timestampUs >> 32)); p += 4;
    writeU32(p, static_cast<uint32_t>(pkt.timestampUs & 0xFFFFFFFF)); p += 4;
    *p++ = pkt.isKeyFrame ? 1 : 0;
    *p++ = rectCount;
    for (uint8_t i = 0; i < rectCount; ++i) {
        const auto& r = pkt.dirtyRects[i];
        writeU32(p, static_cast<uint32_t>(r.x));     p += 4;
        writeU32(p, static_cast<uint32_t>(r.y));     p += 4;
        writeU32(p, static_cast<uint32_t>(r.width));  p += 4;
        writeU32(p, static_cast<uint32_t>(r.height)); p += 4;
    }
    std::memcpy(p, pkt.data.data(), pkt.data.size());
    return buf;
}

// Deserialize an EncodedPacket from a byte buffer received over TCP.
static EncodedPacket deserializePacket(const std::vector<uint8_t>& buf) {
    EncodedPacket pkt;
    if (buf.size() < 14) return pkt;

    const uint8_t* p = buf.data();
    pkt.frameId = readU32(p); p += 4;
    uint64_t tsHi = readU32(p); p += 4;
    uint64_t tsLo = readU32(p); p += 4;
    pkt.timestampUs = (tsHi << 32) | tsLo;
    pkt.isKeyFrame = (*p++ != 0);
    uint8_t rectCount = *p++;

    size_t headerSize = 14 + rectCount * 16;
    if (buf.size() < headerSize) return pkt;

    pkt.dirtyRects.resize(rectCount);
    for (uint8_t i = 0; i < rectCount; ++i) {
        pkt.dirtyRects[i].x      = static_cast<int32_t>(readU32(p)); p += 4;
        pkt.dirtyRects[i].y      = static_cast<int32_t>(readU32(p)); p += 4;
        pkt.dirtyRects[i].width  = static_cast<int32_t>(readU32(p)); p += 4;
        pkt.dirtyRects[i].height = static_cast<int32_t>(readU32(p)); p += 4;
    }

    pkt.data.assign(p, buf.data() + buf.size());
    return pkt;
}

void App::sendVideoPacket(const EncodedPacket& packet) {
    auto buf = serializePacket(packet);

    if (relayMode_) {
        // Send via signaling server relay
        if (signaling_ && signaling_->isConnected() && relayPeerId_.valid()) {
            signaling_->sendRelayData(relayPeerId_, MessageType::VIDEO_DATA,
                                       buf.data(), static_cast<uint32_t>(buf.size()));
        }
        return;
    }

    std::lock_guard<std::mutex> lock(dataChannelMutex_);
    if (!dataChannel_ || !dataChannel_->isOpen()) return;
    dataChannel_->send(MessageType::VIDEO_DATA, buf);
}

void App::viewerReceiveLoop() {
    while (dataRunning_ && dataChannel_ && dataChannel_->isOpen()) {
        if (!dataChannel_->pollRead(100)) continue;

        MessageType type;
        std::vector<uint8_t> payload;
        auto result = dataChannel_->recv(type, payload);

        if (result == SocketResult::OK && type == MessageType::VIDEO_DATA) {
            auto pkt = deserializePacket(payload);
            if (viewerSession_) {
                viewerSession_->onVideoPacket(pkt);
            }
        } else if (result == SocketResult::DISCONNECTED ||
                   result == SocketResult::SOCK_ERROR) {
            LOG_WARN("Data channel disconnected");
            queueAction([this]() {
                disconnectSession();
                statusMessage_ = "Connection to host lost";
            });
            break;
        }
    }
}

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

    // Detect local IP for P2P data connections
    localIp_ = getLocalIp();
    LOG_INFO("Local IP: %s", localIp_.c_str());

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
                if (!hostSession_->start(config_.encoder, config_.capture)) {
                    statusMessage_ = "Failed to start host session";
                    signaling_->rejectConnection(pendingConnectionFrom_);
                    return;
                }

                // Set up send callback: encode loop -> TCP data channel or relay
                hostSession_->setSendCallback([this](const EncodedPacket& pkt) {
                    sendVideoPacket(pkt);
                });

                // Enable relay mode immediately so video starts flowing
                // through the signaling server without waiting for P2P.
                relayMode_ = true;
                relayPeerId_ = pendingConnectionFrom_;

                // Register relay callback for host (viewer may send data back)
                signaling_->onRelayData([this](const UserID& fromId,
                                                MessageType innerType,
                                                const std::vector<uint8_t>& payload) {
                    // Host currently doesn't receive video, but handle
                    // future input relay here if needed.
                    (void)fromId; (void)innerType; (void)payload;
                });

                state_ = AppState::SESSION_HOST;
                signaling_->acceptConnection(pendingConnectionFrom_);

                // Force a keyframe so the viewer can start decoding
                // as soon as relay data arrives.
                if (hostSession_) {
                    hostSession_->requestKeyFrame();
                }

                // Also start P2P data listener — if the viewer can connect
                // directly, we'll switch off relay for better performance.
                dataRunning_ = true;
                if (dataListener_.listen(DATA_PORT)) {
                    LOG_INFO("Data listener started on port %d", DATA_PORT);
                    dataAcceptThread_ = std::thread([this]() {
                        LOG_INFO("Waiting for viewer P2P connect on data port...");
                        for (int i = 0; i < 300 && dataRunning_; ++i) {
                            if (dataListener_.pollRead(100)) {
                                auto accepted = dataListener_.accept();
                                if (accepted) {
                                    LOG_INFO("Viewer connected via P2P from %s:%d, switching off relay",
                                             accepted->remoteAddress().c_str(),
                                             accepted->remotePort());
                                    {
                                        std::lock_guard<std::mutex> lock(dataChannelMutex_);
                                        dataChannel_ = std::move(accepted);
                                    }
                                    // Switch from relay to direct P2P
                                    relayMode_ = false;
                                    if (hostSession_) {
                                        hostSession_->requestKeyFrame();
                                    }
                                    return;
                                }
                            }
                        }
                        LOG_INFO("No P2P viewer within timeout, staying on relay");
                        dataListener_.close();
                    });
                } else {
                    LOG_WARN("Could not listen on data port %d, relay-only mode", DATA_PORT);
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
    if (!signaling_->isRegistered()) {
        statusMessage_ = "Not yet registered with server, please wait";
        return;
    }

    // Normalize to uppercase so old mixed-case IDs still match.
    std::string normalizedId = peerId;
    for (char& c : normalizedId) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    UserID targetId{normalizedId};

    // Set connection-result callbacks before sending the request to avoid races.
    signaling_->onConnectionAccepted([this](const ConnectionAcceptance& acc) {
        // Extract host's address for P2P data connection.
        // Try local address first (same LAN), fall back to public.
        PeerAddress hostAddr = acc.peerLocalAddr.valid()
            ? acc.peerLocalAddr : acc.peerPublicAddr;

        LOG_INFO("Host addresses — local: %s:%d, public: %s:%d, using: %s",
                 acc.peerLocalAddr.host.c_str(), acc.peerLocalAddr.port,
                 acc.peerPublicAddr.host.c_str(), acc.peerPublicAddr.port,
                 acc.peerLocalAddr.valid() ? "local" : "public");

        // Override port with our fixed data port
        hostAddr.port = DATA_PORT;

        // Build fallback list: try local first, then public
        std::vector<PeerAddress> addrsToTry;
        addrsToTry.push_back(hostAddr);
        if (acc.peerLocalAddr.valid() && acc.peerPublicAddr.valid() &&
            acc.peerLocalAddr.host != acc.peerPublicAddr.host) {
            PeerAddress pubAddr = acc.peerPublicAddr;
            pubAddr.port = DATA_PORT;
            addrsToTry.push_back(pubAddr);
        }

        UserID hostId = acc.fromId;

        queueAction([this, addrsToTry, hostId]() {
            connectTimeoutSec_ = 0.0f;

            // Create viewer session
            viewerSession_ = std::make_unique<ViewerSession>();
            if (!viewerSession_->start()) {
                statusMessage_ = "Failed to start viewer session";
                state_ = AppState::DASHBOARD;
                return;
            }

            // Try each address until one connects (P2P)
            dataRunning_ = true;
            dataChannel_ = std::make_unique<TcpChannel>();
            bool connected = false;

            for (const auto& addr : addrsToTry) {
                LOG_INFO("Trying host data channel at %s:%d",
                         addr.host.c_str(), addr.port);
                if (dataChannel_->connect(addr.host, addr.port, 5000)) {
                    connected = true;
                    break;
                }
                LOG_WARN("Failed to connect to %s:%d, trying next...",
                         addr.host.c_str(), addr.port);
                dataChannel_ = std::make_unique<TcpChannel>();
            }

            if (!connected) {
                // P2P failed — fall back to relay through signaling server
                LOG_INFO("P2P failed, falling back to relay mode for host %s",
                         hostId.id.c_str());
                dataChannel_.reset();

                relayMode_ = true;
                relayPeerId_ = hostId;

                // Register relay callback: incoming VIDEO_DATA → viewer session
                signaling_->onRelayData([this](const UserID& fromId,
                                                MessageType innerType,
                                                const std::vector<uint8_t>& payload) {
                    if (innerType == MessageType::VIDEO_DATA && viewerSession_) {
                        auto pkt = deserializePacket(payload);
                        viewerSession_->onVideoPacket(pkt);
                    }
                });

                state_ = AppState::SESSION_VIEWER;
                LOG_INFO("Relay mode active — viewing via server relay");
                return;
            }

            LOG_INFO("Connected to host data channel (P2P)");
            state_ = AppState::SESSION_VIEWER;

            // Start receiving video packets in background
            dataRecvThread_ = std::thread(&App::viewerReceiveLoop, this);
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

        // Use the signaling TCP socket's local address — this is the actual
        // LAN IP the OS chose for this connection, which is more reliable
        // than the UDP getsockname trick (avoids Docker/VPN adapter issues).
        std::string sigLocalIp = signaling_->localAddress();
        if (!sigLocalIp.empty() && sigLocalIp != "0.0.0.0") {
            localIp_ = sigLocalIp;
            LOG_INFO("Local IP (from signaling socket): %s", localIp_.c_str());
        }

        // Register with our local IP and data port so the signaling server
        // can relay our address to peers.
        PeerAddress localAddr;
        localAddr.host = localIp_;
        localAddr.port = DATA_PORT;
        signaling_->registerUser(myId_, localAddr);
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
    // Stop data threads
    dataRunning_ = false;
    {
        std::lock_guard<std::mutex> lock(dataChannelMutex_);
        if (dataChannel_) dataChannel_->close();
    }
    dataListener_.close();
    if (dataAcceptThread_.joinable()) dataAcceptThread_.join();
    if (dataRecvThread_.joinable()) dataRecvThread_.join();
    {
        std::lock_guard<std::mutex> lock(dataChannelMutex_);
        dataChannel_.reset();
    }

    // Clear relay state
    relayMode_ = false;
    relayPeerId_ = UserID{};
    if (signaling_) {
        signaling_->onRelayData(nullptr);
    }

    hostSession_.reset();
    viewerSession_.reset();
    state_ = AppState::DASHBOARD;
    statusMessage_ = "Disconnected";
    LOG_INFO("Session disconnected");
}

void App::shutdown() {
    appRunning_ = false;
    dataRunning_ = false;

    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }

    // Close data channel before joining threads
    {
        std::lock_guard<std::mutex> lock(dataChannelMutex_);
        if (dataChannel_) dataChannel_->close();
    }
    dataListener_.close();
    if (dataAcceptThread_.joinable()) dataAcceptThread_.join();
    if (dataRecvThread_.joinable()) dataRecvThread_.join();

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
