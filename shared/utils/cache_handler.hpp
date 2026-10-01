#pragma once

#include "../config.h"
#include "./nlohmann/json.hpp"

#include <future>

namespace QuestDiscord::Cache {
    /// @brief Requests a refresh of the cover cache from the GitHub repository.
    EXPORT void RefreshCoverCache();
    /// @brief Retrieves the cover cache from the local JSON file.
    /// @return Cover cache data as a JSON object.
    EXPORT void GetCoverCache(std::function<void(nlohmann::json)> callback);
}