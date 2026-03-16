#include <gtest/gtest.h>

#include "transport/protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace omnidesk {

TEST(Protocol, ControlHeaderRoundtrip) {
    ControlHeader original;
    original.magic = PROTOCOL_MAGIC;
    original.version = PROTOCOL_VERSION;
    original.type = static_cast<uint16_t>(MessageType::KEY_FRAME_REQ);
    original.length = 42;

    uint8_t buf[ControlHeader::SIZE];
    original.serialize(buf);

    ControlHeader deserialized = ControlHeader::deserialize(buf);
    EXPECT_EQ(deserialized.magic, original.magic);
    EXPECT_EQ(deserialized.version, original.version);
    EXPECT_EQ(deserialized.type, original.type);
    EXPECT_EQ(deserialized.length, original.length);
    EXPECT_TRUE(deserialized.valid());
}

TEST(Protocol, VideoHeaderRoundtrip) {
    VideoHeader original;
    original.frameId = 0xDEADBEEF;
    original.fragId = 42;
    original.fragCount = 100;
    original.fecGroup = 7;
    original.flags = FLAG_KEYFRAME | FLAG_HAS_DIRTY_RECTS;
    original.rectCount = 3;

    uint8_t buf[VideoHeader::SIZE];
    original.serialize(buf);

    VideoHeader deserialized = VideoHeader::deserialize(buf);
    EXPECT_EQ(deserialized.frameId, original.frameId);
    EXPECT_EQ(deserialized.fragId, original.fragId);
    EXPECT_EQ(deserialized.fragCount, original.fragCount);
    EXPECT_EQ(deserialized.fecGroup, original.fecGroup);
    EXPECT_EQ(deserialized.flags, original.flags);
    EXPECT_EQ(deserialized.rectCount, original.rectCount);
}

TEST(Protocol, MagicNumberAndByteOrder) {
    // PROTOCOL_MAGIC is "OMND" = 0x4F4D4E44
    EXPECT_EQ(PROTOCOL_MAGIC, 0x4F4D4E44u);

    ControlHeader h;
    uint8_t buf[ControlHeader::SIZE];
    h.serialize(buf);

    // Verify magic is stored in network byte order (big-endian)
    // 0x4F4D4E44 big-endian: 4F 4D 4E 44
    EXPECT_EQ(buf[0], 0x4F);
    EXPECT_EQ(buf[1], 0x4D);
    EXPECT_EQ(buf[2], 0x4E);
    EXPECT_EQ(buf[3], 0x44);
}

TEST(Protocol, WriteU16_ReadU16) {
    uint8_t buf[2];

    writeU16(buf, 0x0000);
    EXPECT_EQ(readU16(buf), 0x0000u);

    writeU16(buf, 0xFFFF);
    EXPECT_EQ(readU16(buf), 0xFFFFu);

    writeU16(buf, 0x1234);
    EXPECT_EQ(readU16(buf), 0x1234u);

    writeU16(buf, 0xABCD);
    EXPECT_EQ(readU16(buf), 0xABCDu);
}

TEST(Protocol, WriteU32_ReadU32) {
    uint8_t buf[4];

    writeU32(buf, 0x00000000);
    EXPECT_EQ(readU32(buf), 0x00000000u);

    writeU32(buf, 0xFFFFFFFF);
    EXPECT_EQ(readU32(buf), 0xFFFFFFFFu);

    writeU32(buf, 0x12345678);
    EXPECT_EQ(readU32(buf), 0x12345678u);

    writeU32(buf, 0xDEADBEEF);
    EXPECT_EQ(readU32(buf), 0xDEADBEEFu);
}

} // namespace omnidesk
