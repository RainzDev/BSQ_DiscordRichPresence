#include "nlohmann/json.hpp"
#include "config.hpp"
#include "config.h"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "main.hpp"
#include "web-utils/shared/WebUtils.hpp"
#include "utils/QuestDiscord/quest_discord.hpp"

#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <span>
#include <exception>

namespace {
struct RequestTask {
    std::string method;
    std::string url;
    nlohmann::json data;
    std::promise<WebUtils::JsonResponse> promise;
};

// Own exactly one desktop-companion worker. The previous implementation
// detached both an infinite queue worker and one additional waiter per HTTP
// response, allowing unbounded threads and out-of-order delivery during rapid
// gameplay events.
class RequestDispatcher {
public:
    RequestDispatcher() : worker([this] {
        Run();
    }) {}

    ~RequestDispatcher() {
        // Android's libc++ configuration does not expose std::jthread, so use an
        // explicit stop flag and join while all dispatcher members still exist.
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stopping = true;
        }
        queueCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    std::future<WebUtils::JsonResponse> Enqueue(
        std::string method, std::string url, nlohmann::json data) {
        auto task = std::make_unique<RequestTask>();
        task->method = std::move(method);
        task->url = std::move(url);
        task->data = std::move(data);
        auto future = task->promise.get_future();

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            queue.push_back(std::move(task));
        }
        queueCv.notify_one();
        return future;
    }

private:
    void Run() noexcept {
        while (true) {
            std::unique_ptr<RequestTask> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait(lock, [this] {
                    return stopping || !queue.empty();
                });
                if (stopping && queue.empty()) return;
                task = std::move(queue.front());
                queue.pop_front();
            }

            // std::future reports WebUtils transport failures as exceptions.
            // Catch them only at this background boundary and forward them to
            // the caller's future so none can terminate the game process.
            try {
                std::string eventType = "none";
                if (task->data.is_object()) {
                    const auto type = task->data.find("type");
                    if (type != task->data.end() && type->is_string()) {
                        eventType = type->get_ref<const std::string&>();
                    }
                }
                // Routine heartbeats/stat updates can run every ten seconds.
                // Keep useful routing context at DEBUG without placing complete
                // song/activity payloads into the normal support log.
                logger.debug("RequestDispatcher::Run sending {} {} (event={})",
                             task->method, task->url, eventType);

                WebUtils::URLOptions path{task->url};
                path.noEscape = true;
                if (task->method == "GET") {
                    auto response = WebUtils::GetAsync<WebUtils::JsonResponse>(path);
                    task->promise.set_value(response.get());
                } else {
                    // Keep the POST storage in this worker frame until get()
                    // completes; WebUtils receives only a non-owning span.
                    const std::string jsonText = task->data.dump();
                    const std::vector<uint8_t> body(jsonText.begin(), jsonText.end());
                    const std::span<const uint8_t> bytes(body.data(), body.size());
                    auto response = WebUtils::PostAsync<WebUtils::JsonResponse>(path, bytes);
                    task->promise.set_value(response.get());
                }
            } catch (const std::exception& error) {
                logger.error("RequestDispatcher::Run failed for {} {}: {}",
                             task->method, task->url, error.what());
                task->promise.set_exception(std::current_exception());
            } catch (...) {
                // Third-party networking code can propagate non-standard
                // exceptions. Preserve them in the future instead of allowing
                // an uncaught worker exception to call std::terminate.
                logger.error("RequestDispatcher::Run failed for {} {} with a non-standard exception",
                             task->method, task->url);
                task->promise.set_exception(std::current_exception());
            }
        }
    }

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::unique_ptr<RequestTask>> queue;
    bool stopping = false;
    std::thread worker;
};

// Construct the dispatcher on first desktop request. Its owned worker remains
// valid for the mod's process lifetime and joins during orderly shutdown.
RequestDispatcher& GetRequestDispatcher() {
    static RequestDispatcher dispatcher;
    return dispatcher;
}

