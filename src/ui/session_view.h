#pragma once

#include <functional>

namespace omnidesk {

class HostSession;
class ViewerSession;

class SessionView {
public:
    static void render(HostSession* host, ViewerSession* viewer,
                       std::function<void()> onDisconnect,
                       std::function<void()> onToggleStats,
                       std::function<void()> onToggleSettings);
};

} // namespace omnidesk
