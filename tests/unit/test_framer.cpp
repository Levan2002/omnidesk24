#include <gtest/gtest.h>

#include "transport/framer.h"
#include "transport/protocol.h"
#include "core/types.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace omnidesk {

// Helper to create an EncodedPacket with a given data size.
static EncodedPacket makeEncodedPacket(size_t dataSize, uint64_t frameId = 1) {
    EncodedPacket pkt;
    pkt.frameId = frameId;
    pkt.timestampUs = 1000;
    pkt.isKeyFrame = false;
    pkt.temporalLayer = 0;
    pkt.data.resize(dataSize);
    for (size_t i = 0; i < dataSize; ++i) {
        pkt.data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return pkt;
}

TEST(FramePacketizer, SmallPacketFitsInOneFragment) {
    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);  // No FEC for this test

    // Create a packet smaller than MAX_FRAGMENT_PAYLOAD
    EncodedPacket pkt = makeEncodedPacket(100);
    auto fragments = packetizer.packetize(pkt, /*fecGroupId=*/0);

    // Should produce exactly one data fragment
    ASSERT_GE(fragments.size(), 1u);

    // The first fragment should contain a VideoHeader + payload
    EXPECT_GE(fragments[0].data.size(), VideoHeader::SIZE + 1u);
}

TEST(FramePacketizer, LargePacketMultipleFragments) {
    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);  // No FEC for this test

    // Create a packet larger than MAX_FRAGMENT_PAYLOAD to force multiple fragments
    size_t largeSize = MAX_FRAGMENT_PAYLOAD * 3 + 100;
    EncodedPacket pkt = makeEncodedPacket(largeSize);
    auto fragments = packetizer.packetize(pkt, /*fecGroupId=*/0);

    // Should produce at least 4 fragments
    EXPECT_GE(fragments.size(), 4u);

    // Each fragment should have data (VideoHeader + some payload)
    for (const auto& frag : fragments) {
        EXPECT_GE(frag.data.size(), VideoHeader::SIZE);
    }
}

TEST(FrameAssembler, ReassembleAllFragments_CompleteFrame) {
    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);

    EncodedPacket original = makeEncodedPacket(2000, /*frameId=*/42);
    auto fragments = packetizer.packetize(original, /*fecGroupId=*/0);
    ASSERT_GT(fragments.size(), 0u);

    FrameAssembler assembler;
    EncodedPacket reassembled;
    bool gotFrame = false;

    assembler.setFrameCallback([&](const EncodedPacket& packet) {
        reassembled = packet;
        gotFrame = true;
    });

    // Feed all fragments to the assembler
    for (const auto& frag : fragments) {
        assembler.addFragment(frag.data.data(), frag.data.size());
    }

    EXPECT_TRUE(gotFrame);
    EXPECT_EQ(reassembled.data.size(), original.data.size());
    EXPECT_EQ(reassembled.data, original.data);
}

TEST(FrameAssembler, MissingFragment_IncompleteFrame) {
    FramePacketizer packetizer;
    packetizer.setFecRatio(0.0f);

    // Use a large packet to ensure multiple fragments
    EncodedPacket original = makeEncodedPacket(MAX_FRAGMENT_PAYLOAD * 3, /*frameId=*/7);
    auto fragments = packetizer.packetize(original, /*fecGroupId=*/0);
    ASSERT_GT(fragments.size(), 2u);

    FrameAssembler assembler;
    bool gotFrame = false;

    assembler.setFrameCallback([&](const EncodedPacket&) {
        gotFrame = true;
    });

    // Feed all fragments EXCEPT the second one
    for (size_t i = 0; i < fragments.size(); ++i) {
        if (i == 1) continue;  // Skip fragment 1
        assembler.addFragment(fragments[i].data.data(), fragments[i].data.size());
    }

    // Frame should not be complete
    EXPECT_FALSE(gotFrame);

    // Verify frame is reported as incomplete
    VideoHeader vh = VideoHeader::deserialize(fragments[0].data.data());
    EXPECT_FALSE(assembler.isFrameComplete(vh.frameId));

    auto missing = assembler.getMissingFragments(vh.frameId);
    EXPECT_FALSE(missing.empty());
}

} // namespace omnidesk
