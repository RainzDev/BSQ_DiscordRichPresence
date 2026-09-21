#pragma once

#include "config.h"

#include <string>

#include "nlohmann/json.hpp"

namespace QuestDiscord {
    EXPORT bool Initialize();
    EXPORT void Shutdown();
    EXPORT void HandleEvent(const nlohmann::json& event);
    EXPORT void Refresh();
    EXPORT std::string GetConnectionStatus();
    // Used only for diagnostics; this does not load or execute the helper.
    EXPORT bool IsHelperAvailable();
}
