// Include our main header file, containing the logger
#include "main.hpp"
#include "nlohmann/json.hpp"
#include "config.hpp"
#include "UI/ui.hpp"
#include "utils/Requests/requests.hpp"
#include "utils/QuestDiscord/quest_discord.hpp"

#include "config.h"

#include "nlohmann/json_fwd.hpp"
#include "web-utils/shared/WebUtils.hpp"

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/Helpers/getters.hpp"

#include <span>
#include <cstdint>
#include <codecvt>
#include <exception>
#include <vector>

// Include dependency headers
#include "scotland2/shared/modloader.h"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include <iostream>

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Application.hpp"

#include "System/Action_1.hpp"
#include <string>
#include <string_view>
#include <thread>

#include "GlobalNamespace/UnifiedNetworkPlayerModel.hpp"
#include "GlobalNamespace/IUnifiedNetworkPlayerModel.hpp"

#include "GlobalNamespace/PauseMenuManager.hpp"
#include "GlobalNamespace/LevelCompletionResults.hpp"
#include "GlobalNamespace/IConnectedPlayer.hpp"
#include "GlobalNamespace/MultiplayerPlayersManager.hpp"
#include "GlobalNamespace/MultiplayerSessionManager.hpp"
#include "GlobalNamespace/MultiplayerLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/StandardLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/IReadonlyBeatmapData.hpp"
#include "GlobalNamespace/MultiplayerLocalActivePlayerGameplayManager.hpp"
#include "GlobalNamespace/MultiplayerLocalActivePlayerGameplayAnimator.hpp"
#include "GlobalNamespace/StandardLevelGameplayManager.hpp"
#include "GlobalNamespace/AudioTimeSyncController.hpp"
#include "GlobalNamespace/TutorialSongController.hpp"
#include "GlobalNamespace/MissionLevelScenesTransitionSetupDataSO.hpp"
#include "GlobalNamespace/MissionLevelGameplayManager.hpp"
#include "GlobalNamespace/PauseController.hpp"
#include "GlobalNamespace/MenuDestination.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/BeatmapDifficulty.hpp"
#include "GlobalNamespace/ILobbyPlayersDataModel.hpp"
#include "GlobalNamespace/BeatmapDifficulty.hpp"
#include "GlobalNamespace/LobbyPlayersDataModel.hpp"
#include "System/Collections/Generic/IReadOnlyDictionary_2.hpp"

#include "GlobalNamespace/MultiplayerModeSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/SongStartSyncController.hpp"
#include "GlobalNamespace/MultiplayerResultsViewController.hpp"
#include "GlobalNamespace/GameServerLobbyFlowCoordinator.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/ResultsViewController.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/ConnectedPlayerManager.hpp"

#include "Zenject/DiContainer.hpp"

#include "metacore/shared/unity.hpp"
#include "metacore/shared/songs.hpp"

#include "utils/Schedulers/Heartbeats/heartbeat.hpp"
#include "utils/Schedulers/Beatmap/beatmap.hpp"

#include "beatsaverplusplus/shared/BeatSaver.hpp"
#include "beatsaverplusplus/shared/Models/Beatmap.hpp"

using namespace GlobalNamespace;

bool inMultiplayerGameplay = false;
bool inSingleplayerGameplay = false;

std::string difficultyToString(GlobalNamespace::BeatmapDifficulty difficulty);

