// Include our main header file, containing the logger
#include "main.hpp"
#include "nlohmann/json.hpp"
#include "config.hpp"
#include "UI/ui.hpp"

#include "config.h"

#include "nlohmann/json_fwd.hpp"
#include "web-utils/shared/WebUtils.hpp"

#include "bsml/shared/BSML.hpp"

#include <span>
#include <cstdint>

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

#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/SongStartSyncController.hpp"
#include "GlobalNamespace/MultiplayerResultsViewController.hpp"
#include "GlobalNamespace/GameServerLobbyFlowCoordinator.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelSelectionNavigationController.hpp"
#include "GlobalNamespace/ResultsViewController.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"

using namespace GlobalNamespace;

bool skipNextActivation = false;
bool inGameplay = false;
::GlobalNamespace::BeatmapLevel* getBeatmapLevel;
BeatmapDifficulty getDifficulty;

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

void CreateRequest(std::string jsonStr) {
    std::thread([jsonStr] {
        const std::string getIp = getConfig().PCIPSetting.GetValue();
        const std::string getPort = getConfig().PortSetting.GetValue();

        const std::string URL = "http://" + getIp + ":" + getPort + "/sendData";

        WebUtils::URLOptions path{ URL };
        path.noEscape = true;
        
        std::span<const uint8_t> body(
            reinterpret_cast<const uint8_t*>(jsonStr.data()),
            jsonStr.size()
        );

        auto response = WebUtils::PostAsync<WebUtils::StringResponse>(path, body);

        response.wait();

        auto responseValue = response.get();

        logger.info(
            "Attempted to send post request to {} with result of status code {}, and curl status being {}",
            path.fullURl(), 
            std::to_string(responseValue.get_HttpCode()), 
            std::to_string(responseValue.get_CurlStatus())
        );

        bool success = responseValue.IsSuccessful();
        if (!success) {
            logger.debug("Failed to get response");
            return;
        }
    }).detach();
}

MAKE_HOOK_MATCH(MultiplayerSessionManager_HandlePlayerConnected, &MultiplayerSessionManager::HandlePlayerConnected, void, GlobalNamespace::MultiplayerSessionManager* self, GlobalNamespace::IConnectedPlayer* player) {
    MultiplayerSessionManager_HandlePlayerConnected(self, player);

    auto getCount = self->get_connectedPlayerCount();

    if (!inGameplay) {
        nlohmann::json data;
        data["type"] = "LobbyPlayerOnConnect";
        data["playerCount"] = getCount + 1;

        std::string jsonStr = data.dump();
        CreateRequest(jsonStr);
    }
}

MAKE_HOOK_MATCH(MultiplayerSessionManager_HandlePlayerDisconnected, &MultiplayerSessionManager::HandlePlayerDisconnected, void, GlobalNamespace::MultiplayerSessionManager* self, GlobalNamespace::IConnectedPlayer* player) {
    MultiplayerSessionManager_HandlePlayerDisconnected(self, player);

    auto getCount = self->get_connectedPlayerCount();

    if (!inGameplay) {
        nlohmann::json data;
        data["type"] = "LobbyPlayerOnDisonnect";
        data["playerCount"] = getCount + 1;

        std::string jsonStr = data.dump();
        CreateRequest(jsonStr);
    }
}

