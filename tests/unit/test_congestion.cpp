#include <gtest/gtest.h>

#include "transport/congestion.h"

namespace omnidesk {

TEST(CongestionController, InitialBitrateEqualsConfigured) {
    CongestionController cc;
    EXPECT_EQ(cc.getTargetBitrate(), CongestionController::INITIAL_BITRATE_BPS);
}

TEST(CongestionController, BitrateIncreasesAfterAcks) {
    CongestionController cc;
    uint32_t initialBitrate = cc.getTargetBitrate();

    // Simulate sending packets and receiving ACKs with stable RTT
    for (int i = 0; i < 100; ++i) {
        uint64_t seq = cc.onPacketSent(1000);
        cc.onAckReceived(seq, 20.0f);  // Stable 20ms RTT
    }

    EXPECT_GT(cc.getTargetBitrate(), initialBitrate);
}

TEST(CongestionController, BitrateDecreasesAfterLoss) {
    CongestionController cc;

    // First, let the controller increase the bitrate with some ACKs
    for (int i = 0; i < 50; ++i) {
        uint64_t seq = cc.onPacketSent(1000);
        cc.onAckReceived(seq, 20.0f);
    }

    uint32_t bitrateBeforeLoss = cc.getTargetBitrate();

    // Now simulate packet losses
    for (int i = 0; i < 10; ++i) {
        uint64_t seq = cc.onPacketSent(1000);
        cc.onLoss(seq);
    }

    EXPECT_LT(cc.getTargetBitrate(), bitrateBeforeLoss);
}

TEST(CongestionController, BitrateStaysWithinBounds) {
    CongestionController cc;

    // Drive bitrate up with many ACKs
    for (int i = 0; i < 10000; ++i) {
        uint64_t seq = cc.onPacketSent(1000);
        cc.onAckReceived(seq, 5.0f);
    }
    EXPECT_LE(cc.getTargetBitrate(), CongestionController::MAX_BITRATE_BPS);

    // Drive bitrate down with many losses
    for (int i = 0; i < 10000; ++i) {
        uint64_t seq = cc.onPacketSent(1000);
        cc.onLoss(seq);
    }
    EXPECT_GE(cc.getTargetBitrate(), CongestionController::MIN_BITRATE_BPS);
}

} // namespace omnidesk