namespace {
// Keep an owned copy of the selected level's display data. BeatmapLevel is a
// Unity object whose native lifetime ends with its scene, so retaining its raw
// pointer for asynchronous HTTP callbacks or later result hooks can dereference
// freed memory and crash the game.
nlohmann::json currentBeatmap = nlohmann::json::object();
uint64_t currentBeatmapGeneration = 0;

// Beat Saber strings are managed IL2CPP objects. Their implicit conversion to
// std::string assumes the managed pointer is non-null and can throw the
// non-standard std::codecvt_base::result enum for malformed UTF-16. Presence
// metadata is optional, so substitute an empty string and record the exact field
// rather than allowing malformed custom-song data to unwind through a game hook.
std::string SafeString(StringW value, std::string_view fieldName) noexcept {
    if (!value) {
        logger.warn("Presence metadata field '{}' was null; using an empty string", fieldName);
        return {};
    }

    try {
        return static_cast<std::string>(value);
    } catch (std::codecvt_base::result result) {
        logger.error("Presence metadata field '{}' contained invalid UTF-16 (conversion result {})",
                     fieldName, static_cast<int>(result));
    } catch (const std::exception& error) {
        logger.error("SafeString failed for presence field '{}': {}", fieldName, error.what());
    } catch (...) {
        logger.error("SafeString failed for presence field '{}' with a non-standard exception", fieldName);
    }
    return {};
}

// ArrayW::size()/begin() dereference the managed array pointer. Check it first,
// then convert each mapper independently so one bad entry cannot discard the
// rest of an otherwise valid song snapshot.
std::vector<std::string> SafeStringArray(ArrayW<StringW> values,
                                         std::string_view fieldName) noexcept {
    std::vector<std::string> result;
    if (!values) {
        logger.warn("Presence metadata array '{}' was null; using an empty list", fieldName);
        return result;
    }

    try {
        result.reserve(values.size());
        for (size_t index = 0; index < values.size(); ++index) {
            result.push_back(SafeString(
                values[index], std::string(fieldName) + "[" + std::to_string(index) + "]"));
        }
    } catch (const std::exception& error) {
        logger.error("SafeStringArray failed for presence field '{}': {}", fieldName, error.what());
    } catch (...) {
        logger.error("SafeStringArray failed for presence field '{}' with a non-standard exception", fieldName);
    }
    return result;
}

// Copy every field while the level passed by Beat Saber is known to be alive.
// Returning false lets the presence feature fail closed without interrupting
// the game's original transition when a nonstandard scene supplies no level.
bool TrySnapshotBeatmap(BeatmapLevel* level, BeatmapDifficulty difficulty,
                        nlohmann::json& snapshot) noexcept {
    if (!level) {
        logger.warn("Presence update skipped because Beat Saber supplied no beatmap level");
        return false;
    }

    try {
        snapshot = nlohmann::json::object();
        snapshot["title"] = SafeString(level->songName, "songName");
        snapshot["author"] = SafeString(level->songAuthorName, "songAuthorName");
        snapshot["duration"] = level->songDuration;
        snapshot["mappers"] = SafeStringArray(level->allMappers, "allMappers");
        snapshot["difficulty"] = difficultyToString(difficulty);
        return true;
    } catch (const std::exception& error) {
        // JSON allocation/construction is the only expected std::exception here.
        // Contain it because this helper is called before Beat Saber's original
        // scene-transition method and must never prevent the map from starting.
        logger.error("TrySnapshotBeatmap failed while copying optional presence metadata: {}", error.what());
    } catch (...) {
        logger.error("TrySnapshotBeatmap failed with a non-standard exception");
    }
    return false;
}

// Build a presence event from the safe metadata snapshot instead of reaching
// back into a Unity object that may already have been destroyed.
bool TryBuildCurrentBeatmapEvent(const char* type, nlohmann::json& event) {
    if (!currentBeatmap.is_object() || currentBeatmap.empty()) {
        logger.warn("{} presence update skipped because no beatmap metadata is available", type);
        return false;
    }

    event = currentBeatmap;
    event["type"] = type;
    return true;
}

// Stop scheduled stat reads as soon as gameplay ends. MetaCore reads live scene
// objects, so leaving either flag set after a quit/result transition can make
// the next timer tick access objects from an unloaded scene.
void LeaveGameplay() {
    inSingleplayerGameplay = false;
    inMultiplayerGameplay = false;
}

// Invalidate outstanding cover-art requests whenever the selected gameplay
// session ends. Without a generation token, a slow response for the previous
// song can overwrite Discord after the user has already moved elsewhere.
void ResetBeatmapSession() {
    LeaveGameplay();
    currentBeatmap = nlohmann::json::object();
    ++currentBeatmapGeneration;
}

// Multiplayer menus are not present in every flow. Walk their optional object
// graph one step at a time rather than relying on one unchecked pointer chain.
std::string GetLobbyCodeIfAvailable() noexcept {
    try {
        auto* mainFlow = BSML::Helpers::GetMainFlowCoordinator();
        if (!mainFlow || !mainFlow->_multiplayerModeSelectionFlowCoordinator) return {};

        auto lobbyFlow = mainFlow->_multiplayerModeSelectionFlowCoordinator->_gameServerLobbyFlowCoordinator;
        if (!lobbyFlow || !lobbyFlow->____unifiedNetworkPlayerModel) return {};
        return SafeString(lobbyFlow->____unifiedNetworkPlayerModel->get_code(), "lobbyCode");
    } catch (const std::exception& error) {
        logger.error("GetLobbyCodeIfAvailable failed safely: {}", error.what());
    } catch (...) {
        logger.error("GetLobbyCodeIfAvailable failed with a non-standard exception");
    }
    return {};
}
}


std::string difficultyToString(GlobalNamespace::BeatmapDifficulty difficulty)
{
    switch (difficulty)
    {
    case GlobalNamespace::BeatmapDifficulty::Easy:
        return "Easy";
    case GlobalNamespace::BeatmapDifficulty::Normal:
        return "Normal";
    case GlobalNamespace::BeatmapDifficulty::Hard:
        return "Hard";
    case GlobalNamespace::BeatmapDifficulty::Expert:
        return "Expert";
    case GlobalNamespace::BeatmapDifficulty::ExpertPlus:
        return "Expert+";
    default:
        return "Unknown";
    }
}


