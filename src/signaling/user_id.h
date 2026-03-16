#pragma once

#include "core/types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace omnidesk {

// UserIdGenerator: generates and persists an 8-character alphanumeric user ID.
// Stored in:
//   Linux:   ~/.config/omnidesk24/user_id
//   Windows: %APPDATA%/omnidesk24/user_id
class UserIdGenerator {
public:
    UserIdGenerator();
    ~UserIdGenerator();

    // Load the user ID from persistent storage. If none exists, generates a new one.
    // Returns the loaded/generated UserID.
    UserID loadOrGenerate();

    // Generate a new random 8-char alphanumeric ID (does not save).
    static UserID generateRandom();

    // Save the user ID to persistent storage.
    bool save(const UserID& userId);

    // Load the user ID from persistent storage. Returns empty UserID on failure.
    UserID load();

    // Check if a user ID already exists on the signaling server.
    // collisionChecker is called with the candidate ID; return true if it collides.
    using CollisionChecker = std::function<bool(const UserID&)>;
    void setCollisionChecker(CollisionChecker checker);

    // Get the path where the user ID is stored.
    static std::string getStoragePath();

private:
    // Get the config directory, creating it if necessary.
    static std::string getConfigDir();

    // Ensure the directory exists (creates intermediate dirs).
    static bool ensureDirectory(const std::string& path);

    CollisionChecker collisionChecker_;
};

} // namespace omnidesk
