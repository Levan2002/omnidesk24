// Integration test: full signaling + data channel flow.
// Starts a local SignalingServer, connects two SignalingClients,
// performs the connect/accept handshake, then verifies that a
// TCP data channel can be established and data transferred.

#include <gtest/gtest.h>

#include "core/types.h"
#include "signaling/signaling_server.h"
#include "signaling/signaling_client.h"
#include "signaling/tcp_channel.h"
#include "signaling/wire_format.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omnidesk {
namespace {

static constexpr uint16_t DATA_PORT_BASE = 19800; // Avoid collisions

template <typename Pred>
bool waitFor(std::mutex& mu, std::condition_variable& cv, Pred pred,
             int timeoutMs = 5000) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred);
}

UserID makeId(const char* suffix) {
    std::string id = "TEST";
    id += suffix;
    while (id.size() < 8) id += 'X';
    id.resize(8);
    return UserID{id};
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class DataChannelFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        SocketInitializer::initialize();

        server_ = std::make_unique<SignalingServer>();
        ASSERT_TRUE(server_->start(0)); // OS-assigned port
        serverPort_ = server_->port();
        ASSERT_NE(serverPort_, 0u);
    }

    void TearDown() override {
        hostClient_.disconnect();
        viewerClient_.disconnect();
        server_->stop();
    }

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
        localAddr.port = dataPort_;
        if (!client.registerUser(id, localAddr)) return false;

        return waitFor(mu, cv, [&] { return done.load(); }) && ok.load();
    }

    std::unique_ptr<SignalingServer> server_;
    uint16_t serverPort_ = 0;
    uint16_t dataPort_ = DATA_PORT_BASE;
    SignalingClient hostClient_;
    SignalingClient viewerClient_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Full flow: signaling handshake → data channel → send/receive a packet.
