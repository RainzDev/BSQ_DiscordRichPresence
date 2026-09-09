#include "GlobalNamespace/ScoreUIController.hpp"
#include "GlobalNamespace/IScoreController.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/NoteController.hpp"
#include "GlobalNamespace/NoteCutInfo.hpp"

#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include "main.hpp"
#include "config.hpp"
#include "../src/utils/Requests/requests.hpp"

#include "metacore/shared/stats.hpp"

namespace {
constexpr float DesktopStatUpdateIntervalSeconds = 10.0f;

float NextStatUpdateIntervalSeconds() {
    // Preserve the desktop companion's original cadence exactly. The new menu
    // preference is intentionally read only while native Quest routing is on.
    if (!getConfig().UseQuestDiscord.GetValue()) {
        return DesktopStatUpdateIntervalSeconds;
    }

    return static_cast<float>(GetQuestDiscordUpdateIntervalSeconds());
}

void StatUpdateTick() {
    // MetaCore reads live IL2CPP/Unity gameplay state. Accessing that state from
    // the old detached std::thread could race scene construction and leave
    // UnityMain spinning during the transition into both OST and custom maps.
    // Keep the entire read on Unity's main thread and schedule the next sample
    // only after this one has completed.
    if (inSingleplayerGameplay || inMultiplayerGameplay) {
        nlohmann::json data;
        data["type"] = "BeatmapStatUpdate";
        data["score"] = MetaCore::Stats::GetScore(2);
        data["notesMissed"] = MetaCore::Stats::GetNotesMissed(2);
        data["notesBadCut"] = MetaCore::Stats::GetNotesBadCut(2);
        data["bombsHit"] = MetaCore::Stats::GetBombsHit(2);
        data["currentTime"] = MetaCore::Stats::GetSongTime();
        // The PC companion has no combo field and is intentionally unchanged.
        // Add this value only when the same event will be routed to the native
        // Quest Discord state machine.
        if (getConfig().UseQuestDiscord.GetValue()) {
            data["combo"] = MetaCore::Stats::GetCombo(2);
        }
        SendPresenceEvent(data);
    }

    // Resolve the interval after each update so an in-menu change takes effect
    // on the next scheduling cycle without spawning another recurring worker.
    BSML::MainThreadScheduler::ScheduleAfterTime(
        NextStatUpdateIntervalSeconds(), StatUpdateTick);
}
}

void StatUpdate() {
    // Select the initial delay through the same mode-aware path used by every
    // later tick, keeping Desktop Companion fixed at ten seconds.
    BSML::MainThreadScheduler::ScheduleAfterTime(
        NextStatUpdateIntervalSeconds(), StatUpdateTick);
}
