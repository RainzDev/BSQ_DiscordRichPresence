#include "./Requests/requests.hpp"
#include "config.hpp"
#include "Requests/requests.hpp"

#include <fstream>
#include <thread>

#include "main.hpp"

#include "../shared/utils/cache_handler.hpp"
#include "../shared/nlohmann/json.hpp"

namespace {
    constexpr const char* JsonCacheSource = "/sdcard/ModData/com.beatgames.beatsaber/Mods/DiscordRichPresence/cover.json";

    void UpdateJsonFile(std::string jsonStr) {
        std::thread([jsonStr]() {
            std::ofstream outFile(JsonCacheSource);
            if (outFile.is_open()) {
                outFile << jsonStr;

                outFile.close();
                logger.info("Successfully closed the file");
            } else {
                logger.error("Could not open the file");
            }
        }).detach();
    }

    nlohmann::json ReadJsonFile() {
        std::ifstream inFile(JsonCacheSource);
        if (!inFile.is_open()) {
            logger.error("Could not open the file");
            return {};
        }

        nlohmann::json data;
        inFile >> data;


        return data;
    }

    void RefreshCoverCacheInternal() {
        std::shared_future<WebUtils::JsonResponse> future = CreateRequest("GET", "https://api.github.com/repos/RainzDev/cursed-beatsaber-image-host/contents/cover", {});

        BSML::MainThreadScheduler::AwaitFuture<WebUtils::JsonResponse>(
            future,
            [future]() mutable -> void
            {
                const auto& result = future.get();

                rapidjson::StringBuffer buffer;
                rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
                result.GetParsedData().Accept(writer);

                std::string jsonStr(buffer.GetString());

                UpdateJsonFile(jsonStr);
            }
        );
        return;
    }
}

namespace QuestDiscord::Cache {
    EXPORT void RefreshCoverCache() {
        RefreshCoverCacheInternal();
    }

    EXPORT void GetCoverCache(std::function<void(nlohmann::json)> callback) {
        std::thread([callback] {
            try {
                nlohmann::json data = ReadJsonFile();
                callback(data);
            } catch (const std::exception& e) {
                callback(nlohmann::json::object()); 
            }
        }).detach();
    }
}