MAKE_HOOK_MATCH(MultiplayerSessionManager_HandlePlayerConnected, &MultiplayerSessionManager::HandlePlayerConnected, void, GlobalNamespace::MultiplayerSessionManager* self, GlobalNamespace::IConnectedPlayer* player) {
    MultiplayerSessionManager_HandlePlayerConnected(self, player);

    // The connection callback can race lobby teardown. The original method has
    // already completed, so skip only our optional presence update when its
    // manager was removed during that transition.
    if (!self || !self->_connectedPlayerManager) {
        logger.warn("Lobby connect presence update skipped because the player manager is unavailable");
        return;
    }

    auto getCount = self->_connectedPlayerManager->connectedPlayerCount;
    auto maxPlayerCount = self->get_maxPlayerCount();
    auto getLocalPlayer = self->get_localPlayer();
    const std::string lobbyCode = GetLobbyCodeIfAvailable();

    if (!inMultiplayerGameplay) {
        if (player == getLocalPlayer) {
            nlohmann::json data;
            data["type"] = "LobbyLocalPlayerOnConnect";
            // Include the same snapshot as remote-player events so Quest mode
            // can enter a complete lobby state even when the local user is the
            // only connected player.
            data["playerCount"] = getCount;
            data["maxPlayerCount"] = maxPlayerCount;
            data["lobbyCode"] = lobbyCode;

            SendPresenceEvent(data);

            return;
        }
        nlohmann::json data;
        data["type"] = "LobbyPlayerOnConnect";
        data["playerCount"] = getCount;
        data["maxPlayerCount"] = maxPlayerCount;
        data["lobbyCode"] = lobbyCode;

        SendPresenceEvent(data);
    }
}

MAKE_HOOK_MATCH(MultiplayerSessionManager_HandlePlayerDisconnected, &MultiplayerSessionManager::HandlePlayerDisconnected, void, GlobalNamespace::MultiplayerSessionManager* self, GlobalNamespace::IConnectedPlayer* player) {
    MultiplayerSessionManager_HandlePlayerDisconnected(self, player);

    // Lobby objects may already be disappearing when a disconnect is reported;
    // never let an optional Discord update dereference that torn-down graph.
    if (!self || !self->_connectedPlayerManager) {
        logger.warn("Lobby disconnect presence update skipped because the player manager is unavailable");
        return;
    }

    auto getCount = self->_connectedPlayerManager->connectedPlayerCount;
    auto maxPlayerCount = self->get_maxPlayerCount();
    auto getLocalPlayer = self->get_localPlayer();
    const std::string lobbyCode = GetLobbyCodeIfAvailable();

    if (!inMultiplayerGameplay) {
        if (player == getLocalPlayer) {
            nlohmann::json data;
            data["type"] = "LobbyLocalPlayerOnDisconnect";
            // Preserve counts for both transports; the Quest state machine can
            // now clear or update its lobby without waiting for another player.
            data["playerCount"] = getCount;
            data["maxPlayerCount"] = maxPlayerCount;
            data["lobbyCode"] = lobbyCode;

            SendPresenceEvent(data);

            return;
        }
        nlohmann::json data;
        data["type"] = "LobbyPlayerOnDisconnect";
        data["playerCount"] = getCount;
        data["maxPlayerCount"] = maxPlayerCount;
        data["lobbyCode"] = lobbyCode;

        SendPresenceEvent(data);
    }
}

MAKE_HOOK_MATCH(LevelCollectionViewController_DidActivate, &GlobalNamespace::LevelCollectionViewController::DidActivate, void, GlobalNamespace::LevelCollectionViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    LevelCollectionViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    // Entering level selection means no gameplay objects should be polled, even
    // when this controller is part of an unusual or rapidly changing flow.
    ResetBeatmapSession();
    if (self && self->get_isActiveAndEnabled()) {
        nlohmann::json data;
        data["type"] = "LevelSelectionMenuInitialized";

        SendPresenceEvent(data);
    }
}

