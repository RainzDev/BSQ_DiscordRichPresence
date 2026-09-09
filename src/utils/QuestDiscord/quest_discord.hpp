#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace QuestDiscord {
    bool Initialize();
    void Shutdown();
    void HandleEvent(const nlohmann::json& event);
    void Refresh();
    std::string GetConnectionStatus();
    // Used only for diagnostics; this does not load or execute the helper.
    bool IsHelperAvailable();
}
