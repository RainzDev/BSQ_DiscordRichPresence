#pragma once

#include "../shared/config.h"

#include <string>
#include <ctime>

#include "nlohmann/json.hpp"

namespace QuestDiscord {
    struct EXPORT PresenceState {
        std::string phase = "MainMenu";
        nlohmann::json song = nlohmann::json::object();
        nlohmann::json stats = nlohmann::json::object();
        int playerCount = 0;
        int maxPlayerCount = 0;
        std::string lobbyCode;
        std::string partyId;
        std::time_t songStart = 0;
        std::time_t songEnd = 0;
        int songLength = 0;
        std::time_t pauseStart = 0;
    };

    EXPORT bool Initialize();
    EXPORT void Shutdown();
    EXPORT void HandleEvent(const nlohmann::json& event);
    EXPORT void Refresh();
    EXPORT std::string GetConnectionStatus();
    // Used only for diagnostics; this does not load or execute the helper.
    EXPORT bool IsHelperAvailable();
    EXPORT nlohmann::json BuildActivity(const PresenceState& state);
}