// Return a failed future for invalid caller input without starting a worker or
// throwing through a Unity hook/UI callback.
std::future<WebUtils::JsonResponse> MakeFailedRequest(const char* message) {
    std::promise<WebUtils::JsonResponse> promise;
    auto future = promise.get_future();
    promise.set_exception(std::make_exception_ptr(std::invalid_argument(message)));
    return future;
}
}

std::future<WebUtils::JsonResponse> CreateRequest(
    std::string method,
    std::string URLPath,
    nlohmann::json jsonData
) {
    // Reject unsupported verbs explicitly rather than throwing from the worker
    // after a gameplay hook has already queued the request.
    if (method != "GET" && method != "POST") {
        logger.error("Desktop companion request rejected: unsupported method {}", method);
        return MakeFailedRequest("Unsupported desktop companion HTTP method");
    }

    // Resolve config on the caller (Unity) thread. Config-utils state is not
    // guaranteed to be safe to read from a native networking worker.
    std::string url;
    if (URLPath.rfind("http://", 0) == 0 || URLPath.rfind("https://", 0) == 0) {
        url = std::move(URLPath);
    } else {
        const std::string ip = getConfig().PCIPSetting.GetValue();
        const std::string port = getConfig().PortSetting.GetValue();
        if (ip.empty() || port.empty()) {
            logger.error("Desktop companion request skipped because its IP or port is empty");
            return MakeFailedRequest("Desktop companion IP or port is empty");
        }
        url = "http://" + ip + ":" + port + URLPath;
    }

    try {
        return GetRequestDispatcher().Enqueue(
            std::move(method), std::move(url), std::move(jsonData));
    } catch (const std::exception& error) {
        // Thread creation and queue allocation can fail. Convert that failure
        // into the returned future instead of unwinding through a Unity hook.
        logger.error("CreateRequest could not access RequestDispatcher: {}", error.what());
        return MakeFailedRequest("Desktop request dispatcher is unavailable");
    } catch (...) {
        logger.error("CreateRequest could not access RequestDispatcher due to a non-standard exception");
        return MakeFailedRequest("Desktop request dispatcher failed with a non-standard exception");
    }
}

std::future<WebUtils::JsonResponse> GetLatestGithub() {
    // Enqueue the github latest release request to preserve ordering
    nlohmann::json empty;
    return CreateRequest("GET", "https://api.github.com/repos/RainzDev/BeatSaberBridgeAPI.CPP/releases/latest", empty);
}

void SendPresenceEvent(const nlohmann::json& jsonData) noexcept {
    // This function is called by hooks and recurring scheduler callbacks. Keep
    // it as the final exception boundary so malformed metadata or a failed queue
    // allocation can never unwind into Beat Saber.
    try {
        if (getConfig().UseQuestDiscord.GetValue()) {
            QuestDiscord::HandleEvent(jsonData);
            return;
        }
        CreateRequest("POST", "/sendData", jsonData);
    } catch (const std::exception& error) {
        logger.error("SendPresenceEvent dropped an event safely: {}", error.what());
    } catch (...) {
        logger.error("SendPresenceEvent dropped an event after a non-standard exception");
    }
}

void SetQuestDiscordMode(bool enabled) noexcept {
    // A settings toggle runs directly inside a Unity UI callback. Contain all
    // transport setup failures here so changing modes cannot close the game.
    try {
        getConfig().UseQuestDiscord.SetValue(enabled);
        nlohmann::json mainMenu = {{"type", "MainMenuInitialized"}};
        if (enabled) {
            if (!QuestDiscord::Initialize()) {
                logger.warn("Quest Discord mode is enabled but its RPC bridge is not connected yet");
            }
            QuestDiscord::HandleEvent(mainMenu);
        } else {
            QuestDiscord::Shutdown();
            CreateRequest("POST", "/sendData", mainMenu);
        }
    } catch (const std::exception& error) {
        logger.error("SetQuestDiscordMode failed safely: {}", error.what());
    } catch (...) {
        logger.error("SetQuestDiscordMode failed with a non-standard exception");
    }
}
