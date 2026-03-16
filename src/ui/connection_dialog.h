#pragma once

#include "core/types.h"
#include <functional>

namespace omnidesk {

class ConnectionDialog {
public:
    static void render(const UserID& fromUser,
                       std::function<void()> onAccept,
                       std::function<void()> onReject);
};

} // namespace omnidesk
