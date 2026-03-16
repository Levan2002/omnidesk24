#include "signaling/user_id.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#  include <direct.h>
#  include <shlobj.h>
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace omnidesk {

static constexpr const char* ALPHANUMERIC = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static constexpr size_t ALPHANUMERIC_LEN = 62;
static constexpr size_t USER_ID_LENGTH = 8;

UserIdGenerator::UserIdGenerator() = default;
UserIdGenerator::~UserIdGenerator() = default;

void UserIdGenerator::setCollisionChecker(CollisionChecker checker) {
    collisionChecker_ = std::move(checker);
}

std::string UserIdGenerator::getConfigDir() {
    std::string dir;

#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        dir = std::string(path) + "\\omnidesk24";
    } else {
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            dir = std::string(appdata) + "\\omnidesk24";
        }
    }
#else
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (home) {
        dir = std::string(home) + "/.config/omnidesk24";
    }
#endif

    return dir;
}

bool UserIdGenerator::ensureDirectory(const std::string& path) {
    if (path.empty()) return false;

#ifdef _WIN32
    // Create directory recursively
    size_t pos = 0;
    while ((pos = path.find_first_of("\\/", pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        _mkdir(sub.c_str());
    }
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    // Create directory recursively
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string sub = path.substr(0, pos);
        mkdir(sub.c_str(), 0755);
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string UserIdGenerator::getStoragePath() {
    std::string dir = getConfigDir();
    if (dir.empty()) return "";

#ifdef _WIN32
    return dir + "\\user_id";
#else
    return dir + "/user_id";
#endif
}

UserID UserIdGenerator::generateRandom() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, ALPHANUMERIC_LEN - 1);

    UserID userId;
    userId.id.resize(USER_ID_LENGTH);
    for (size_t i = 0; i < USER_ID_LENGTH; ++i) {
        userId.id[i] = ALPHANUMERIC[dist(gen)];
    }
    return userId;
}

UserID UserIdGenerator::load() {
    std::string path = getStoragePath();
    if (path.empty()) return UserID{};

    std::ifstream file(path);
    if (!file.is_open()) return UserID{};

    std::string line;
    std::getline(file, line);

    // Trim whitespace
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                              line.back() == ' ')) {
        line.pop_back();
    }

    if (line.size() != USER_ID_LENGTH) return UserID{};

    // Validate all characters are alphanumeric
    for (char c : line) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return UserID{};
    }

    return UserID{line};
}

bool UserIdGenerator::save(const UserID& userId) {
    if (!userId.valid()) return false;

    std::string dir = getConfigDir();
    if (dir.empty()) return false;

    ensureDirectory(dir);

    std::string path = getStoragePath();
    if (path.empty()) return false;

    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << userId.id << std::endl;
    return file.good();
}

UserID UserIdGenerator::loadOrGenerate() {
    // Try to load existing ID
    UserID existing = load();
    if (existing.valid()) return existing;

    // Generate new ID, checking for collisions if a checker is set
    UserID newId;
    static constexpr int MAX_COLLISION_RETRIES = 10;

    for (int i = 0; i < MAX_COLLISION_RETRIES; ++i) {
        newId = generateRandom();

        if (!collisionChecker_ || !collisionChecker_(newId)) {
            break;  // No collision
        }

        if (i == MAX_COLLISION_RETRIES - 1) {
            // Give up collision checking; use the last generated ID anyway
        }
    }

    save(newId);
    return newId;
}

} // namespace omnidesk
