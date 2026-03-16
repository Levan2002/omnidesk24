#pragma once

#include "core/types.h"
#include "transport/protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward-declare libpq handle
struct pg_conn;
typedef struct pg_conn PGconn;

namespace omnidesk {

// Session record for connection history
struct SessionRecord {
    int64_t id = 0;
    std::string hostUserId;
    std::string viewerUserId;
    std::string hostPublicAddr;
    std::string viewerPublicAddr;
    int64_t startedAt = 0;   // unix timestamp
    int64_t endedAt = 0;     // 0 if still active
    std::string status;      // "active", "completed", "failed"
};

// PostgreSQL database interface for the signaling server.
// Stores registered users, connection history, and session metrics.
class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Connect to PostgreSQL using a connection string.
    // e.g. "host=localhost port=5432 dbname=omnidesk24 user=omnidesk password=secret"
    bool connect(const std::string& connStr);

    // Disconnect from the database.
    void disconnect();

    // Check if connected.
    bool isConnected() const;

    // Create tables if they don't exist.
    bool initSchema();

    // User registration persistence
    bool upsertUser(const std::string& userId, const std::string& publicHost,
                    uint16_t publicPort, const std::string& localHost,
                    uint16_t localPort);
    bool removeUser(const std::string& userId);
    bool setUserOffline(const std::string& userId);

    // Session tracking
    int64_t createSession(const std::string& hostId, const std::string& viewerId,
                          const std::string& hostAddr, const std::string& viewerAddr);
    bool endSession(int64_t sessionId, const std::string& status);

    // Stats / queries
    int64_t totalSessions();
    int64_t activeSessions();
    int64_t totalRegisteredUsers();
    std::vector<SessionRecord> recentSessions(int limit = 20);

    // Log a signaling event (for debugging/auditing)
    bool logEvent(const std::string& eventType, const std::string& userId,
                  const std::string& details);

private:
    bool exec(const std::string& sql);
    bool execParams(const std::string& sql, const std::vector<std::string>& params);
    std::string queryScalar(const std::string& sql);

    PGconn* conn_ = nullptr;
};

} // namespace omnidesk
