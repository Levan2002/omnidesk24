// Integration test: signaling server connection flow.
// Starts a local SignalingServer, connects two SignalingClients,
// and verifies registration, connect-request, accept, and reject flows.

#include <gtest/gtest.h>

#include "core/types.h"
#include "signaling/signaling_server.h"
#include "signaling/signaling_client.h"
#include "signaling/user_id.h"
#include "signaling/tcp_channel.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace omnidesk {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Wait for a condition variable with a timeout. Returns true if notified.
template <typename Pred>
bool waitFor(std::mutex& mu, std::condition_variable& cv, Pred pred,
             int timeoutMs = 5000) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred);
}

// Generate a deterministic 8-char UserID for testing.
UserID makeTestUserId(const char* suffix) {
    std::string id = "TEST";
    id += suffix;
    while (id.size() < 8) id += 'X';
    id.resize(8);
    return UserID{id};
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SignalingFlowTest : public ::testing::Test {
protected:
    static constexpr uint16_t kServerPort = 0;  // OS-assigned

    void SetUp() override {
        SocketInitializer::initialize();

        server_ = std::make_unique<SignalingServer>();
        ASSERT_TRUE(server_->start(kServerPort))
            << "Failed to start signaling server";

        serverPort_ = server_->port();
        ASSERT_NE(serverPort_, 0u);
    }

    void TearDown() override {
        clientA_.disconnect();
        clientB_.disconnect();
        server_->stop();
    }

    // Connect a client to the test server.
    bool connectClient(SignalingClient& client) {
        return client.connect("127.0.0.1", serverPort_);
    }

    // Register a client and wait for confirmation.
    bool registerClient(SignalingClient& client, const UserID& userId) {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<bool> registered{false};
        std::atomic<bool> success{false};

        client.onRegistered([&](bool ok) {
            success.store(ok);
            registered.store(true);
            std::lock_guard<std::mutex> lk(mu);
            cv.notify_one();
        });

        if (!client.registerUser(userId)) return false;

        bool got = waitFor(mu, cv, [&] { return registered.load(); });
        return got && success.load();
    }

    std::unique_ptr<SignalingServer> server_;
    uint16_t serverPort_ = 0;
    SignalingClient clientA_;
    SignalingClient clientB_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(SignalingFlowTest, ServerStartsAndStops) {
    EXPECT_TRUE(server_->isRunning());
    EXPECT_EQ(server_->userCount(), 0u);
}

TEST_F(SignalingFlowTest, ClientConnectsAndRegisters) {
    UserID idA = makeTestUserId("AAA1");

    clientA_.setAutoReconnect(false);
    ASSERT_TRUE(connectClient(clientA_));
    EXPECT_TRUE(clientA_.isConnected());

    ASSERT_TRUE(registerClient(clientA_, idA));
    EXPECT_TRUE(clientA_.isRegistered());
    EXPECT_EQ(clientA_.userId(), idA);

    // Server should see one registered user.
    // Give the server a moment to process.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(server_->isUserRegistered(idA));
    EXPECT_EQ(server_->userCount(), 1u);
}

TEST_F(SignalingFlowTest, TwoClientsRegister) {
    UserID idA = makeTestUserId("CLT1");
    UserID idB = makeTestUserId("CLT2");

    clientA_.setAutoReconnect(false);
    clientB_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(connectClient(clientB_));

    ASSERT_TRUE(registerClient(clientA_, idA));
    ASSERT_TRUE(registerClient(clientB_, idB));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(server_->isUserRegistered(idA));
    EXPECT_TRUE(server_->isUserRegistered(idB));
    EXPECT_EQ(server_->userCount(), 2u);
}

TEST_F(SignalingFlowTest, ConnectionRequest_ReceivedByTarget) {
    UserID idA = makeTestUserId("REQ1");
    UserID idB = makeTestUserId("REQ2");

    clientA_.setAutoReconnect(false);
    clientB_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(connectClient(clientB_));
    ASSERT_TRUE(registerClient(clientA_, idA));
    ASSERT_TRUE(registerClient(clientB_, idB));

    // Set up callback on Client B to receive the connection request.
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> requestReceived{false};
    ConnectionRequest receivedReq;

    clientB_.onConnectionRequest([&](const ConnectionRequest& req) {
        receivedReq = req;
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    // Client A requests connection to Client B.
    ASSERT_TRUE(clientA_.requestConnection(idB));

    // Wait for Client B to receive the request.
    bool got = waitFor(mu, cv, [&] { return requestReceived.load(); });
    ASSERT_TRUE(got) << "Client B did not receive connection request in time";
    EXPECT_EQ(receivedReq.fromId, idA);
}

TEST_F(SignalingFlowTest, ConnectionAccept_ReceivedByRequester) {
    UserID idA = makeTestUserId("ACC1");
    UserID idB = makeTestUserId("ACC2");

    clientA_.setAutoReconnect(false);
    clientB_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(connectClient(clientB_));
    ASSERT_TRUE(registerClient(clientA_, idA));
    ASSERT_TRUE(registerClient(clientB_, idB));

    // Client B will accept the incoming request.
    std::mutex muReq;
    std::condition_variable cvReq;
    std::atomic<bool> requestReceived{false};

    clientB_.onConnectionRequest([&](const ConnectionRequest& req) {
        // Accept the connection from A.
        clientB_.acceptConnection(req.fromId);
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    // Client A waits for acceptance.
    std::mutex muAcc;
    std::condition_variable cvAcc;
    std::atomic<bool> accepted{false};
    ConnectionAcceptance receivedAcc;

    clientA_.onConnectionAccepted([&](const ConnectionAcceptance& acc) {
        receivedAcc = acc;
        accepted.store(true);
        std::lock_guard<std::mutex> lk(muAcc);
        cvAcc.notify_one();
    });

    // Client A requests connection to Client B.
    ASSERT_TRUE(clientA_.requestConnection(idB));

    // Wait for B to receive the request.
    bool gotReq = waitFor(muReq, cvReq, [&] { return requestReceived.load(); });
    ASSERT_TRUE(gotReq) << "Client B did not receive connection request";

    // Wait for A to receive acceptance.
    bool gotAcc = waitFor(muAcc, cvAcc, [&] { return accepted.load(); });
    ASSERT_TRUE(gotAcc) << "Client A did not receive connection acceptance";
    EXPECT_EQ(receivedAcc.fromId, idB);
}

TEST_F(SignalingFlowTest, ConnectionReject_ReceivedByRequester) {
    UserID idA = makeTestUserId("REJ1");
    UserID idB = makeTestUserId("REJ2");

    clientA_.setAutoReconnect(false);
    clientB_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(connectClient(clientB_));
    ASSERT_TRUE(registerClient(clientA_, idA));
    ASSERT_TRUE(registerClient(clientB_, idB));

    // Client B will reject the incoming request.
    std::mutex muReq;
    std::condition_variable cvReq;
    std::atomic<bool> requestReceived{false};

    clientB_.onConnectionRequest([&](const ConnectionRequest& req) {
        clientB_.rejectConnection(req.fromId, "busy");
        requestReceived.store(true);
        std::lock_guard<std::mutex> lk(muReq);
        cvReq.notify_one();
    });

    // Client A waits for rejection.
    std::mutex muRej;
    std::condition_variable cvRej;
    std::atomic<bool> rejected{false};
    ConnectionRejection receivedRej;

    clientA_.onConnectionRejected([&](const ConnectionRejection& rej) {
        receivedRej = rej;
        rejected.store(true);
        std::lock_guard<std::mutex> lk(muRej);
        cvRej.notify_one();
    });

    // Client A requests connection to Client B.
    ASSERT_TRUE(clientA_.requestConnection(idB));

    // Wait for B to receive the request.
    bool gotReq = waitFor(muReq, cvReq, [&] { return requestReceived.load(); });
    ASSERT_TRUE(gotReq) << "Client B did not receive connection request";

    // Wait for A to receive rejection.
    bool gotRej = waitFor(muRej, cvRej, [&] { return rejected.load(); });
    ASSERT_TRUE(gotRej) << "Client A did not receive connection rejection";
    EXPECT_EQ(receivedRej.fromId, idB);
    EXPECT_EQ(receivedRej.reason, "busy");
}

TEST_F(SignalingFlowTest, ConnectToOfflineUser_ReturnsOffline) {
    UserID idA = makeTestUserId("OFF1");
    UserID idOffline = makeTestUserId("OFF2");

    clientA_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(registerClient(clientA_, idA));

    // Set up offline callback.
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> offlineReceived{false};
    UserID offlineTarget;

    clientA_.onUserOffline([&](const UserID& targetId) {
        offlineTarget = targetId;
        offlineReceived.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    // Request connection to a user that is not registered.
    clientA_.requestConnection(idOffline);

    // The server should respond with a USER_OFFLINE message.
    bool got = waitFor(mu, cv, [&] { return offlineReceived.load(); });
    ASSERT_TRUE(got) << "Did not receive user_offline notification";
    EXPECT_EQ(offlineTarget, idOffline);
}

TEST_F(SignalingFlowTest, ClientDisconnect_ServerRemovesUser) {
    UserID idA = makeTestUserId("DIS1");

    clientA_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(registerClient(clientA_, idA));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(server_->isUserRegistered(idA));

    clientA_.disconnect();

    // Give the server time to detect the disconnect and clean up.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(server_->isUserRegistered(idA));
    EXPECT_EQ(server_->userCount(), 0u);
}

TEST_F(SignalingFlowTest, FullFlow_RequestAcceptBothSidesNotified) {
    // End-to-end test: two clients connect, register, one requests, the other
    // accepts, and both sides receive proper notifications.
    UserID idA = makeTestUserId("FUL1");
    UserID idB = makeTestUserId("FUL2");

    clientA_.setAutoReconnect(false);
    clientB_.setAutoReconnect(false);

    ASSERT_TRUE(connectClient(clientA_));
    ASSERT_TRUE(connectClient(clientB_));
    ASSERT_TRUE(registerClient(clientA_, idA));
    ASSERT_TRUE(registerClient(clientB_, idB));

    // Track all events.
    std::mutex mu;
    std::condition_variable cv;

    std::atomic<bool> bGotRequest{false};
    std::atomic<bool> aGotAcceptance{false};
    ConnectionRequest bReq;
    ConnectionAcceptance aAcc;

    clientB_.onConnectionRequest([&](const ConnectionRequest& req) {
        bReq = req;
        bGotRequest.store(true);
        // Accept immediately.
        clientB_.acceptConnection(req.fromId);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    clientA_.onConnectionAccepted([&](const ConnectionAcceptance& acc) {
        aAcc = acc;
        aGotAcceptance.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    // Client A initiates.
    ASSERT_TRUE(clientA_.requestConnection(idB));

    // Wait for both sides to be notified.
    bool allDone = waitFor(mu, cv, [&] {
        return bGotRequest.load() && aGotAcceptance.load();
    }, 10000);
    ASSERT_TRUE(allDone)
        << "Full flow did not complete: bGotRequest=" << bGotRequest.load()
        << " aGotAcceptance=" << aGotAcceptance.load();

    EXPECT_EQ(bReq.fromId, idA);
    EXPECT_EQ(aAcc.fromId, idB);
}

} // namespace
} // namespace omnidesk