TEST_F(DataChannelFlowTest, FullConnectionAndDataTransfer) {
    UserID hostId  = makeId("HST1");
    UserID viewerId = makeId("VWR1");

    ASSERT_TRUE(connectAndRegister(hostClient_, hostId));
    ASSERT_TRUE(connectAndRegister(viewerClient_, viewerId));

    // --- Step 1: Viewer requests connection to host ---
    std::mutex muReq;
    std::condition_variable cvReq;
    std::atomic<bool> requestReceived{false};

    hostClient_.onConnectionRequest([&](const ConnectionRequest& req) {
        EXPECT_EQ(req.fromId, viewerId);
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    ASSERT_TRUE(viewerClient_.requestConnection(hostId));

    ASSERT_TRUE(waitFor(muReq, cvReq, [&] { return requestReceived.load(); }))
        << "Host did not receive connection request";

    // --- Step 2: Host starts data listener, then accepts ---
    TcpChannel dataListener;
    ASSERT_TRUE(dataListener.listen(dataPort_));

    std::unique_ptr<TcpChannel> hostDataChannel;
    std::mutex dataChMu;
    std::atomic<bool> viewerConnected{false};

    // Accept in background thread (like App::dataAcceptThread_)
    std::thread acceptThread([&]() {
        for (int i = 0; i < 100; ++i) {
            if (dataListener.pollRead(100)) {
                auto ch = dataListener.accept();
                if (ch) {
                    std::lock_guard<std::mutex> lk(dataChMu);
                    hostDataChannel = std::move(ch);
                    viewerConnected.store(true);
                    return;
                }
            }
        }
    });

    // --- Step 3: Host accepts via signaling ---
    std::mutex muAcc;
    std::condition_variable cvAcc;
    std::atomic<bool> accepted{false};
    ConnectionAcceptance receivedAcc;

    viewerClient_.onConnectionAccepted([&](const ConnectionAcceptance& acc) {
        receivedAcc = acc;
        accepted.store(true);
        std::lock_guard<std::mutex> lk(muAcc);
        cvAcc.notify_one();
    });

    hostClient_.acceptConnection(viewerId);

    ASSERT_TRUE(waitFor(muAcc, cvAcc, [&] { return accepted.load(); }))
        << "Viewer did not receive connection acceptance";

    // --- Step 4: Viewer connects to host's data port ---
    // Use the local address from the acceptance (should be 127.0.0.1 in tests)
    PeerAddress hostAddr = receivedAcc.peerLocalAddr.valid()
        ? receivedAcc.peerLocalAddr : receivedAcc.peerPublicAddr;
    // In test, force to localhost
    hostAddr.host = "127.0.0.1";
    hostAddr.port = dataPort_;

    TcpChannel viewerDataChannel;
    ASSERT_TRUE(viewerDataChannel.connect(hostAddr.host, hostAddr.port, 5000))
        << "Viewer failed to connect to host data channel at "
        << hostAddr.host << ":" << hostAddr.port;

    // Wait for host to accept the data connection
    acceptThread.join();
    ASSERT_TRUE(viewerConnected.load())
        << "Host did not accept viewer on data channel";

    // --- Step 5: Host sends a video packet, viewer receives it ---
    // Simulate an EncodedPacket serialized as a VIDEO_DATA message.
    std::vector<uint8_t> testPayload(256);
    for (size_t i = 0; i < testPayload.size(); ++i)
        testPayload[i] = static_cast<uint8_t>(i & 0xFF);

    {
        std::lock_guard<std::mutex> lk(dataChMu);
        ASSERT_TRUE(hostDataChannel != nullptr);
        auto sendResult = hostDataChannel->send(MessageType::VIDEO_DATA, testPayload);
        EXPECT_EQ(sendResult, SocketResult::OK);
    }

    // Viewer receives
    ASSERT_TRUE(viewerDataChannel.pollRead(3000))
        << "No data available on viewer channel within 3 seconds";

    MessageType recvType;
    std::vector<uint8_t> recvPayload;
    auto recvResult = viewerDataChannel.recv(recvType, recvPayload);
    EXPECT_EQ(recvResult, SocketResult::OK);
    EXPECT_EQ(recvType, MessageType::VIDEO_DATA);
    EXPECT_EQ(recvPayload.size(), testPayload.size());
    EXPECT_EQ(recvPayload, testPayload);

    // --- Step 6: Clean up ---
    viewerDataChannel.close();
    {
        std::lock_guard<std::mutex> lk(dataChMu);
        if (hostDataChannel) hostDataChannel->close();
    }
    dataListener.close();
}

// Verify that the data channel mutex prevents crashes when send races
// with channel teardown.
TEST_F(DataChannelFlowTest, ConcurrentSendAndClose_NoCrash) {
    // Set up a simple TCP pair
    TcpChannel listener;
    ASSERT_TRUE(listener.listen(dataPort_ + 1));

    TcpChannel client;
    ASSERT_TRUE(client.connect("127.0.0.1", dataPort_ + 1, 3000));

    ASSERT_TRUE(listener.pollRead(3000));
    auto server = listener.accept();
    ASSERT_TRUE(server != nullptr);

    // Shared state protected by mutex (like App::dataChannelMutex_)
    std::mutex chMutex;
    std::atomic<bool> running{true};

    // Sender thread: rapid-fire sends
    std::thread sender([&]() {
        std::vector<uint8_t> data(128, 0xAB);
        while (running.load()) {
            std::lock_guard<std::mutex> lock(chMutex);
            if (!server || !server->isOpen()) break;
            server->send(MessageType::VIDEO_DATA, data);
        }
    });

    // Let it run briefly then close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false);

    {
        std::lock_guard<std::mutex> lock(chMutex);
        server->close();
    }

    sender.join();
    client.close();
    listener.close();
    // If we get here without crashing, the test passes.
}

// Verify local address is available after connect.
TEST_F(DataChannelFlowTest, SignalingClient_LocalAddress) {
    SignalingClient client;
    client.setAutoReconnect(false);
    ASSERT_TRUE(client.connect("127.0.0.1", serverPort_));

    std::string localAddr = client.localAddress();
    EXPECT_FALSE(localAddr.empty());
    EXPECT_NE(localAddr, "0.0.0.0");
    // When connecting to localhost, should be 127.0.0.1
    EXPECT_EQ(localAddr, "127.0.0.1");

    client.disconnect();
}

} // namespace
} // namespace omnidesk
