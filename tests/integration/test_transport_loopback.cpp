// Integration test: transport layer loopback.
// Creates a UdpChannel pair on localhost, packetizes a synthetic EncodedPacket,
// sends fragments, reassembles on the receiver, and verifies data integrity.
// Also tests FEC recovery after dropping a fragment.

#include <gtest/gtest.h>

#include "core/types.h"
#include "transport/tcp_channel.h"
#include "transport/udp_channel.h"
#include "transport/framer.h"
#include "transport/fec.h"
#include "transport/protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace omnidesk {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Create a synthetic EncodedPacket with a recognizable byte pattern.
EncodedPacket makeSyntheticPacket(uint64_t frameId, size_t dataSize) {
    EncodedPacket pkt;
    pkt.frameId = frameId;
    pkt.timestampUs = frameId * 33333;
    pkt.isKeyFrame = (frameId == 0);
    pkt.temporalLayer = 0;
    pkt.data.resize(dataSize);
    for (size_t i = 0; i < dataSize; ++i) {
        pkt.data[i] = static_cast<uint8_t>((frameId * 7 + i * 13) & 0xFF);
    }
    pkt.dirtyRects.push_back({0, 0, 1920, 1080});
    return pkt;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class TransportLoopbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        SocketInitializer::initialize();

        // Bind both channels to localhost with OS-assigned ports.
        ASSERT_TRUE(sender_.bind(0)) << "Failed to bind sender UDP channel";
        ASSERT_TRUE(receiver_.bind(0)) << "Failed to bind receiver UDP channel";

        senderPort_ = sender_.localPort();
        receiverPort_ = receiver_.localPort();
        ASSERT_NE(senderPort_, 0u);
        ASSERT_NE(receiverPort_, 0u);
    }

    void TearDown() override {
        sender_.close();
        receiver_.close();
    }

    PeerAddress receiverAddr() const {
        return PeerAddress{"127.0.0.1", receiverPort_};
    }

    PeerAddress senderAddr() const {
        return PeerAddress{"127.0.0.1", senderPort_};
    }

    UdpChannel sender_;
    UdpChannel receiver_;
    uint16_t senderPort_ = 0;
    uint16_t receiverPort_ = 0;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(TransportLoopbackTest, UdpChannel_SendReceive_BasicRoundtrip) {
    // Send a small buffer and verify it arrives intact.
    std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    ASSERT_TRUE(sender_.sendTo(receiverAddr(), payload));

    // Poll for data.
    bool ready = receiver_.pollRead(2000);
    ASSERT_TRUE(ready) << "Receiver did not get data within timeout";

    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    PeerAddress from;
    size_t received = receiver_.recvFrom(buf.data(), buf.size(), from);
    ASSERT_EQ(received, payload.size());
    buf.resize(received);
    EXPECT_EQ(buf, payload);
}

TEST_F(TransportLoopbackTest, Packetize_SmallPacket_SingleFragment) {
    // A small encoded packet should produce a single fragment.
    EncodedPacket pkt = makeSyntheticPacket(1, 100);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);  // No FEC for this test.
    auto fragments = packetizer.packetize(pkt, /*fecGroupId=*/0);

    ASSERT_GE(fragments.size(), 1u);
    // With 100 bytes of data and no FEC, expect exactly 1 fragment.
    EXPECT_EQ(fragments.size(), 1u);
    EXPECT_FALSE(fragments[0].isParity);
}

TEST_F(TransportLoopbackTest, Packetize_LargePacket_MultipleFragments) {
    // A large encoded packet should be split into multiple fragments.
    size_t largeSize = MAX_FRAGMENT_PAYLOAD * 5 + 100;  // ~5.x fragments
    EncodedPacket pkt = makeSyntheticPacket(2, largeSize);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);
    auto fragments = packetizer.packetize(pkt, /*fecGroupId=*/0);

    EXPECT_GE(fragments.size(), 6u);  // At least 6 fragments for 5+ payloads.
    for (const auto& frag : fragments) {
        EXPECT_FALSE(frag.isParity);
        EXPECT_LE(frag.data.size(), MAX_UDP_PAYLOAD);
    }
}

