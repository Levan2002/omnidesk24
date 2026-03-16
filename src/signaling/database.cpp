#include "signaling/database.h"
#include "core/logger.h"

#include <libpq-fe.h>
#include <cstdlib>
#include <ctime>

namespace omnidesk {

Database::Database() = default;

Database::~Database() {
    disconnect();
}

bool Database::connect(const std::string& connStr) {
    conn_ = PQconnectdb(connStr.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        LOG_ERROR("DB: connection failed: %s", PQerrorMessage(conn_));
        PQfinish(conn_);
        conn_ = nullptr;
        return false;
    }
    LOG_INFO("DB: connected to PostgreSQL");
    return true;
}

void Database::disconnect() {
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool Database::isConnected() const {
    return conn_ && PQstatus(conn_) == CONNECTION_OK;
}

bool Database::initSchema() {
    if (!isConnected()) return false;

    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS users (
            user_id         VARCHAR(8) PRIMARY KEY,
            public_host     VARCHAR(64),
            public_port     INTEGER,
            local_host      VARCHAR(64),
            local_port      INTEGER,
            is_online       BOOLEAN DEFAULT TRUE,
            registered_at   TIMESTAMP DEFAULT NOW(),
            last_seen       TIMESTAMP DEFAULT NOW()
        );

        CREATE TABLE IF NOT EXISTS sessions (
            id              BIGSERIAL PRIMARY KEY,
            host_user_id    VARCHAR(8) NOT NULL,
            viewer_user_id  VARCHAR(8) NOT NULL,
            host_addr       VARCHAR(64),
            viewer_addr     VARCHAR(64),
            started_at      TIMESTAMP DEFAULT NOW(),
            ended_at        TIMESTAMP,
            status          VARCHAR(20) DEFAULT 'active'
        );

        CREATE INDEX IF NOT EXISTS idx_sessions_host ON sessions(host_user_id);
        CREATE INDEX IF NOT EXISTS idx_sessions_viewer ON sessions(viewer_user_id);
        CREATE INDEX IF NOT EXISTS idx_sessions_status ON sessions(status);

        CREATE TABLE IF NOT EXISTS events (
            id              BIGSERIAL PRIMARY KEY,
            event_type      VARCHAR(32) NOT NULL,
            user_id         VARCHAR(8),
            details         TEXT,
            created_at      TIMESTAMP DEFAULT NOW()
        );

        CREATE INDEX IF NOT EXISTS idx_events_type ON events(event_type);
        CREATE INDEX IF NOT EXISTS idx_events_user ON events(user_id);
    )SQL";

    if (!exec(schema)) {
        LOG_ERROR("DB: failed to create schema");
        return false;
    }
    LOG_INFO("DB: schema initialized");
    return true;
}

bool Database::upsertUser(const std::string& userId, const std::string& publicHost,
                           uint16_t publicPort, const std::string& localHost,
                           uint16_t localPort) {
    std::string sql = R"SQL(
        INSERT INTO users (user_id, public_host, public_port, local_host, local_port,
                           is_online, last_seen)
        VALUES ($1, $2, $3, $4, $5, TRUE, NOW())
        ON CONFLICT (user_id) DO UPDATE SET
            public_host = $2, public_port = $3,
            local_host = $4, local_port = $5,
            is_online = TRUE, last_seen = NOW()
    )SQL";

    return execParams(sql, {userId, publicHost, std::to_string(publicPort),
                            localHost, std::to_string(localPort)});
}

bool Database::removeUser(const std::string& userId) {
    return execParams("DELETE FROM users WHERE user_id = $1", {userId});
}

bool Database::setUserOffline(const std::string& userId) {
    return execParams(
        "UPDATE users SET is_online = FALSE, last_seen = NOW() WHERE user_id = $1",
        {userId});
}

