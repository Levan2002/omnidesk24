#pragma once

#include "core/types.h"
#include <functional>
#include <string>

namespace omnidesk {

class Dashboard {
public:
    static void render(const UserID& myId,
                       char* connectIdBuf, size_t connectIdBufSize,
                       std::function<void()> onConnect,
                       std::function<void()> onSettings,
                       bool signalingConnected,
                       const std::string& statusMessage);
};

} // namespace omnidesk