TEST_F(TransportLoopbackTest, PacketizeAndReassemble_NoLoss) {
    // Full pipeline: packetize, send over UDP, receive, reassemble.
    EncodedPacket original = makeSyntheticPacket(10, 5000);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);
    auto fragments = packetizer.packetize(original, /*fecGroupId=*/0);
    ASSERT_GT(fragments.size(), 0u);

    // Set up assembler with a callback.
    FrameAssembler assembler;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> frameReassembled{false};
    EncodedPacket reassembled;

    assembler.setFrameCallback([&](const EncodedPacket& pkt) {
        reassembled = pkt;
        frameReassembled.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    // Send all fragments from sender to receiver.
    for (const auto& frag : fragments) {
        ASSERT_TRUE(sender_.sendTo(receiverAddr(), frag.data));
    }

    // Receive all fragments and feed them to the assembler.
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    PeerAddress from;
    size_t fragsReceived = 0;

    for (size_t i = 0; i < fragments.size() + 5; ++i) {
        if (!receiver_.pollRead(1000)) break;
        size_t n = receiver_.recvFrom(buf.data(), buf.size(), from);
        if (n == 0) break;
        assembler.addFragment(buf.data(), n);
        ++fragsReceived;
        if (frameReassembled.load()) break;
    }

    ASSERT_EQ(fragsReceived, fragments.size())
        << "Did not receive all fragments";

    // Wait for reassembly.
    std::unique_lock<std::mutex> lock(mu);
    bool done = cv.wait_for(lock, std::chrono::milliseconds(2000),
                            [&] { return frameReassembled.load(); });
    ASSERT_TRUE(done) << "Frame was not reassembled in time";

    // Verify the reassembled data matches the original.
    EXPECT_EQ(reassembled.data.size(), original.data.size());
    EXPECT_EQ(reassembled.data, original.data);
    EXPECT_EQ(reassembled.frameId, original.frameId);
}

TEST_F(TransportLoopbackTest, PacketizeAndReassemble_WithFEC_NoLoss) {
    // Packetize with FEC enabled, send all fragments including parity,
    // and reassemble successfully.
    EncodedPacket original = makeSyntheticPacket(20, 8000);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.25f);  // 25% FEC overhead.
    auto fragments = packetizer.packetize(original, /*fecGroupId=*/1);
    ASSERT_GT(fragments.size(), 0u);

    // Count data vs parity fragments.
    size_t dataFrags = 0;
    size_t parityFrags = 0;
    for (const auto& f : fragments) {
        if (f.isParity) ++parityFrags;
        else ++dataFrags;
    }
    EXPECT_GT(dataFrags, 0u);
    // With 25% ratio and enough data, we should get some parity.
    // (For very small fragment counts, parity might round to 0.)

    // Send all fragments.
    FrameAssembler assembler;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> frameReassembled{false};
    EncodedPacket reassembled;

    assembler.setFrameCallback([&](const EncodedPacket& pkt) {
        reassembled = pkt;
        frameReassembled.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    for (const auto& frag : fragments) {
        ASSERT_TRUE(sender_.sendTo(receiverAddr(), frag.data));
    }

    // Receive and reassemble.
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    PeerAddress from;
    for (size_t i = 0; i < fragments.size() + 5; ++i) {
        if (!receiver_.pollRead(1000)) break;
        size_t n = receiver_.recvFrom(buf.data(), buf.size(), from);
        if (n == 0) break;
        assembler.addFragment(buf.data(), n);
        if (frameReassembled.load()) break;
    }

    std::unique_lock<std::mutex> lock(mu);
    bool done = cv.wait_for(lock, std::chrono::milliseconds(2000),
                            [&] { return frameReassembled.load(); });
    ASSERT_TRUE(done) << "Frame was not reassembled with FEC (no loss)";
    EXPECT_EQ(reassembled.data, original.data);
}

TEST_F(TransportLoopbackTest, FEC_DropOneFragment_RecoverSucceeds) {
    // Packetize with FEC, drop one data fragment, and verify the assembler
    // can still recover the complete frame using parity.
    EncodedPacket original = makeSyntheticPacket(30, 10000);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.5f);  // 50% FEC for strong recovery.
    auto fragments = packetizer.packetize(original, /*fecGroupId=*/2);
    ASSERT_GT(fragments.size(), 2u);

    // Identify the first non-parity (data) fragment to drop.
    int dropIndex = -1;
    for (size_t i = 0; i < fragments.size(); ++i) {
        if (!fragments[i].isParity) {
            dropIndex = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(dropIndex, 0) << "No data fragment to drop";

    FrameAssembler assembler;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> frameReassembled{false};
    EncodedPacket reassembled;

    assembler.setFrameCallback([&](const EncodedPacket& pkt) {
        reassembled = pkt;
        frameReassembled.store(true);
        std::lock_guard<std::mutex> lk(mu);
        cv.notify_one();
    });

    // Send all fragments EXCEPT the dropped one.
    for (size_t i = 0; i < fragments.size(); ++i) {
        if (static_cast<int>(i) == dropIndex) continue;  // Skip this fragment.
        ASSERT_TRUE(sender_.sendTo(receiverAddr(), fragments[i].data));
    }

    // Receive and feed to assembler.
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    PeerAddress from;
    size_t expectedRecv = fragments.size() - 1;
    for (size_t i = 0; i < expectedRecv + 5; ++i) {
        if (!receiver_.pollRead(1000)) break;
        size_t n = receiver_.recvFrom(buf.data(), buf.size(), from);
        if (n == 0) break;
        assembler.addFragment(buf.data(), n);
        if (frameReassembled.load()) break;
    }

    // The assembler should have used FEC to recover the missing fragment.
    std::unique_lock<std::mutex> lock(mu);
    bool done = cv.wait_for(lock, std::chrono::milliseconds(3000),
                            [&] { return frameReassembled.load(); });
    ASSERT_TRUE(done) << "FEC recovery failed: frame not reassembled after dropping one fragment";
    EXPECT_EQ(reassembled.data, original.data);
}

TEST_F(TransportLoopbackTest, SelectiveAck_TracksFragments) {
    // Test the SelectiveAck bitmap tracking mechanism.
    SelectiveAck sack;
    sack.frameId = 42;
    sack.fragCount = 16;
    sack.bitmap.resize((sack.fragCount + 7) / 8, 0);

    // Mark specific fragments as received.
    sack.setReceived(0);
    sack.setReceived(3);
    sack.setReceived(7);
    sack.setReceived(15);

    EXPECT_TRUE(sack.isReceived(0));
    EXPECT_FALSE(sack.isReceived(1));
    EXPECT_FALSE(sack.isReceived(2));
    EXPECT_TRUE(sack.isReceived(3));
    EXPECT_TRUE(sack.isReceived(7));
    EXPECT_TRUE(sack.isReceived(15));
    EXPECT_FALSE(sack.isReceived(8));

    // Serialize and deserialize.
    auto serialized = sack.serialize();
    auto deserialized = SelectiveAck::deserialize(serialized.data(), serialized.size());
    EXPECT_EQ(deserialized.frameId, sack.frameId);
    EXPECT_EQ(deserialized.fragCount, sack.fragCount);
    EXPECT_TRUE(deserialized.isReceived(0));
    EXPECT_TRUE(deserialized.isReceived(3));
    EXPECT_TRUE(deserialized.isReceived(7));
    EXPECT_TRUE(deserialized.isReceived(15));
    EXPECT_FALSE(deserialized.isReceived(1));
}

TEST_F(TransportLoopbackTest, MultipleFrames_InterleavedSendReceive) {
    // Send two frames back-to-back and verify both are reassembled correctly.
    EncodedPacket original1 = makeSyntheticPacket(100, 3000);
    EncodedPacket original2 = makeSyntheticPacket(101, 4000);

    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);
    auto frags1 = packetizer.packetize(original1, /*fecGroupId=*/0);
    auto frags2 = packetizer.packetize(original2, /*fecGroupId=*/0);

    FrameAssembler assembler;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<EncodedPacket> reassembledFrames;

    assembler.setFrameCallback([&](const EncodedPacket& pkt) {
        std::lock_guard<std::mutex> lk(mu);
        reassembledFrames.push_back(pkt);
        cv.notify_one();
    });

    // Send all fragments from both frames.
    for (const auto& frag : frags1) {
        ASSERT_TRUE(sender_.sendTo(receiverAddr(), frag.data));
    }
    for (const auto& frag : frags2) {
        ASSERT_TRUE(sender_.sendTo(receiverAddr(), frag.data));
    }

    // Receive all.
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    PeerAddress from;
    size_t totalFrags = frags1.size() + frags2.size();
    for (size_t i = 0; i < totalFrags + 5; ++i) {
        if (!receiver_.pollRead(1000)) break;
        size_t n = receiver_.recvFrom(buf.data(), buf.size(), from);
        if (n == 0) break;
        assembler.addFragment(buf.data(), n);

        std::lock_guard<std::mutex> lk(mu);
        if (reassembledFrames.size() >= 2) break;
    }

    // Wait for both frames.
    std::unique_lock<std::mutex> lock(mu);
    bool done = cv.wait_for(lock, std::chrono::milliseconds(3000),
                            [&] { return reassembledFrames.size() >= 2; });
    ASSERT_TRUE(done) << "Did not reassemble both frames; got "
                      << reassembledFrames.size();

    // Sort by frameId for deterministic comparison.
    std::sort(reassembledFrames.begin(), reassembledFrames.end(),
              [](const EncodedPacket& a, const EncodedPacket& b) {
                  return a.frameId < b.frameId;
              });

    EXPECT_EQ(reassembledFrames[0].data, original1.data);
    EXPECT_EQ(reassembledFrames[1].data, original2.data);
}

TEST_F(TransportLoopbackTest, VideoHeader_SerializeDeserialize) {
    // Verify VideoHeader round-trip serialization.
    VideoHeader hdr;
    hdr.frameId = 12345;
    hdr.fragId = 7;
    hdr.fragCount = 20;
    hdr.fecGroup = 3;
    hdr.flags = FLAG_KEYFRAME | FLAG_HAS_DIRTY_RECTS;
    hdr.rectCount = 2;

    uint8_t buf[VideoHeader::SIZE];
    hdr.serialize(buf);

    VideoHeader parsed = VideoHeader::deserialize(buf);
    EXPECT_EQ(parsed.frameId, hdr.frameId);
    EXPECT_EQ(parsed.fragId, hdr.fragId);
    EXPECT_EQ(parsed.fragCount, hdr.fragCount);
    EXPECT_EQ(parsed.fecGroup, hdr.fecGroup);
    EXPECT_EQ(parsed.flags, hdr.flags);
    EXPECT_EQ(parsed.rectCount, hdr.rectCount);
}

} // namespace
} // namespace omnidesk