MAKE_HOOK_MATCH(LevelCollectionViewController_DidActivate, &GlobalNamespace::LevelCollectionViewController::DidActivate, void, GlobalNamespace::LevelCollectionViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    LevelCollectionViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    if (skipNextActivation) {
        skipNextActivation = false;
        return;
    }

    if (!screenSystemEnabling) return;

    nlohmann::json data;
    data["type"] = "LevelSelectionMenuInitialized";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(MainFlowCoordinator_DidActivate, &GlobalNamespace::MainFlowCoordinator::DidActivate, void, GlobalNamespace::MainFlowCoordinator* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    nlohmann::json data;
    data["type"] = "MainMenuInitialized";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
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

    BeatmapLevel *level = self->____standardLevelScenesTransitionSetupData->get_beatmapLevel();
    BeatmapDifficulty difficulty = beatmapKey->difficulty;

    if (!level) return;

    getBeatmapLevel = level;
    getDifficulty = difficulty;
    skipNextActivation = true;

    nlohmann::json data;
    data["type"] = "BeatmapInitialized";
    data["title"] = level->songName;
    data["author"] = level->songAuthorName;
    data["duration"] = level->songDuration;
    data["mappers"] = level->allMappers;
    data["difficulty"] = difficultyToString(difficulty);

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(SongStartSyncController_StartSong, &SongStartSyncController::StartSong, void, SongStartSyncController *self, PlayersSpecificSettingsAtGameStartModel* playersSpecificSettingsAtGameStartModel, ::StringW sessionGameId) {
    SongStartSyncController_StartSong(self, playersSpecificSettingsAtGameStartModel, sessionGameId);
    auto sessionManager = self->_multiplayerSessionManager;

    logger.debug("SongStartSyncController_StartSong called, session manager is null: {}", sessionManager == nullptr);

    inGameplay = true;

    nlohmann::json data;
    data["title"] = getBeatmapLevel->songName;
    data["author"] = getBeatmapLevel->songAuthorName;
    data["duration"] = getBeatmapLevel->songDuration;
    data["mappers"] = getBeatmapLevel->allMappers;
    data["difficulty"] = difficultyToString(getDifficulty);

    if (sessionManager->isSpectating) {
        data["type"] = "SpectateInitialized";
    } else {
        data["type"] = "MultiplayerBeatmapInitialized";
    }

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
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

    getBeatmapLevel = beatmapLevel;
    getDifficulty = beatmapKey->difficulty;
}

MAKE_HOOK_MATCH(PauseController_Pause, &PauseController::Pause, void, PauseController *self) {
    PauseController_Pause(self);

    nlohmann::json data;
    data["type"] = "BeatmapPaused";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(PauseController_HandlePauseMenuManagerDidPressContinueButton, &PauseController::HandlePauseMenuManagerDidPressContinueButton, void, PauseController *self) {
    PauseController_HandlePauseMenuManagerDidPressContinueButton(self);

    nlohmann::json data;
    data["type"] = "BeatmapResumed";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(PauseMenuManager_MenuButtonPressed, &PauseMenuManager::MenuButtonPressed, void, PauseMenuManager *self) {
    PauseMenuManager_MenuButtonPressed(self);
    skipNextActivation = false;
}

MAKE_HOOK_MATCH(StandardLevelGameplayManager_HandleGameEnergyDidReach0, &StandardLevelGameplayManager::HandleGameEnergyDidReach0, void, StandardLevelGameplayManager *self) {
    StandardLevelGameplayManager_HandleGameEnergyDidReach0(self);

    nlohmann::json data;
    data["type"] = "BeatmapFailed";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(MultiplayerLocalActivePlayerGameplayAnimator_TransitionIntoFailedState, &MultiplayerLocalActivePlayerGameplayAnimator::TransitionIntoFailedState, void, MultiplayerLocalActivePlayerGameplayAnimator *self) {
    MultiplayerLocalActivePlayerGameplayAnimator_TransitionIntoFailedState(self);

    nlohmann::json data;
    data["type"] = "SpectateInitialized";
    data["title"] = getBeatmapLevel->songName;
    data["author"] = getBeatmapLevel->songAuthorName;
    data["duration"] = getBeatmapLevel->songDuration;
    data["mappers"] = getBeatmapLevel->allMappers;
    data["difficulty"] = difficultyToString(getDifficulty);

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(MultiplayerResultsViewController_DidActivate, &MultiplayerResultsViewController::DidActivate, void, MultiplayerResultsViewController *self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    MultiplayerResultsViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    inGameplay = false;

    nlohmann::json data;
    data["type"] = "MultiplayerBeatmapFinished";

    std::string jsonStr = data.dump();
    CreateRequest(jsonStr);
}

MAKE_HOOK_MATCH(ResultsViewController_DidActivate, &ResultsViewController::DidActivate, void, ResultsViewController *self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    ResultsViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);

    auto results = self->_levelCompletionResults;
    auto level = self->____beatmapLevel;
    auto difficulty = self->____beatmapKey.difficulty;

    if (results && results->levelEndStateType == LevelCompletionResults::LevelEndStateType::Cleared) {
        nlohmann::json data;
        data["type"] = "BeatmapCleared";
        data["title"] = level->songName;
        data["author"] = level->songAuthorName;
        data["duration"] = level->songDuration;
        data["mappers"] = level->allMappers;
        data["difficulty"] = difficultyToString(difficulty);

        std::string jsonStr = data.dump();
        CreateRequest(jsonStr);
    }
}

// Store the mod ID and version, so it can be sent to the modloader at startup
static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

// Called in the early stages of game loading
// (see https://github.com/sc2ad/scotland2?tab=readme-ov-file#installationusage)
// Often used to initialize and load configs, in addition to its contents here
extern "C" EXPORT void setup(CModInfo* info) noexcept {
    *info = modInfo.to_c();

    // Register our logger so that all its messages are stored in a file
    Paper::Logger::RegisterFileContextId(MOD_ID);
    BSML::Init();

    getConfig().Init(modInfo);
    BSML::Register::RegisterSettingsMenu("DRP", DidActivate, true);
    

    logger.info("Completed setup!");
}

// Called later on in the game loading, after all mods have been opened
// Often used to install hooks and use the APIs of other mods or libraries
extern "C" EXPORT void late_load() noexcept {
    il2cpp_functions::Init();
    logger.info("Installing hooks");
    InstallUIHooks();
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
    INSTALL_HOOK(logger, MainFlowCoordinator_DidActivate);
    INSTALL_HOOK(logger, StandardLevelGameplayManager_HandleGameEnergyDidReach0);
    INSTALL_HOOK(logger, ResultsViewController_DidActivate);
    logger.info("Completed load!");
}
