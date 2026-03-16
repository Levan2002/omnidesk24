#pragma once

namespace omnidesk {

class HostSession;
class ViewerSession;

class StatsOverlay {
public:
    static void render(HostSession* host, ViewerSession* viewer);
};

} // namespace omnidesk
