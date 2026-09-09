#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "config.hpp"
#include "../src/utils/Requests/requests.hpp"
#include "main.hpp"

namespace {
void HeartbeatTick() {
    // The former detached infinite thread had no shutdown path and could use
    // mod statics while the process was tearing down. A scheduler callback has
    // the same cadence without introducing an unmanaged native thread.
    if (!getConfig().UseQuestDiscord.GetValue()) {
        nlohmann::json data;
        data["type"] = "HeartbeatReceiver";
        SendPresenceEvent(data);
    }

    // Quest mode intentionally skips heartbeats because its local RPC binding
    // maintains its own connection and HandleEvent ignores heartbeat messages.
    BSML::MainThreadScheduler::ScheduleAfterTime(10.0f, HeartbeatTick);
}
}

void Heartbeat() {
    // Delay the first heartbeat to match the old loop's steady cadence without
    // sending network traffic during the most sensitive startup frame.
    BSML::MainThreadScheduler::ScheduleAfterTime(10.0f, HeartbeatTick);
}
