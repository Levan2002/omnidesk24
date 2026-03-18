// Integration test: End-to-end WebRTC video flow.
// Starts a local SignalingServer, creates two WebRtcSession instances
// (host + viewer), connects them via real SDP/ICE exchange through
// the local signaling server, encodes a test frame with OpenH264,
// sends it over the WebRTC video DataChannel, receives and decodes
// on the viewer side, and verifies the decoded frame.
//
// No GUI dependencies — headless, no GLFW/OpenGL.

#include <gtest/gtest.h>

#include "core/types.h"
#include "core/logger.h"
#include "signaling/signaling_server.h"
#include "signaling/signaling_client.h"
#include "signaling/tcp_channel.h"
#include "webrtc/webrtc_session.h"
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"
#include "codec/codec_factory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omnidesk {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Wait for a predicate to become true, with timeout.
template <typename Pred>
bool waitFor(std::mutex& mu, std::condition_variable& cv, Pred pred,
             int timeoutMs = 10000) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred);
}

/// Wait for an atomic bool to become true using polling (for callbacks
/// that fire from internal libdatachannel threads where cv notification
/// is impractical).
bool waitForAtomic(const std::atomic<bool>& flag, int timeoutMs = 15000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

/// Create a deterministic 8-char user ID for testing.
UserID makeTestId(const char* suffix) {
    std::string id = "TE2E";
    id += suffix;
    while (id.size() < 8) id += 'X';
    id.resize(8);
    return UserID{id};
}

/// Generate a solid-color I420 test frame.
/// The Y plane is filled with yVal, U with uVal, V with vVal.
Frame generateTestFrameI420(int width, int height,
                            uint8_t yVal, uint8_t uVal, uint8_t vVal,
                            uint64_t frameId = 1) {
    Frame f;
    f.allocate(width, height, PixelFormat::I420);
    f.frameId = frameId;
    f.timestampUs = frameId * 33333; // ~30fps timestamps

    // Fill Y plane
    std::memset(f.plane(0), yVal,
                static_cast<size_t>(f.stride) * f.height);

    // Fill U plane
    std::memset(f.plane(1), uVal,
                static_cast<size_t>(f.stride / 2) * (f.height / 2));

    // Fill V plane
    std::memset(f.plane(2), vVal,
                static_cast<size_t>(f.stride / 2) * (f.height / 2));

    return f;
}

/// Generate a gradient I420 test frame for richer content.
Frame generateGradientFrameI420(int width, int height,
                                uint64_t frameId = 1) {
    Frame f;
    f.allocate(width, height, PixelFormat::I420);
    f.frameId = frameId;
    f.timestampUs = frameId * 33333;

    // Y plane: horizontal gradient
    uint8_t* yPlane = f.plane(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            yPlane[y * f.stride + x] =
                static_cast<uint8_t>((x * 255) / std::max(width - 1, 1));
        }
    }

    // U plane: vertical gradient
    uint8_t* uPlane = f.plane(1);
    int uvH = height / 2;
    int uvW = width / 2;
    int uvStride = f.stride / 2;
    for (int y = 0; y < uvH; ++y) {
        for (int x = 0; x < uvW; ++x) {
            uPlane[y * uvStride + x] =
                static_cast<uint8_t>(128 + (y * 64) / std::max(uvH - 1, 1));
        }
    }

    // V plane: constant mid-gray
    uint8_t* vPlane = f.plane(2);
    std::memset(vPlane, 128,
                static_cast<size_t>(uvStride) * uvH);

    return f;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class WebRtcE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        SocketInitializer::initialize();

        // Start a local signaling server on an OS-assigned port.
        server_ = std::make_unique<SignalingServer>();
        ASSERT_TRUE(server_->start(0));
        serverPort_ = server_->port();
        ASSERT_NE(serverPort_, 0u);
    }

    void TearDown() override {
        hostSignaling_.disconnect();
        viewerSignaling_.disconnect();
        server_->stop();
    }

    /// Connect and register a signaling client with the local server.
    bool connectAndRegister(SignalingClient& client, const UserID& id) {
        client.setAutoReconnect(false);
        if (!client.connect("127.0.0.1", serverPort_)) return false;

        std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> done{false};
        std::atomic<bool> ok{false};

        client.onRegistered([&](bool success) {
            ok.store(success);
            done.store(true);
            std::lock_guard<std::mutex> lk(mu);
            cv.notify_one();
        });

        PeerAddress localAddr;
        localAddr.host = "127.0.0.1";
        localAddr.port = 0;
        if (!client.registerUser(id, localAddr)) return false;

        return waitFor(mu, cv, [&] { return done.load(); }, 5000) && ok.load();
    }

    /// Wire up SDP/ICE forwarding between signaling clients and WebRTC sessions.
    void wireSignalingToWebRtc(SignalingClient& sigClient,
                               WebRtcSession& localSession,
                               WebRtcSession& /*remoteSession*/) {
        // When this client receives an SDP offer, feed it to the local session.
        sigClient.onSdpOffer([&localSession](const SdpMessage& msg) {
            LOG_INFO("Test: forwarding SDP offer to local session");
            localSession.onRemoteDescription(msg.sdp, msg.type);
        });

        // When this client receives an SDP answer, feed it to the local session.
        sigClient.onSdpAnswer([&localSession](const SdpMessage& msg) {
            LOG_INFO("Test: forwarding SDP answer to local session");
            localSession.onRemoteDescription(msg.sdp, msg.type);
        });

        // When this client receives an ICE candidate, feed it to the local session.
        sigClient.onIceCandidate([&localSession](const IceCandidateMsg& msg) {
            LOG_INFO("Test: forwarding ICE candidate to local session");
            localSession.onRemoteCandidate(msg.candidate, msg.sdpMid);
        });
    }

    std::unique_ptr<SignalingServer> server_;
    uint16_t serverPort_ = 0;
    SignalingClient hostSignaling_;
    SignalingClient viewerSignaling_;
};