int64_t Database::createSession(const std::string& hostId, const std::string& viewerId,
                                 const std::string& hostAddr, const std::string& viewerAddr) {
    if (!isConnected()) return -1;

    const char* paramValues[4] = {
        hostId.c_str(), viewerId.c_str(), hostAddr.c_str(), viewerAddr.c_str()
    };

    PGresult* res = PQexecParams(conn_,
        "INSERT INTO sessions (host_user_id, viewer_user_id, host_addr, viewer_addr) "
        "VALUES ($1, $2, $3, $4) RETURNING id",
        4, nullptr, paramValues, nullptr, nullptr, 0);

    int64_t id = -1;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        id = std::strtoll(PQgetvalue(res, 0, 0), nullptr, 10);
    } else {
        LOG_ERROR("DB: createSession failed: %s", PQerrorMessage(conn_));
    }
    PQclear(res);
    return id;
}

bool Database::endSession(int64_t sessionId, const std::string& status) {
    return execParams(
        "UPDATE sessions SET ended_at = NOW(), status = $1 WHERE id = $2",
        {status, std::to_string(sessionId)});
}

int64_t Database::totalSessions() {
    std::string val = queryScalar("SELECT COUNT(*) FROM sessions");
    return val.empty() ? 0 : std::strtoll(val.c_str(), nullptr, 10);
}

int64_t Database::activeSessions() {
    std::string val = queryScalar("SELECT COUNT(*) FROM sessions WHERE status = 'active'");
    return val.empty() ? 0 : std::strtoll(val.c_str(), nullptr, 10);
}

int64_t Database::totalRegisteredUsers() {
    std::string val = queryScalar("SELECT COUNT(*) FROM users WHERE is_online = TRUE");
    return val.empty() ? 0 : std::strtoll(val.c_str(), nullptr, 10);
}

std::vector<SessionRecord> Database::recentSessions(int limit) {
    std::vector<SessionRecord> records;
    if (!isConnected()) return records;

    std::string sql =
        "SELECT id, host_user_id, viewer_user_id, host_addr, viewer_addr, "
        "EXTRACT(EPOCH FROM started_at)::BIGINT, "
        "COALESCE(EXTRACT(EPOCH FROM ended_at)::BIGINT, 0), status "
        "FROM sessions ORDER BY started_at DESC LIMIT " + std::to_string(limit);

    PGresult* res = PQexec(conn_, sql.c_str());
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; ++i) {
            SessionRecord r;
            r.id = std::strtoll(PQgetvalue(res, i, 0), nullptr, 10);
            r.hostUserId = PQgetvalue(res, i, 1);
            r.viewerUserId = PQgetvalue(res, i, 2);
            r.hostPublicAddr = PQgetvalue(res, i, 3);
            r.viewerPublicAddr = PQgetvalue(res, i, 4);
            r.startedAt = std::strtoll(PQgetvalue(res, i, 5), nullptr, 10);
            r.endedAt = std::strtoll(PQgetvalue(res, i, 6), nullptr, 10);
            r.status = PQgetvalue(res, i, 7);
            records.push_back(std::move(r));
        }
    }
    PQclear(res);
    return records;
}

bool Database::logEvent(const std::string& eventType, const std::string& userId,
                         const std::string& details) {
    return execParams(
        "INSERT INTO events (event_type, user_id, details) VALUES ($1, $2, $3)",
        {eventType, userId, details});
}

// ---- Private helpers ----

bool Database::exec(const std::string& sql) {
    if (!isConnected()) return false;
    PGresult* res = PQexec(conn_, sql.c_str());
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
               PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!ok) {
        LOG_ERROR("DB: exec failed: %s", PQerrorMessage(conn_));
    }
    PQclear(res);
    return ok;
}

bool Database::execParams(const std::string& sql, const std::vector<std::string>& params) {
    if (!isConnected()) return false;

    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& p : params) {
        values.push_back(p.c_str());
    }

    PGresult* res = PQexecParams(conn_, sql.c_str(),
                                  static_cast<int>(params.size()),
                                  nullptr, values.data(),
                                  nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK ||
               PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!ok) {
        LOG_ERROR("DB: execParams failed: %s", PQerrorMessage(conn_));
    }
    PQclear(res);
    return ok;
}

std::string Database::queryScalar(const std::string& sql) {
    if (!isConnected()) return "";
    PGresult* res = PQexec(conn_, sql.c_str());
    std::string result;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        result = PQgetvalue(res, 0, 0);
    }
    PQclear(res);
    return result;
}

} // namespace omnidesk
