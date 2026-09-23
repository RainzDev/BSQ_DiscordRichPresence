#pragma once

#include "../config.h"

#include <string>
#include <ctime>

#include "../nlohmann/json.hpp"

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

    /// @brief Starts up the Discord Rich Presence connection.
    /// @return true if the connection was successfully established, false otherwise.
    EXPORT bool Initialize();

    /// @brief Shuts down the Discord Rich Presence connection.
    EXPORT void Shutdown();
    /// @brief Handles the event state.
    /// @param event The event JSON object.
    EXPORT void HandleEvent(const nlohmann::json& event);
    /// @brief Refreshes the Discord Rich Presence connection.
    EXPORT void Refresh();
    /// @brief Gets the connection status.
    /// @return The string for the whole status indicating if it's ready, specific errors, etc.
    EXPORT std::string GetConnectionStatus();
    /// @brief Checks if the helper is available.
    /// @return true if available, false otherwise.
    /// @note  This does not load or execute the helper.
    EXPORT bool IsHelperAvailable();
    /// @brief Gets the current state of the gameplay.
    /// @param state Details in the current state.
    /// @return The current state of the gameplay.
    EXPORT std::string GameplayState(const PresenceState& state);
    /// @brief Gets the current state of the song.
    /// @param state Details in the current state.
    /// @return The current state of the song.
    EXPORT std::string SongDetails(const PresenceState& state);
    /// @brief Builds the activity that should be sent for the given state.
    /// @return true if the helper is available, false otherwise.
    EXPORT nlohmann::json BuildActivity(const PresenceState& state);
    /// @brief Sends a fully custom activity object immediately.
    /// @param activity The activity JSON object.
    EXPORT void SendCustomActivity(const nlohmann::json& activity);
    /// @brief Sends a fully custom activity object immediately.
    /// @param activity The activity as stringified JSON.
    EXPORT void SendCustomActivity(const std::string& activity);
    /// @brief Send a pre-built "frame" object (contains cmd/args/nonce) directly.
    /// @param frame The frame JSON object.
    EXPORT void SendCustomFrame(const nlohmann::json& frame);
    /// @brief Send a pre-built "frame" object (contains cmd/args/nonce) directly.
    /// @param frame The frame as stringified JSON.
    EXPORT void SendCustomFrame(const std::string& frame);
}