MAKE_HOOK_MATCH(MainFlowCoordinator_DidActivate, &GlobalNamespace::MainFlowCoordinator::DidActivate, void, GlobalNamespace::MainFlowCoordinator* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    // A return to the main flow is another authoritative gameplay-exit signal;
    // clear the flags before the recurring stats callback can run again.
    ResetBeatmapSession();
    nlohmann::json data;
    data["type"] = "MainMenuInitialized";

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(MenuTransitionsHelper_StartStandardLevel,
                static_cast<void (MenuTransitionsHelper::*)(
                    ::StringW,
                    ByRef<BeatmapKey>,
                    BeatmapLevel*,
                    OverrideEnvironmentSettings*,
                    ColorScheme*,
                    bool,
                    ColorScheme*,
                    GameplayModifiers*,
                    PlayerSpecificSettings*,
                    PracticeSettings*,
                    EnvironmentsListModel*,
                    ::StringW,
                    bool,
                    bool,
                    System::Action*,
                    System::Action_1<::Zenject::DiContainer*>*,
                    System::Action_2<::UnityW<StandardLevelScenesTransitionSetupDataSO>,
                                     LevelCompletionResults*>*,
                    System::Action_2<::UnityW<StandardLevelScenesTransitionSetupDataSO>,
                                     LevelCompletionResults*>*,
                    System::Nullable_1<RecordingToolManager_SetupData>
                )>(&MenuTransitionsHelper::StartStandardLevel),
                void,
                MenuTransitionsHelper *self,
                ::StringW gameMode,
                ByRef<BeatmapKey> beatmapKey,
                BeatmapLevel* beatmapLevel,
                OverrideEnvironmentSettings* overrideEnvironmentSettings,
                ColorScheme* overrideColorScheme,
                bool playerOverrideLightshowColors,
                ColorScheme* beatmapOverrideColorScheme,
                GameplayModifiers* gameplayModifiers,
                PlayerSpecificSettings* playerSpecificSettings,
                PracticeSettings* practiceSettings,
                EnvironmentsListModel* environmentsListModel,
                ::StringW backButtonText,
                bool useTestNoteCutSoundEffects,
                bool startPaused,
                ::System::Action* beforeSceneSwitchToGameplayCallback,
                ::System::Action_1<::Zenject::DiContainer*>* afterSceneSwitchToGameplayCallback,
                ::System::Action_2<::UnityW<StandardLevelScenesTransitionSetupDataSO>, LevelCompletionResults*>* levelFinishedCallback,
                ::System::Action_2<::UnityW<StandardLevelScenesTransitionSetupDataSO>, LevelCompletionResults*>* levelRestartedCallback,
                ::System::Nullable_1<RecordingToolManager_SetupData> recordingToolData)
{
    // Snapshot the level before starting the scene transition. The original
    // method may unload menu-owned objects before an asynchronous BeatSaver
    // response completes, so callbacks must capture only standard C++ data.
    nlohmann::json beatmapSnapshot;
    const BeatmapDifficulty difficulty = beatmapKey->difficulty;
    bool hasBeatmap = TrySnapshotBeatmap(beatmapLevel, difficulty, beatmapSnapshot);
    std::string hash;
    uint64_t beatmapGeneration = currentBeatmapGeneration;
    if (hasBeatmap) {
        try {
            // MetaCore accepts an owned std::string. Convert the managed level ID
            // through the same guarded path as display metadata; malformed IDs
            // then behave like OST levels instead of terminating this hook.
            hash = MetaCore::Songs::GetHash(SafeString(beatmapLevel->levelID, "levelID"));
            currentBeatmap = beatmapSnapshot;
            beatmapGeneration = ++currentBeatmapGeneration;
            inSingleplayerGameplay = false;
            inMultiplayerGameplay = false;
        } catch (const std::exception& error) {
            // This block runs before Beat Saber's original transition method.
            // Disable only presence for this attempt so allocation/library
            // failures cannot prevent the map itself from starting.
            logger.error("StartStandardLevel presence preparation failed safely: {}", error.what());
            hasBeatmap = false;
        } catch (...) {
            logger.error("StartStandardLevel presence preparation failed with a non-standard exception");
            hasBeatmap = false;
        }
    }

    MenuTransitionsHelper_StartStandardLevel(
        self,
        gameMode,
        beatmapKey,
        beatmapLevel,
        overrideEnvironmentSettings,
        overrideColorScheme,
        playerOverrideLightshowColors,
        beatmapOverrideColorScheme,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        environmentsListModel,
        backButtonText,
        useTestNoteCutSoundEffects,
        startPaused,
        beforeSceneSwitchToGameplayCallback,
        afterSceneSwitchToGameplayCallback,
        levelFinishedCallback,
        levelRestartedCallback,
        recordingToolData
    );

    // Presence is optional; the game's transition has already run and must not
    // be affected when a modded flow omitted the level metadata.
    if (!hasBeatmap) return;

    logger.info("Level hash: {}", hash);

    // OST levels do not have a BeatSaver hash. Send the locally available level
    // metadata through whichever transport is active instead of issuing an
    // invalid /maps/hash/ request. The Desktop Companion already consumes this
    // standard BeatmapInitialized shape; without it, the companion leaves
    // inBeatmap false, ignores every later stat update, and remains in Main Menu.
    if (hash.empty()) {
        nlohmann::json data = beatmapSnapshot;
        data["type"] = "BeatmapInitialized";
        data["coverURL"] = nullptr;

        logger.info("Using local metadata for an OST level in {} mode",
                    getConfig().UseQuestDiscord.GetValue()
                        ? "Quest Discord"
                        : "Desktop Companion");
        SendPresenceEvent(data);
        return;
    }

    const bool questInitialEventSent = getConfig().UseQuestDiscord.GetValue();
    if (questInitialEventSent) {
        // Publish local metadata immediately for custom songs as well. The
        // BeatSaver request below becomes an optional cover-art enhancement,
        // so an offline API cannot suppress the entire playing presence.
        nlohmann::json data = beatmapSnapshot;
        data["type"] = "BeatmapInitialized";
        data["coverURL"] = nullptr;
        SendPresenceEvent(data);
    }

    // A failed asynchronous request stores its error in the future. Handle that
    // error at this boundary so it cannot escape a scheduler callback and call
    // std::terminate on Unity's main thread.
    try {
        std::shared_future<BeatSaver::API::BeatmapResponse> beatmapFuture =
            BeatSaver::API::GetBeatmapByHashAsync(hash, [](float progress) {});

        BSML::MainThreadScheduler::AwaitFuture<BeatSaver::API::BeatmapResponse>(
            beatmapFuture,
            [beatmapFuture, beatmapSnapshot, beatmapGeneration, questInitialEventSent]() -> void {
                try {
                    const auto& response = beatmapFuture.get();

                    // Ignore stale responses rather than publishing a cover for
                    // a song whose gameplay session has already ended.
                    if (beatmapGeneration != currentBeatmapGeneration) return;
                    if (!response.IsSuccessful()) {
                        logger.warn("BeatSaver metadata request failed; presence will keep local song data");
                        return;
                    }

                    const auto beatmap = response.GetParsedData();
                    const auto& versions = beatmap.GetVersions();
                    if (versions.empty()) {
                        logger.warn("BeatSaver metadata contained no versions; presence will keep local song data");
                        return;
                    }

                    const std::string coverURL = versions.front().GetCoverURL();
                    nlohmann::json data = beatmapSnapshot;
                    // Quest mode already published BeatmapInitialized before the
                    // network request. A dedicated cover-only event prevents this
                    // late response from restarting the timer, erasing stats, or
                    // changing Paused back to Playing. Desktop mode still receives
                    // exactly the BeatmapInitialized event expected by the
                    // unmodified PC companion. If the user changed modes while the
                    // request was running, use BeatmapInitialized so the newly
                    // selected transport receives a complete song state.
                    const bool stillUsingQuest = getConfig().UseQuestDiscord.GetValue();
                    data["type"] = questInitialEventSent && stillUsingQuest
                        ? "BeatmapCoverResolved"
                        : "BeatmapInitialized";
                    data["coverURL"] = !coverURL.empty() ? nlohmann::json(coverURL) : nlohmann::json(nullptr);

                    // Retain the resolved cover in the owned snapshot so pause,
                    // failure, and result events reuse it without touching Unity.
                    currentBeatmap = data;
                    currentBeatmap.erase("type");
                    SendPresenceEvent(data);
                } catch (const std::exception& error) {
                    logger.error("StartStandardLevel BeatSaver completion failed safely: {}", error.what());
                } catch (...) {
                    // Some third-party futures may propagate a non-standard
                    // exception; contain it at the scheduler boundary.
                    logger.error("StartStandardLevel BeatSaver completion failed with a non-standard exception");
                }
            }
        );
    } catch (const std::exception& error) {
        logger.error("StartStandardLevel could not start the BeatSaver metadata request: {}", error.what());
    } catch (...) {
        // Starting an async request is optional and must never block gameplay.
        logger.error("StartStandardLevel could not start the BeatSaver metadata request due to a non-standard exception");
    }

}

MAKE_HOOK_MATCH(AudioTimeSyncController_StartSong, &AudioTimeSyncController::StartSong,
                void, AudioTimeSyncController* self, float_t startTimeOffset) {
    AudioTimeSyncController_StartSong(self, startTimeOffset);

    // Start polling only after the song controller confirms playback and only
    // when a valid level snapshot exists; tutorials and unusual scenes may call
    // StartSong without the standard level-selection path.
    if (!inMultiplayerGameplay && !currentBeatmap.empty()) {
        inSingleplayerGameplay = true;
    }
}

MAKE_HOOK_MATCH(SongStartSyncController_StartSong, &SongStartSyncController::StartSong, void, SongStartSyncController *self, PlayersSpecificSettingsAtGameStartModel* playersSpecificSettingsAtGameStartModel, ::StringW sessionGameId) {
    SongStartSyncController_StartSong(self, playersSpecificSettingsAtGameStartModel, sessionGameId);

    // Multiplayer scene setup can be cancelled after its transition starts.
    // Do not dereference either the controller graph or stale level metadata if
    // that happened; Discord presence must remain optional.
    if (!self || !self->_multiplayerSessionManager || currentBeatmap.empty()) {
        logger.warn("Multiplayer start presence skipped because its scene state is incomplete");
        LeaveGameplay();
        return;
    }

    auto* sessionManager = self->_multiplayerSessionManager;
    inMultiplayerGameplay = true;
    inSingleplayerGameplay = false;

    nlohmann::json data = currentBeatmap;

    if (sessionManager->isSpectating) {
        data["type"] = "SpectateInitialized";
    } else {
        data["type"] = "MultiplayerBeatmapInitialized";
    }

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(MenuTransitionsHelper_StartMultiplayerLevel, static_cast<
                    void (MenuTransitionsHelper::*)
                    (
                        ::StringW,
                        ByRef<BeatmapKey>,
                        BeatmapLevel*,
                        IBeatmapLevelData*,
                        ColorScheme*,
                        GameplayModifiers*,
                        PlayerSpecificSettings*,
                        PracticeSettings*,
                        ::StringW,
                        bool,
                        ::System::Action*,
                        ::System::Action_1<::Zenject::DiContainer*>*,
                        ::System::Action_2<::UnityW<MultiplayerLevelScenesTransitionSetupDataSO>, MultiplayerResultsData*>*,
                        ::System::Action_1<DisconnectedReason>*
                    )
                >(&MenuTransitionsHelper::StartMultiplayerLevel), void,
                MenuTransitionsHelper *self,
                ::StringW gameMode,
                ByRef<BeatmapKey> beatmapKey,
                BeatmapLevel* beatmapLevel,
                IBeatmapLevelData* beatmapLevelData,
                ColorScheme* overrideColorScheme,
                GameplayModifiers* gameplayModifiers,
                PlayerSpecificSettings* playerSpecificSettings,
                PracticeSettings* practiceSettings,
                ::StringW backButtonText,
                bool useTestNoteCutSoundEffects,
                ::System::Action* beforeSceneSwitchCallback,
                ::System::Action_1<::Zenject::DiContainer*>* afterSceneSwitchCallback,
                ::System::Action_2<::UnityW<MultiplayerLevelScenesTransitionSetupDataSO>, MultiplayerResultsData*>* levelFinishedCallback,
                ::System::Action_1<DisconnectedReason>* didDisconnectCallback)
{
    // Capture multiplayer metadata before the original transition can release
    // the lobby's BeatmapLevel object. Later gameplay hooks use only this copy.
    nlohmann::json beatmapSnapshot;
    bool hasBeatmap = TrySnapshotBeatmap(
        beatmapLevel, beatmapKey->difficulty, beatmapSnapshot);
    if (hasBeatmap) {
        try {
            currentBeatmap = std::move(beatmapSnapshot);
            ++currentBeatmapGeneration;
        } catch (const std::exception& error) {
            // Match the solo transition guarantee: optional presence state must
            // not be able to block multiplayer scene loading.
            logger.error("StartMultiplayerLevel presence preparation failed safely: {}", error.what());
            hasBeatmap = false;
        } catch (...) {
            logger.error("StartMultiplayerLevel presence preparation failed with a non-standard exception");
            hasBeatmap = false;
        }
    }
    if (!hasBeatmap) {
        ResetBeatmapSession();
    }

    MenuTransitionsHelper_StartMultiplayerLevel(
        self,
        gameMode,
        beatmapKey,
        beatmapLevel,
        beatmapLevelData,
        overrideColorScheme,
        gameplayModifiers,
        playerSpecificSettings,
        practiceSettings,
        backButtonText,
        useTestNoteCutSoundEffects,
        beforeSceneSwitchCallback,
        afterSceneSwitchCallback,
        levelFinishedCallback,
        didDisconnectCallback);
}

MAKE_HOOK_MATCH(PauseController_Pause, &PauseController::Pause, void, PauseController *self) {
    PauseController_Pause(self);

    nlohmann::json data;
    data["type"] = "BeatmapPaused";

    // Pause both possible modes so the stats scheduler never reads gameplay
    // objects while Beat Saber has suspended their updates.
    LeaveGameplay();

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidPressContinueButton, &PauseController::HandlePauseMenuManagerDidPressContinueButton, void, PauseController *self) {
    PauseController_HandlePauseMenuManagerDidPressContinueButton(self);

    nlohmann::json data;
    data["type"] = "BeatmapResumed";

    // Resume only the mode represented by the stored presence state. The
    // single-player pause hook is not a reliable signal for multiplayer state.
    if (!currentBeatmap.empty()) inSingleplayerGameplay = true;

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidPressRestartButton, &PauseController::HandlePauseMenuManagerDidPressRestartButton, void, PauseController *self) {
    PauseController_HandlePauseMenuManagerDidPressRestartButton(self);

    nlohmann::json data;
    if (!TryBuildCurrentBeatmapEvent("BeatmapRestarted", data)) return;

    // Restart occurs only after the gameplay scene is live again, so polling can
    // safely resume once a valid owned level snapshot has been confirmed.
    inSingleplayerGameplay = true;
    inMultiplayerGameplay = false;

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(PauseMenuManager_MenuButtonPressed, &PauseMenuManager::MenuButtonPressed, void, PauseMenuManager *self) {
    PauseMenuManager_MenuButtonPressed(self);
    // Quitting through the pause menu bypasses the results hooks. Explicitly
    // invalidate the session so the next timed stat read cannot hit dead Unity
    // gameplay objects and late cover-art callbacks cannot publish stale data.
    ResetBeatmapSession();
}

MAKE_HOOK_MATCH(MultiplayerLocalActivePlayerGameplayAnimator_TransitionIntoFailedState, &MultiplayerLocalActivePlayerGameplayAnimator::TransitionIntoFailedState, void, MultiplayerLocalActivePlayerGameplayAnimator *self) {
    MultiplayerLocalActivePlayerGameplayAnimator_TransitionIntoFailedState(self);

    nlohmann::json data;
    if (!TryBuildCurrentBeatmapEvent("SpectateInitialized", data)) return;

    SendPresenceEvent(data);
}

MAKE_HOOK_MATCH(MultiplayerResultsViewController_DidActivate, &MultiplayerResultsViewController::DidActivate, void, MultiplayerResultsViewController *self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    MultiplayerResultsViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    // Results are an authoritative end to multiplayer gameplay, so stop live
    // stats immediately before sending the final state.
    LeaveGameplay();

    nlohmann::json data;
    data["type"] = "MultiplayerBeatmapFinished";

    SendPresenceEvent(data);
    // Prevent a late BeatSaver completion from replacing the final result.
    ++currentBeatmapGeneration;
}

MAKE_HOOK_MATCH(ResultsViewController_DidActivate, &ResultsViewController::DidActivate, void, ResultsViewController *self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    ResultsViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    // ResultsViewController can briefly activate without populated result data
    // in cancelled or modded flows. The old code dereferenced results before its
    // null check, producing a direct native crash.
    if (!self || !self->_levelCompletionResults) {
        logger.warn("Results presence skipped because completion results are unavailable");
        LeaveGameplay();
        return;
    }

    auto* results = self->_levelCompletionResults;
    LeaveGameplay();

    if (results->levelEndStateType == LevelCompletionResults::LevelEndStateType::Failed) {

        nlohmann::json data;
        data["type"] = "BeatmapFailed";

        SendPresenceEvent(data);
    }

    if (results->levelEndStateType == LevelCompletionResults::LevelEndStateType::Cleared) {
        nlohmann::json data;
        if (!currentBeatmap.is_object() || currentBeatmap.empty()) {
            // The owned snapshot is the normal path and keeps today's Desktop
            // payload unchanged. In an unusual flow that cleared it early, the
            // ResultsViewController still owns a live BeatmapLevel for the
            // duration of this callback. Use that only as a fallback so the PC
            // companion does not silently miss BeatmapCleared as it could in the
            // reviewed code.
            if (!self->____beatmapLevel ||
                !TrySnapshotBeatmap(self->____beatmapLevel,
                                    self->____beatmapKey.difficulty, data)) {
                logger.warn("BeatmapCleared presence skipped because neither snapshot nor results metadata is available");
                ++currentBeatmapGeneration;
                return;
            }
            data["type"] = "BeatmapCleared";
            logger.info("BeatmapCleared used ResultsViewController metadata because the gameplay snapshot was unavailable");
        } else {
            data = currentBeatmap;
            data["type"] = "BeatmapCleared";
        }

        SendPresenceEvent(data);
    }

    // Results own the final presence state; any outstanding cover lookup now
    // belongs to an ended session and must be ignored.
    ++currentBeatmapGeneration;
}

MAKE_HOOK_MATCH(ResultsViewController_ContinueButtonPressed, &ResultsViewController::ContinueButtonPressed, void, ResultsViewController* self) {
    ResultsViewController_ContinueButtonPressed(self);

    // Continuing releases the results scene and its level objects; discard the
    // snapshot and invalidate any outstanding metadata callback.
    ResetBeatmapSession();
    nlohmann::json data;
    data["type"] = "LevelSelectionMenuInitialized";

    SendPresenceEvent(data);
}

// Store the mod ID and version, so it can be sent to the modloader at startup
static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// Called in the early stages of game loading
// (see https://github.com/sc2ad/scotland2?tab=readme-ov-file#installationusage)
// Often used to initialize and load configs, in addition to its contents here
extern "C" EXPORT void setup(CModInfo* info) noexcept {
    // Scotland2 normally guarantees this pointer, but setup is a C ABI boundary;
    // reject invalid loader input rather than dereferencing address zero.
    if (!info) return;
    *info = modInfo.to_c();

    // Register our logger so that all its messages are stored in a file
    Paper::Logger::RegisterFileContextId(MOD_ID);
    BSML::Init();

    getConfig().Init(modInfo);
    const bool settingsRegistered = BSML::Register::RegisterSettingsMenu(
        "Discord Rich Presence", DidActivate, true);
    logger.info("Discord Rich Presence settings menu registration: {}",
        settingsRegistered ? "success" : "already registered");

    BSML::Register::RegisterMainMenuViewControllerMethod(
        "Discord Rich Presence",
        "Discord Presence",
        "Configure Desktop Companion or native Quest Discord presence",
        DidActivate);
    logger.info("Registered Discord Presence button in the Mods menu");


    logger.info("Completed setup!");
}

// Called later on in the game loading, after all mods have been opened
// Often used to install hooks and use the APIs of other mods or libraries
extern "C" EXPORT void late_load() noexcept {
    il2cpp_functions::Init();
    // Quest IPC is optional startup work. Contain class-loader/thread failures
    // at this noexcept modloader boundary so the remaining hooks still load.
    try {
        if (getConfig().UseQuestDiscord.GetValue() && !QuestDiscord::Initialize()) {
            logger.warn("Quest Discord will retry its RPC connection on the next presence update");
        }
    } catch (const std::exception& error) {
        logger.error("late_load Quest Discord initialization failed safely: {}", error.what());
    } catch (...) {
        logger.error("late_load Quest Discord initialization failed with a non-standard exception");
    }
    logger.info("Installing hooks");
    InstallUIHooks();
    INSTALL_HOOK(logger, AudioTimeSyncController_StartSong);
    INSTALL_HOOK(logger, SongStartSyncController_StartSong);
    INSTALL_HOOK(logger, PauseMenuManager_MenuButtonPressed);
    INSTALL_HOOK(logger, LevelCollectionViewController_DidActivate);
    INSTALL_HOOK(logger, MultiplayerSessionManager_HandlePlayerConnected);
    INSTALL_HOOK(logger, MultiplayerLocalActivePlayerGameplayAnimator_TransitionIntoFailedState);
    INSTALL_HOOK(logger, MultiplayerResultsViewController_DidActivate);
    INSTALL_HOOK(logger, MultiplayerSessionManager_HandlePlayerDisconnected);
    INSTALL_HOOK(logger, MenuTransitionsHelper_StartStandardLevel);
    INSTALL_HOOK(logger, MenuTransitionsHelper_StartMultiplayerLevel);
    INSTALL_HOOK(logger, PauseController_Pause);
    INSTALL_HOOK(logger, PauseController_HandlePauseMenuManagerDidPressContinueButton);
    INSTALL_HOOK(logger, PauseController_HandlePauseMenuManagerDidPressRestartButton);
    INSTALL_HOOK(logger, MainFlowCoordinator_DidActivate);
    INSTALL_HOOK(logger, ResultsViewController_DidActivate);
    INSTALL_HOOK(logger, ResultsViewController_ContinueButtonPressed);
    MetaCore::Engine::ScheduleMainThread(Heartbeat);
    MetaCore::Engine::ScheduleMainThread(StatUpdate);
    try {
        const bool questMode = getConfig().UseQuestDiscord.GetValue();
        const std::string connectionStatus = questMode
            ? QuestDiscord::GetConnectionStatus()
            : "not requested (Desktop Companion mode)";
        // One compact line gives support logs the configuration that most often
        // distinguishes a packaging issue from a Discord binding failure.
        logger.info("Discord Rich Presence startup: version={}, mode={}, Quest update={}s, helper={}, status={}",
                    VERSION,
                    questMode ? "Quest Discord" : "Desktop Companion",
                    GetQuestDiscordUpdateIntervalSeconds(),
                    QuestDiscord::IsHelperAvailable() ? "present" : "missing",
                    connectionStatus);
    } catch (const std::exception& error) {
        // Diagnostics are optional and must not prevent the mod from loading.
        logger.error("late_load startup diagnostics failed safely: {}", error.what());
    } catch (...) {
        logger.error("late_load startup diagnostics failed with a non-standard exception");
    }
    logger.info("Completed load!");
}
