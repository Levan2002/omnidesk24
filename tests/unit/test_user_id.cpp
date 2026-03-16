#include <gtest/gtest.h>

#include "signaling/user_id.h"
#include "core/types.h"

#include <cctype>
#include <string>

namespace omnidesk {

TEST(UserIdGenerator, GeneratedIdIs8Characters) {
    UserID id = UserIdGenerator::generateRandom();
    EXPECT_EQ(id.id.length(), 8u);
    EXPECT_TRUE(id.valid());
}

TEST(UserIdGenerator, GeneratedIdIsAlphanumeric) {
    UserID id = UserIdGenerator::generateRandom();
    for (char c : id.id) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)))
            << "Character '" << c << "' is not alphanumeric";
    }
}

TEST(UserIdGenerator, TwoGeneratedIdsAreDifferent) {
    UserID id1 = UserIdGenerator::generateRandom();
    UserID id2 = UserIdGenerator::generateRandom();

    // With 8 alphanumeric characters (62^8 ≈ 2e14 possibilities),
    // the probability of collision is negligible.
    EXPECT_NE(id1, id2);
}

} // namespace omnidesk
