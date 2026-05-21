#include "nlohmann/json.hpp"
#include "../src/config.hpp"
#include "config.h"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "main.hpp"
#include "web-utils/shared/WebUtils.hpp"

#include <stdexcept>

std::future<WebUtils::JsonResponse> CreateRequest(
    std::string method,
    std::string URLPath,
    nlohmann::json jsonData
) {
    std::promise<WebUtils::JsonResponse> promise;

    auto future = promise.get_future();

    std::thread(
        [method, URLPath, jsonData,
         promise = std::move(promise)]() mutable {

            try {

                const std::string getIp =
                    getConfig().PCIPSetting.GetValue();

                const std::string getPort =
                    getConfig().PortSetting.GetValue();

                const std::string URL =
                    "http://" + getIp + ":" + getPort + URLPath;

                std::string jsonStr = jsonData.dump();

                WebUtils::URLOptions path{ URL };
                path.noEscape = true;

                std::span<const uint8_t> body(
                    reinterpret_cast<const uint8_t*>(jsonStr.data()),
                    jsonStr.size()
                );

                std::future<WebUtils::JsonResponse> response;

                if (method == "GET") {
                    response =
                        WebUtils::GetAsync<WebUtils::JsonResponse>(path);
                }
                else if (method == "POST") {
                    response =
                        WebUtils::PostAsync<WebUtils::JsonResponse>(
                            path,
                            body
                        );
                }
                else {
                    throw std::runtime_error("Invalid method");
                }

                promise.set_value(response.get());
            }
            catch (...) {
                promise.set_exception(std::current_exception());
            }

        }).detach();

    return future;
}

std::future<WebUtils::JsonResponse> GetLatestGithub() {
    std::promise<WebUtils::JsonResponse> promise;

    auto future = promise.get_future();

    std::thread(
        [promise = std::move(promise)]() mutable {

            try {
                WebUtils::URLOptions path{ "https://api.github.com/repos/RainzDev/BeatSaberBridgeAPI.CPP/releases/latest" };
                path.noEscape = true;

                std::future<WebUtils::JsonResponse> response = WebUtils::GetAsync<WebUtils::JsonResponse>(path);

                promise.set_value(response.get());
            }
            catch (...) {
                promise.set_exception(std::current_exception());
            }

        }).detach();

    return future;
}
