#include "ui.hpp"
#include "main.hpp"
#include "../config.hpp"
#include "bsml/shared/BSML.hpp"
#include "UnityEngine/Application.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/MultiplayerLobbyConnectionController.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "web-utils/shared/WebUtils.hpp"

#include "../src/utils/Requests/requests.hpp"

using namespace GlobalNamespace;

void DidActivate(HMUI::ViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    if (!firstActivation)
        return;

    auto container = BSML::Lite::CreateScrollableSettingsContainer(self);

    AddConfigValueInputString(container, getConfig().PCIPSetting);
    AddConfigValueInputString(container, getConfig().PortSetting);


    BSML::Lite::CreateUIButton(container, "Open Instructions", []() {
        UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#-quick-start");
    });
}

MAKE_HOOK_MATCH(MainMenuViewController_DidActivate, &MainMenuViewController::DidActivate, void, MainMenuViewController* self, bool firstActivation, bool addedToHierachy, bool screenSystemEnabling) {
    MainMenuViewController_DidActivate(self, firstActivation, addedToHierachy, screenSystemEnabling);

    if (firstActivation && getConfig().FirstTime.GetValue()) {
        auto modal = BSML::Lite::CreateModal(self->transform, {100, 60}, []() {});

        auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);
    
        auto text = BSML::Lite::CreateText(verticalLayout, "Thank you for installing the mod! To setup your Discord RPC, please\nlook through the instructions by pressing \"Open Instructions\". ");
        text->set_enableWordWrapping(true);
        text->set_alignment(TMPro::TextAlignmentOptions::Center);

        auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);

        BSML::Lite::CreateUIButton(horizontalLayout, "Open Instructions", []() {
            UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#-quick-start");
        });
        BSML::Lite::CreateUIButton(horizontalLayout, "Close", [modal]() {
            getConfig().FirstTime.SetValue(false);
            modal->Hide();
        });

        modal->Show();

        return;
    }

    std::shared_future<WebUtils::JsonResponse> future = CreateRequest("GET", "/version", {});

    BSML::MainThreadScheduler::AwaitFuture<WebUtils::JsonResponse>(
        future,
        [future, self, firstActivation]() -> void
        {
            auto& result = future.get();

            if (result.IsSuccessful() && firstActivation) {
                std::string version = result.GetParsedData()["version"].GetString();

                std::shared_future<WebUtils::JsonResponse> githubFutureData = GetLatestGithub();

                BSML::MainThreadScheduler::AwaitFuture<WebUtils::JsonResponse>(
                    githubFutureData,
                    [githubFutureData, self, version]() -> void {
                        auto& result = githubFutureData.get();

                        std::string latestVersion = result.GetParsedData()["tag_name"].GetString();

                        if (version != latestVersion) {
                            auto modal = BSML::Lite::CreateModal(self->transform, {100, 40}, []() {});

                            auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);

                            auto text = BSML::Lite::CreateText(verticalLayout, "It seems like you're using an outdated version of the local server.\nPlease update it to latest whenever you can.");
    
                            text->set_enableWordWrapping(true);
                            text->set_alignment(TMPro::TextAlignmentOptions::Center);

                            auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);

                            BSML::Lite::CreateUIButton(horizontalLayout, "Update", [modal]() {
                                CreateRequest("POST", "/update", {});
                                if (UnityW(modal) != nullptr) {
                                    modal->Hide();
                                }
                            });
                            BSML::Lite::CreateUIButton(horizontalLayout, "Close", [modal]() {
                                if (UnityW(modal) != nullptr) {
                                    modal->Hide();
                                }
                            });

                            if (UnityW(modal) != nullptr) {
                                modal->Show();
                            }

                            return;
                        }

                });
            } else {
                logger.debug("Failed to retrieve version.");

                if (!firstActivation) {
                    return;
                }

                auto modal = BSML::Lite::CreateModal(self->transform, {105, 40}, []() {});

                auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);

                auto text = BSML::Lite::CreateText(verticalLayout, "Your local server could not be updated due to not being able to connect.\nPlease open the instructions to download the new version of the local server");
    
                text->set_enableWordWrapping(true);
                text->set_alignment(TMPro::TextAlignmentOptions::Center);

                auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);

                BSML::Lite::CreateUIButton(horizontalLayout, "Open Instructions", []() {
                    UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#2-install-local-server");
                });
                BSML::Lite::CreateUIButton(horizontalLayout, "Close", [modal]() {
                    modal->Hide();
                });

                modal->Show();

                return;
            }
        });
}

void InstallUIHooks() {
    INSTALL_HOOK(logger, MainMenuViewController_DidActivate);
}