// ---------------------------------------------------------------------------
// TEST: Full host-to-viewer video flow through WebRTC
// ---------------------------------------------------------------------------

TEST_F(WebRtcE2ETest, HostToViewerVideoFlow) {
    // ---- Step 1: Register both peers with the signaling server ----
    UserID hostId  = makeTestId("HST1");
    UserID viewerId = makeTestId("VWR1");

    ASSERT_TRUE(connectAndRegister(hostSignaling_, hostId))
        << "Host failed to register with signaling server";
    ASSERT_TRUE(connectAndRegister(viewerSignaling_, viewerId))
        << "Viewer failed to register with signaling server";

    // ---- Step 2: Create WebRTC sessions ----
    WebRtcConfig rtcConfig;
    // Use Google STUN for ICE; no TURN needed for loopback.
    rtcConfig.stunServers = {"stun:stun.l.google.com:19302"};

    WebRtcSession hostSession(&hostSignaling_, viewerId, rtcConfig);
    WebRtcSession viewerSession(&viewerSignaling_, hostId, rtcConfig);

    // Track connection state.
    std::atomic<bool> hostConnected{false};
    std::atomic<bool> viewerConnected{false};

    hostSession.setOnConnected([&]() {
        LOG_INFO("Test: HOST WebRTC connected");
        hostConnected.store(true);
    });
    viewerSession.setOnConnected([&]() {
        LOG_INFO("Test: VIEWER WebRTC connected");
        viewerConnected.store(true);
    });

    // ---- Step 3: Wire signaling callbacks to WebRTC sessions ----
    // Host signaling receives SDP answers and ICE candidates from viewer.
    wireSignalingToWebRtc(hostSignaling_, hostSession, viewerSession);
    // Viewer signaling receives SDP offers and ICE candidates from host.
    wireSignalingToWebRtc(viewerSignaling_, viewerSession, hostSession);

    // ---- Step 4: Signaling handshake: host requests, viewer accepts ----
    std::mutex muReq;
    std::condition_variable cvReq;
    std::atomic<bool> requestReceived{false};

    viewerSignaling_.onConnectionRequest([&](const ConnectionRequest& req) {
        EXPECT_EQ(req.fromId, hostId);
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    std::mutex muAcc;
    std::condition_variable cvAcc;
    std::atomic<bool> accepted{false};

    hostSignaling_.onConnectionAccepted([&](const ConnectionAcceptance& /*acc*/) {
        LOG_INFO("Test: connection accepted by viewer");
        accepted.store(true);
        std::lock_guard<std::mutex> lk(muAcc);
        cvAcc.notify_one();
    });

    ASSERT_TRUE(hostSignaling_.requestConnection(viewerId))
        << "Host failed to send connection request";

    ASSERT_TRUE(waitFor(muReq, cvReq, [&] { return requestReceived.load(); }, 5000))
        << "Viewer did not receive connection request from host";

    ASSERT_TRUE(viewerSignaling_.acceptConnection(hostId))
        << "Viewer failed to accept connection";

    ASSERT_TRUE(waitFor(muAcc, cvAcc, [&] { return accepted.load(); }, 5000))
        << "Host did not receive acceptance from viewer";

    // ---- Step 5: Start WebRTC sessions ----
    // Viewer starts first (waits for offer).
    ASSERT_TRUE(viewerSession.startAsViewer())
        << "Viewer WebRTC session failed to start";

    // Host starts (creates offer, DataChannels).
    ASSERT_TRUE(hostSession.startAsHost())
        << "Host WebRTC session failed to start";

    // ---- Step 6: Wait for WebRTC connection ----
    ASSERT_TRUE(waitForAtomic(hostConnected, 15000))
        << "Host WebRTC did not connect within 15 seconds";
    ASSERT_TRUE(waitForAtomic(viewerConnected, 15000))
        << "Viewer WebRTC did not connect within 15 seconds";

    LOG_INFO("Test: Both peers connected via WebRTC");

    // Give DataChannels a moment to fully open.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- Step 7: Encode a test frame with OpenH264 ----
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;

    auto encoder = CodecFactory::createEncoder(CodecBackend::OpenH264);
    ASSERT_NE(encoder, nullptr) << "Failed to create OpenH264 encoder";

    EncoderConfig encCfg;
    encCfg.width = kWidth;
    encCfg.height = kHeight;
    encCfg.targetBitrateBps = 500000;
    encCfg.maxBitrateBps = 1000000;
    encCfg.maxFps = 30.0f;
    encCfg.screenContent = true;
    encCfg.temporalLayers = 1;

    ASSERT_TRUE(encoder->init(encCfg))
        << "OpenH264 encoder init failed (is libopenh264 available?)";

    // Generate and encode a solid-color test frame.
    // Y=180 corresponds to a medium-bright green in BT.601.
    Frame testFrame = generateTestFrameI420(kWidth, kHeight, 180, 100, 130);

    EncodedPacket packet;
    std::vector<RegionInfo> regions = {
        {Rect{0, 0, kWidth, kHeight}, ContentType::UNKNOWN}
    };

    ASSERT_TRUE(encoder->encode(testFrame, regions, packet))
        << "Encoding test frame failed";
    ASSERT_FALSE(packet.data.empty()) << "Encoded packet is empty";
    EXPECT_TRUE(packet.isKeyFrame) << "First frame should be a keyframe";

    LOG_INFO("Test: Encoded frame — %zu bytes, keyframe=%d",
             packet.data.size(), packet.isKeyFrame ? 1 : 0);

    // ---- Step 8: Set up viewer to receive video ----
    std::mutex muVideo;
    std::condition_variable cvVideo;
    std::atomic<bool> videoReceived{false};
    std::vector<uint8_t> receivedNalData;

    viewerSession.setOnVideo([&](const uint8_t* data, size_t size) {
        LOG_INFO("Test: Viewer received video data — %zu bytes", size);
        {
            std::lock_guard<std::mutex> lk(muVideo);
            receivedNalData.assign(data, data + size);
        }
        videoReceived.store(true);
        cvVideo.notify_one();
    });

    // ---- Step 9: Host sends encoded frame via WebRTC ----
    ASSERT_TRUE(hostSession.sendVideo(packet.data.data(), packet.data.size()))
        << "Failed to send video via WebRTC DataChannel";

    LOG_INFO("Test: Host sent %zu bytes of video data", packet.data.size());

    // ---- Step 10: Wait for viewer to receive ----
    ASSERT_TRUE(waitFor(muVideo, cvVideo, [&] { return videoReceived.load(); }, 10000))
        << "Viewer did not receive video data within 10 seconds";

    // ---- Step 11: Decode on viewer side ----
    std::vector<uint8_t> nalData;
    {
        std::lock_guard<std::mutex> lk(muVideo);
        nalData = receivedNalData;
    }
    ASSERT_FALSE(nalData.empty()) << "Received NAL data is empty";
    EXPECT_EQ(nalData.size(), packet.data.size())
        << "Received data size mismatch (sent vs received)";

    auto decoder = CodecFactory::createDecoder(CodecBackend::OpenH264);
    ASSERT_NE(decoder, nullptr) << "Failed to create OpenH264 decoder";
    ASSERT_TRUE(decoder->init(kWidth, kHeight))
        << "Decoder init failed";

    Frame decodedFrame;
    ASSERT_TRUE(decoder->decode(nalData.data(), nalData.size(), decodedFrame))
        << "Decoding received NAL data failed";

    // ---- Step 12: Verify decoded frame ----
    EXPECT_EQ(decodedFrame.width, kWidth);
    EXPECT_EQ(decodedFrame.height, kHeight);
    EXPECT_EQ(decodedFrame.format, PixelFormat::I420);
    EXPECT_FALSE(decodedFrame.data.empty());

    // Verify pixel data is non-zero and approximately correct.
    // H.264 compression will alter exact values, so we check within tolerance.
    const uint8_t* yPlane = decodedFrame.plane(0);
    const uint8_t* uPlane = decodedFrame.plane(1);
    const uint8_t* vPlane = decodedFrame.plane(2);

    ASSERT_NE(yPlane, nullptr);
    ASSERT_NE(uPlane, nullptr);
    ASSERT_NE(vPlane, nullptr);

    // Check Y plane: the input was 180 for all pixels.
    // After encode/decode, expect most pixels within +/-15 of 180.
    int yCorrect = 0;
    int yTotal = kWidth * kHeight;
    for (int i = 0; i < yTotal; ++i) {
        if (std::abs(static_cast<int>(yPlane[i]) - 180) <= 15) {
            ++yCorrect;
        }
    }
    float yAccuracy = static_cast<float>(yCorrect) / static_cast<float>(yTotal);
    EXPECT_GT(yAccuracy, 0.85f)
        << "Y plane accuracy too low: " << (yAccuracy * 100.0f)
        << "% of pixels within tolerance (expected >85%)";

    // Check U plane: input was 100.
    int uvWidth = kWidth / 2;
    int uvHeight = kHeight / 2;
    int uCorrect = 0;
    int uvTotal = uvWidth * uvHeight;
    for (int y = 0; y < uvHeight; ++y) {
        for (int x = 0; x < uvWidth; ++x) {
            int idx = y * (decodedFrame.stride / 2) + x;
            if (std::abs(static_cast<int>(uPlane[idx]) - 100) <= 15) {
                ++uCorrect;
            }
        }
    }
    float uAccuracy = static_cast<float>(uCorrect) / static_cast<float>(uvTotal);
    EXPECT_GT(uAccuracy, 0.85f)
        << "U plane accuracy too low: " << (uAccuracy * 100.0f) << "%";

    // Check V plane: input was 130.
    int vCorrect = 0;
    for (int y = 0; y < uvHeight; ++y) {
        for (int x = 0; x < uvWidth; ++x) {
            int idx = y * (decodedFrame.stride / 2) + x;
            if (std::abs(static_cast<int>(vPlane[idx]) - 130) <= 15) {
                ++vCorrect;
            }
        }
    }
    float vAccuracy = static_cast<float>(vCorrect) / static_cast<float>(uvTotal);
    EXPECT_GT(vAccuracy, 0.85f)
        << "V plane accuracy too low: " << (vAccuracy * 100.0f) << "%";

    LOG_INFO("Test: Decoded frame verified — %dx%d, Y=%.1f%% U=%.1f%% V=%.1f%%",
             decodedFrame.width, decodedFrame.height,
             yAccuracy * 100.0f, uAccuracy * 100.0f, vAccuracy * 100.0f);

    // ---- Step 13: Clean up ----
    hostSession.close();
    viewerSession.close();
}

// ---------------------------------------------------------------------------
// TEST: Multiple frames streamed through WebRTC
// ---------------------------------------------------------------------------

TEST_F(WebRtcE2ETest, MultiFrameStreaming) {
    // ---- Register and connect ----
    UserID hostId  = makeTestId("HST2");
    UserID viewerId = makeTestId("VWR2");

    ASSERT_TRUE(connectAndRegister(hostSignaling_, hostId));
    ASSERT_TRUE(connectAndRegister(viewerSignaling_, viewerId));

    WebRtcConfig rtcConfig;
    rtcConfig.stunServers = {"stun:stun.l.google.com:19302"};

    WebRtcSession hostSession(&hostSignaling_, viewerId, rtcConfig);
    WebRtcSession viewerSession(&viewerSignaling_, hostId, rtcConfig);

    std::atomic<bool> hostConnected{false};
    std::atomic<bool> viewerConnected{false};

    hostSession.setOnConnected([&]() { hostConnected.store(true); });
    viewerSession.setOnConnected([&]() { viewerConnected.store(true); });

    wireSignalingToWebRtc(hostSignaling_, hostSession, viewerSession);
    wireSignalingToWebRtc(viewerSignaling_, viewerSession, hostSession);

    // Signaling handshake
    std::atomic<bool> requestReceived{false};
    std::mutex muReq;
    std::condition_variable cvReq;

    viewerSignaling_.onConnectionRequest([&](const ConnectionRequest&) {
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    std::atomic<bool> accepted{false};
    std::mutex muAcc;
    std::condition_variable cvAcc;

    hostSignaling_.onConnectionAccepted([&](const ConnectionAcceptance&) {
        accepted.store(true);
        std::lock_guard<std::mutex> lk(muAcc);
        cvAcc.notify_one();
    });

    ASSERT_TRUE(hostSignaling_.requestConnection(viewerId));
    ASSERT_TRUE(waitFor(muReq, cvReq, [&] { return requestReceived.load(); }));
    ASSERT_TRUE(viewerSignaling_.acceptConnection(hostId));
    ASSERT_TRUE(waitFor(muAcc, cvAcc, [&] { return accepted.load(); }));

    ASSERT_TRUE(viewerSession.startAsViewer());
    ASSERT_TRUE(hostSession.startAsHost());

    ASSERT_TRUE(waitForAtomic(hostConnected, 15000));
    ASSERT_TRUE(waitForAtomic(viewerConnected, 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- Encode and send multiple frames ----
    constexpr int kWidth = 160;
    constexpr int kHeight = 120;
    constexpr int kNumFrames = 5;

    auto encoder = CodecFactory::createEncoder(CodecBackend::OpenH264);
    ASSERT_NE(encoder, nullptr);

    EncoderConfig encCfg;
    encCfg.width = kWidth;
    encCfg.height = kHeight;
    encCfg.targetBitrateBps = 300000;
    encCfg.maxBitrateBps = 600000;
    encCfg.maxFps = 30.0f;
    encCfg.temporalLayers = 1;

    ASSERT_TRUE(encoder->init(encCfg));

    // Track received frames on the viewer side.
    std::atomic<int> framesReceived{0};
    std::mutex muVideo;
    std::condition_variable cvVideo;

    viewerSession.setOnVideo([&](const uint8_t* /*data*/, size_t size) {
        if (size > 0) {
            framesReceived.fetch_add(1);
            std::lock_guard<std::mutex> lk(muVideo);
            cvVideo.notify_one();
        }
    });

    // Send frames with different Y values so each is distinct.
    for (int i = 0; i < kNumFrames; ++i) {
        uint8_t yVal = static_cast<uint8_t>(100 + i * 25);
        Frame frame = generateTestFrameI420(kWidth, kHeight,
                                            yVal, 128, 128,
                                            static_cast<uint64_t>(i + 1));

        EncodedPacket pkt;
        std::vector<RegionInfo> regions = {
            {Rect{0, 0, kWidth, kHeight}, ContentType::UNKNOWN}
        };

        ASSERT_TRUE(encoder->encode(frame, regions, pkt));

        // Skip frames that were skipped by the encoder (skip frames have empty data).
        if (pkt.data.empty()) continue;

        ASSERT_TRUE(hostSession.sendVideo(pkt.data.data(), pkt.data.size()))
            << "Failed to send frame " << i;

        // Small delay between frames to avoid overwhelming the DataChannel.
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    // Wait for at least some frames to arrive (not all may arrive if encoder
    // decides to skip some).
    {
        std::unique_lock<std::mutex> lk(muVideo);
        cvVideo.wait_for(lk, std::chrono::seconds(10),
                         [&] { return framesReceived.load() >= 1; });
    }

    int received = framesReceived.load();
    EXPECT_GE(received, 1)
        << "Expected at least 1 frame to arrive, got " << received;

    LOG_INFO("Test: MultiFrame — sent %d frames, viewer received %d",
             kNumFrames, received);

    hostSession.close();
    viewerSession.close();
}

// ---------------------------------------------------------------------------
// TEST: Codec-only encode/decode without network (sanity check)
// ---------------------------------------------------------------------------

TEST_F(WebRtcE2ETest, CodecRoundTrip_NoNetwork) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;

    // Create encoder
    auto encoder = CodecFactory::createEncoder(CodecBackend::OpenH264);
    ASSERT_NE(encoder, nullptr);

    EncoderConfig cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.targetBitrateBps = 500000;
    cfg.maxBitrateBps = 1000000;
    cfg.maxFps = 30.0f;
    cfg.temporalLayers = 1;

    ASSERT_TRUE(encoder->init(cfg));

    // Create decoder
    auto decoder = CodecFactory::createDecoder(CodecBackend::OpenH264);
    ASSERT_NE(decoder, nullptr);
    ASSERT_TRUE(decoder->init(kWidth, kHeight));

    // Encode a gradient frame for richer content.
    Frame input = generateGradientFrameI420(kWidth, kHeight);

    EncodedPacket packet;
    std::vector<RegionInfo> regions = {
        {Rect{0, 0, kWidth, kHeight}, ContentType::UNKNOWN}
    };
    ASSERT_TRUE(encoder->encode(input, regions, packet));
    ASSERT_FALSE(packet.data.empty());
    EXPECT_TRUE(packet.isKeyFrame);

    // Decode
    Frame output;
    ASSERT_TRUE(decoder->decode(packet.data.data(), packet.data.size(), output));

    EXPECT_EQ(output.width, kWidth);
    EXPECT_EQ(output.height, kHeight);
    EXPECT_EQ(output.format, PixelFormat::I420);
    EXPECT_FALSE(output.data.empty());

    // Verify non-zero content in the decoded frame.
    bool hasNonZero = false;
    for (size_t i = 0; i < output.data.size(); ++i) {
        if (output.data[i] != 0) { hasNonZero = true; break; }
    }
    EXPECT_TRUE(hasNonZero) << "Decoded frame is all zeros";

    LOG_INFO("Test: CodecRoundTrip — encode %zu bytes, decode %dx%d OK",
             packet.data.size(), output.width, output.height);
}

// ---------------------------------------------------------------------------
// TEST: WebRTC DataChannel bidirectional control messages
// ---------------------------------------------------------------------------

TEST_F(WebRtcE2ETest, BidirectionalDataChannel) {
    UserID hostId  = makeTestId("HST3");
    UserID viewerId = makeTestId("VWR3");

    ASSERT_TRUE(connectAndRegister(hostSignaling_, hostId));
    ASSERT_TRUE(connectAndRegister(viewerSignaling_, viewerId));

    WebRtcConfig rtcConfig;
    rtcConfig.stunServers = {"stun:stun.l.google.com:19302"};

    WebRtcSession hostSession(&hostSignaling_, viewerId, rtcConfig);
    WebRtcSession viewerSession(&viewerSignaling_, hostId, rtcConfig);

    std::atomic<bool> hostConnected{false};
    std::atomic<bool> viewerConnected{false};

    hostSession.setOnConnected([&]() { hostConnected.store(true); });
    viewerSession.setOnConnected([&]() { viewerConnected.store(true); });

    wireSignalingToWebRtc(hostSignaling_, hostSession, viewerSession);
    wireSignalingToWebRtc(viewerSignaling_, viewerSession, hostSession);

    // Handshake
    std::atomic<bool> requestReceived{false};
    std::mutex muReq;
    std::condition_variable cvReq;
    viewerSignaling_.onConnectionRequest([&](const ConnectionRequest&) {
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    std::atomic<bool> accepted{false};
    std::mutex muAcc;
    std::condition_variable cvAcc;
    hostSignaling_.onConnectionAccepted([&](const ConnectionAcceptance&) {
        accepted.store(true);
        std::lock_guard<std::mutex> lk(muAcc);
        cvAcc.notify_one();
    });

    ASSERT_TRUE(hostSignaling_.requestConnection(viewerId));
    ASSERT_TRUE(waitFor(muReq, cvReq, [&] { return requestReceived.load(); }));
    ASSERT_TRUE(viewerSignaling_.acceptConnection(hostId));
    ASSERT_TRUE(waitFor(muAcc, cvAcc, [&] { return accepted.load(); }));

    ASSERT_TRUE(viewerSession.startAsViewer());
    ASSERT_TRUE(hostSession.startAsHost());

    ASSERT_TRUE(waitForAtomic(hostConnected, 15000));
    ASSERT_TRUE(waitForAtomic(viewerConnected, 15000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // ---- Host sends control data to viewer ----
    std::mutex muData;
    std::condition_variable cvData;
    std::atomic<bool> viewerReceivedData{false};
    std::vector<uint8_t> viewerRecvBuf;

    viewerSession.setOnData([&](const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lk(muData);
        viewerRecvBuf.assign(data, data + size);
        viewerReceivedData.store(true);
        cvData.notify_one();
    });

    // Send a test control message (simulating an input event).
    std::vector<uint8_t> controlMsg = {0xDE, 0xAD, 0xBE, 0xEF,
                                       0x01, 0x02, 0x03, 0x04};
    ASSERT_TRUE(hostSession.sendData(controlMsg.data(), controlMsg.size()));

    ASSERT_TRUE(waitFor(muData, cvData,
                        [&] { return viewerReceivedData.load(); }, 5000))
        << "Viewer did not receive control data";

    {
        std::lock_guard<std::mutex> lk(muData);
        EXPECT_EQ(viewerRecvBuf, controlMsg)
            << "Control data mismatch";
    }

    LOG_INFO("Test: Bidirectional DataChannel OK");

    hostSession.close();
    viewerSession.close();
}

} // namespace
} // namespace omnidesk
