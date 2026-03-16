#include <gtest/gtest.h>

#include "transport/fec.h"

#include <cstdint>
#include <vector>

namespace omnidesk {

// Helper: create K data packets with known content.
static std::vector<FecPacket> makeDataPackets(int k, size_t packetLen = 100) {
    std::vector<FecPacket> packets(k);
    for (int i = 0; i < k; ++i) {
        packets[i].index = static_cast<uint16_t>(i);
        packets[i].groupId = 1;
        packets[i].isParity = false;
        packets[i].data.resize(packetLen);
        // Fill with a recognizable pattern
        for (size_t j = 0; j < packetLen; ++j) {
            packets[i].data[j] = static_cast<uint8_t>((i * 37 + j) & 0xFF);
        }
    }
    return packets;
}

TEST(FecEncoder, EncodeProducesParityPackets) {
    FecEncoder encoder;
    encoder.setProtectionRatio(0.5f);  // 50% → K data should yield ~K/2 parity

    const int K = 5;
    auto dataPackets = makeDataPackets(K);
    auto parityPackets = encoder.encode(dataPackets, /*groupId=*/1);

    // Should produce at least 1 parity packet
    EXPECT_GE(parityPackets.size(), 1u);

    // All parity packets should be flagged as parity
    for (const auto& p : parityPackets) {
        EXPECT_TRUE(p.isParity);
    }
}

TEST(FecDecoder, AllPacketsPresent_DecodeSucceeds) {
    FecEncoder encoder;
    encoder.setProtectionRatio(0.2f);

    const int K = 5;
    auto dataPackets = makeDataPackets(K);
    auto parityPackets = encoder.encode(dataPackets, /*groupId=*/1);

    FecDecoder decoder;
    decoder.setExpectedCount(K, static_cast<uint16_t>(parityPackets.size()));

    // Add all data packets
    for (const auto& p : dataPackets) {
        decoder.addPacket(p);
    }
    // Add all parity packets
    for (const auto& p : parityPackets) {
        decoder.addPacket(p);
    }

    EXPECT_TRUE(decoder.isComplete());

    auto recovered = decoder.getDataPackets();
    ASSERT_EQ(recovered.size(), static_cast<size_t>(K));

    // Verify data integrity
    for (int i = 0; i < K; ++i) {
        EXPECT_EQ(recovered[i].data, dataPackets[i].data);
    }
}

TEST(FecDecoder, OnePacketMissing_RecoverFromParity) {
    FecEncoder encoder;
    encoder.setProtectionRatio(0.5f);  // Enough parity to recover one loss

    const int K = 4;
    auto dataPackets = makeDataPackets(K);
    auto parityPackets = encoder.encode(dataPackets, /*groupId=*/1);

    // Need at least 1 parity packet for recovery
    ASSERT_GE(parityPackets.size(), 1u);

    FecDecoder decoder;
    decoder.setExpectedCount(K, static_cast<uint16_t>(parityPackets.size()));

    // Add all data packets EXCEPT packet index 0
    for (int i = 1; i < K; ++i) {
        decoder.addPacket(dataPackets[i]);
    }
    // Add all parity packets
    for (const auto& p : parityPackets) {
        decoder.addPacket(p);
    }

    bool recovered = decoder.tryRecover();
    EXPECT_TRUE(recovered);
    EXPECT_TRUE(decoder.isComplete());

    auto result = decoder.getDataPackets();
    ASSERT_EQ(result.size(), static_cast<size_t>(K));
    // Verify recovered packet matches original
    EXPECT_EQ(result[0].data, dataPackets[0].data);
}

TEST(FecDecoder, TooManyMissing_DecodeFails) {
    FecEncoder encoder;
    encoder.setProtectionRatio(0.1f);  // Very low parity

    const int K = 10;
    auto dataPackets = makeDataPackets(K);
    auto parityPackets = encoder.encode(dataPackets, /*groupId=*/1);

    FecDecoder decoder;
    decoder.setExpectedCount(K, static_cast<uint16_t>(parityPackets.size()));

    // Only add half of the data packets (missing too many)
    for (int i = 0; i < K / 2; ++i) {
        decoder.addPacket(dataPackets[i]);
    }
    for (const auto& p : parityPackets) {
        decoder.addPacket(p);
    }

    bool recovered = decoder.tryRecover();
    // With only 10% parity and 50% data loss, recovery should fail
    EXPECT_FALSE(recovered);
    EXPECT_FALSE(decoder.isComplete());
}

} // namespace omnidesk
