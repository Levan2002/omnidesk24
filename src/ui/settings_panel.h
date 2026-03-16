#pragma once

#include "ui/app.h"

namespace omnidesk {

class SettingsPanel {
public:
    static void render(AppConfig& config, bool* open);
};

} // namespace omnidesk
