#include "ui.hpp"
#include "main.hpp"
#include "../config.hpp"
#include "bsml/shared/BSML.hpp"
#include "UnityEngine/Application.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/MultiplayerLobbyConnectionController.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "web-utils/shared/WebUtils.hpp"

#include "../src/utils/Requests/requests.hpp"
#include "../src/utils/QuestDiscord/quest_discord.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <vector>
#include <exception>

using namespace GlobalNamespace;

namespace {
    // The Mods settings screen is approximately 116 units wide. Leave a small
    // margin for its mask and scrollbar while allowing labels to use nearly
    // the full panel instead of the stock prefab's narrower 90-unit geometry.
    constexpr float SettingsPanelRowWidth = 112.0f;
    constexpr float SettingsPanelToggleHeight = 8.0f;

    void ReserveTextRow(TMPro::TextMeshProUGUI* text, float height) {
        if (!text || !text->get_gameObject()) return;

        auto* object = text->get_gameObject().ptr();
        auto* layout = object->GetComponent<UnityEngine::UI::LayoutElement*>();
        if (!layout) layout = object->AddComponent<UnityEngine::UI::LayoutElement*>();
        if (!layout) {
            logger.error("Discord settings UI could not reserve a row for a text block");
            return;
        }

        // BSML Lite text objects do not always include a LayoutElement. In a
        // scrollable VerticalLayoutGroup, a text object without one can report
        // zero preferred height, causing the following text/control to occupy
        // the same row. Reserve real vertical space instead of manually moving
        // either object, so rebuilding the layout remains deterministic.
        layout->set_minHeight(height);
        layout->set_preferredHeight(height);
        layout->set_flexibleHeight(0.0f);
    }

    BSML::ToggleSetting* FitToggleToSettingsPanel(BSML::ToggleSetting* toggle) {
        if (!toggle || !toggle->get_gameObject()) return toggle;

        auto* object = toggle->get_gameObject().ptr();
        auto* layout = object->GetComponent<UnityEngine::UI::LayoutElement*>();
        if (!layout) layout = object->AddComponent<UnityEngine::UI::LayoutElement*>();
        if (layout) {
            // BSML copies Beat Saber's 90-unit fullscreen settings prefab.
            // The Mods settings panel is narrower, so retaining that width
            // pushes most of the caption behind the panel mask. Give every
            // toggle one panel-sized row with a stable height instead.
            layout->set_minWidth(SettingsPanelRowWidth);
            layout->set_preferredWidth(SettingsPanelRowWidth);
            layout->set_flexibleWidth(0.0f);
            layout->set_minHeight(SettingsPanelToggleHeight);
            layout->set_preferredHeight(SettingsPanelToggleHeight);
        }

        auto root = object->get_transform().cast<UnityEngine::RectTransform>();
        auto switchTransform = root->Find("SwitchView");
        auto* switchRect = switchTransform
            ? switchTransform->get_gameObject()->GetComponent<UnityEngine::RectTransform*>()
            : nullptr;
        constexpr float switchWidth = 10.0f;

        if (auto nameTransform = root->Find("NameText")) {
            auto nameRect = nameTransform.cast<UnityEngine::RectTransform>();
            // The caption owns the row up to the switch. This is deliberately
            // anchor-based so all privacy labels remain separate from their
            // switches as the scroll view lays out or rebuilds its contents.
            nameRect->set_anchorMin({0.0f, 0.0f});
            nameRect->set_anchorMax({1.0f, 1.0f});
            nameRect->set_pivot({0.5f, 0.5f});
            nameRect->set_offsetMin({0.5f, 0.0f});
            nameRect->set_offsetMax({-(switchWidth + 1.5f), 0.0f});
            if (toggle->text) {
                toggle->text->set_alignment(TMPro::TextAlignmentOptions::MidlineLeft);
                toggle->text->set_enableWordWrapping(false);
                toggle->text->set_overflowMode(TMPro::TextOverflowModes::Ellipsis);
                toggle->text->set_fontSize(3.0f);
            }
        }

        if (switchRect) {
            // Pin the native switch to the right edge of its own row. Do not
            // replace it: keeping BSML's switch preserves focus, sound, hover,
            // and controller interaction behavior.
            switchRect->set_anchorMin({1.0f, 0.5f});
            switchRect->set_anchorMax({1.0f, 0.5f});
            switchRect->set_pivot({1.0f, 0.5f});
            switchRect->set_anchoredPosition({-0.5f, 0.0f});
            const auto currentSize = switchRect->get_sizeDelta();
            switchRect->set_sizeDelta({switchWidth, currentSize.y});
        }

        return toggle;
    }

    // Use complete display labels as the dropdown choices so BSML owns both
    // the visible setting label and selector layout. This avoids the manually
    // positioned label/control pairs that previously stacked in this menu.
    std::array<std::string_view, 3> QuestUpdateSpeedChoices{
        "5 seconds", "10 seconds", "15 seconds"
    };

    std::string QuestUpdateSpeedLabel() {
        return std::to_string(GetQuestDiscordUpdateIntervalSeconds()) + " seconds";
    }

    int QuestUpdateSpeedValue(StringW label) {
        // Dropdown values are constrained by QuestUpdateSpeedChoices, but use
        // a safe default if a custom BSML build supplies an unexpected string.
        const std::string selected(label);
        if (selected == "5 seconds") return 5;
        if (selected == "15 seconds") return 15;
        return 10;
    }
}

void DidActivate(HMUI::ViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    // BSML can invoke an activation callback while a controller is being
    // replaced. Avoid building UI against a missing transform, and build only
    // once so reactivation cannot stack duplicate controls.
    if (!firstActivation || !self || !self->get_transform()) return;

    auto container = BSML::Lite::CreateScrollableSettingsContainer(self->get_transform());
    if (!container || !container->get_transform()) {
        logger.error("Discord settings UI could not create its scroll container");
        return;
    }

    // These shared vectors live as long as the mode-toggle callback and let it
    // switch whole groups without relying on fragile manual layout positions.
    auto desktopControls = std::make_shared<std::vector<UnityW<UnityEngine::GameObject>>>();
    auto questControls = std::make_shared<std::vector<UnityW<UnityEngine::GameObject>>>();
    const bool useQuest = getConfig().UseQuestDiscord.GetValue();

    FitToggleToSettingsPanel(BSML::Lite::CreateToggle(container->get_transform(), "Send Presence to Quest Discord App", useQuest,
        [desktopControls, questControls](bool enabled) {
            for (auto& control : *desktopControls) {
                if (control) control->SetActive(!enabled);
            }
            for (auto& control : *questControls) {
                if (control) control->SetActive(enabled);
            }
            SetQuestDiscordMode(enabled);
        }));

    auto desktopTitle = BSML::Lite::CreateText(container->get_transform(), "Desktop Companion");
    if (desktopTitle) {
        desktopTitle->set_alignment(TMPro::TextAlignmentOptions::Center);
        ReserveTextRow(desktopTitle, 6.0f);
        desktopControls->push_back(desktopTitle->get_gameObject());
    }

    auto* ipSetting = AddConfigValueInputString(container->get_transform(), getConfig().PCIPSetting);
    auto* portSetting = AddConfigValueInputString(container->get_transform(), getConfig().PortSetting);
    // Creation can fail when another mod tears down this controller mid-frame;
    // track only controls that actually exist.
    if (ipSetting) desktopControls->push_back(ipSetting->get_gameObject());
    if (portSetting) desktopControls->push_back(portSetting->get_gameObject());

    auto* instructionsButton = BSML::Lite::CreateUIButton(container->get_transform(), "Open Instructions", []() {
        UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#-quick-start");
    });
    if (instructionsButton) desktopControls->push_back(instructionsButton->get_gameObject());

    auto questTitle = BSML::Lite::CreateText(container->get_transform(), "Quest Discord Privacy");
    if (questTitle) {
        questTitle->set_alignment(TMPro::TextAlignmentOptions::Center);
        ReserveTextRow(questTitle, 6.0f);
        questControls->push_back(questTitle->get_gameObject());
    }
    auto questHelp = BSML::Lite::CreateText(
        container->get_transform(),
        "Sends directly to the Discord app installed on this Quest. Discord must be open or signed in.",
        3.0f);
    if (questHelp) {
        questHelp->set_enableWordWrapping(true);
        questHelp->set_alignment(TMPro::TextAlignmentOptions::Center);
        ReserveTextRow(questHelp, 9.0f);
        questControls->push_back(questHelp->get_gameObject());
    }

    auto* updateSpeedDropdown = BSML::Lite::CreateDropdown(
        container->get_transform(),
        "Discord Update Speed",
        QuestUpdateSpeedLabel(),
        QuestUpdateSpeedChoices,
        [](StringW value) {
            // This config value is consulted only by the native Quest branch
            // of the stat scheduler; Desktop Companion remains fixed at ten
            // seconds regardless of the selection made here.
            getConfig().QuestUpdateIntervalSeconds.SetValue(
                QuestUpdateSpeedValue(value));
        });
    if (updateSpeedDropdown) {
        BSML::Lite::AddHoverHint(
            updateSpeedDropdown,
            "How often gameplay statistics are sent directly to the Discord app on this Quest.");
        questControls->push_back(updateSpeedDropdown->get_gameObject());
    }

    // Config entries are process-lifetime objects, so capturing their addresses
    // is safe; each toggle immediately rebuilds the native Quest activity.
    auto addPrivacyToggle = [container, questControls](auto& setting) {
        auto* settingPtr = &setting;
        auto* toggle = FitToggleToSettingsPanel(BSML::Lite::CreateToggle(
            container->get_transform(),
            setting.GetName(),
            setting.GetValue(),
            [settingPtr](bool enabled) {
                // Config persistence and activity rebuilding are third-party
                // boundaries; contain their errors inside the toggle callback.
                try {
                    settingPtr->SetValue(enabled);
                    QuestDiscord::Refresh();
                } catch (const std::exception& error) {
                    logger.error("CreatePrivacyToggle callback could not apply the Quest privacy setting: {}", error.what());
                } catch (...) {
                    logger.error("CreatePrivacyToggle callback failed with a non-standard exception");
                }
            }));
        if (toggle) questControls->push_back(toggle->get_gameObject());
    };

    addPrivacyToggle(getConfig().QuestShowStatus);
    addPrivacyToggle(getConfig().QuestShowSongTitle);
    addPrivacyToggle(getConfig().QuestShowSongAuthor);
    addPrivacyToggle(getConfig().QuestShowMappers);
    addPrivacyToggle(getConfig().QuestShowDifficulty);
    addPrivacyToggle(getConfig().QuestShowScore);
    addPrivacyToggle(getConfig().QuestShowCombo);
    addPrivacyToggle(getConfig().QuestShowMisses);
    addPrivacyToggle(getConfig().QuestShowBadCuts);
    addPrivacyToggle(getConfig().QuestShowBombs);
    addPrivacyToggle(getConfig().QuestShowTimer);
    addPrivacyToggle(getConfig().QuestShowCoverArt);
    addPrivacyToggle(getConfig().QuestShowPlatform);
    addPrivacyToggle(getConfig().QuestShowMultiplayer);

    auto questStatus = BSML::Lite::CreateText(
        container->get_transform(),
        "Quest Discord status: " + QuestDiscord::GetConnectionStatus(),
        3.0f);
    if (questStatus) {
        questStatus->set_enableWordWrapping(true);
        questStatus->set_alignment(TMPro::TextAlignmentOptions::Center);
        ReserveTextRow(questStatus, 8.0f);
        questControls->push_back(questStatus->get_gameObject());
    }
    auto safeQuestStatus = UnityW(questStatus);

    auto* reconnectButton = BSML::Lite::CreateUIButton(container->get_transform(), "Reconnect Quest Discord", [safeQuestStatus]() mutable {
        // Reconnect is user-triggered and must never propagate JNI allocation
        // or class-loader failures through the BSML button event.
        try {
            QuestDiscord::Initialize();
            QuestDiscord::Refresh();
            if (safeQuestStatus) {
                safeQuestStatus->set_text(
                    "Quest Discord status: " + QuestDiscord::GetConnectionStatus());
            }
        } catch (const std::exception& error) {
            logger.error("CreateQuestContent reconnect callback failed safely: {}", error.what());
        } catch (...) {
            logger.error("CreateQuestContent reconnect callback failed with a non-standard exception");
        }
    });
    if (reconnectButton) questControls->push_back(reconnectButton->get_gameObject());

    // A control may have been destroyed by a concurrent view transition after
    // it was created; apply visibility only to still-valid objects.
    for (auto& control : *desktopControls) if (control) control->SetActive(!useQuest);
    for (auto& control : *questControls) if (control) control->SetActive(useQuest);
}

MAKE_HOOK_MATCH(MainMenuViewController_DidActivate, &MainMenuViewController::DidActivate, void, MainMenuViewController* self, bool firstActivation, bool addedToHierachy, bool screenSystemEnabling) {
    MainMenuViewController_DidActivate(self, firstActivation, addedToHierachy, screenSystemEnabling);

    // Desktop companion setup and update checks remain exactly as before, but
    // are irrelevant when presence is routed directly to the Quest Discord app.
    if (getConfig().UseQuestDiscord.GetValue()) return;

    if (!self) return;

    if (firstActivation && getConfig().FirstTime.GetValue()) {
        auto modal = BSML::Lite::CreateModal(self->transform, {100, 60}, []() {});
        // UI creation is optional and can fail under a replaced/custom menu;
        // abort this informational popup without affecting the main menu.
        if (!modal) return;

        auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);
        if (!verticalLayout) return;

        auto text = BSML::Lite::CreateText(verticalLayout, "Thank you for installing the mod! To setup your Discord RPC, please\nlook through the instructions by pressing \"Open Instructions\". ");
        if (text) {
            text->set_enableWordWrapping(true);
            text->set_alignment(TMPro::TextAlignmentOptions::Center);
        }

        auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);
        if (!horizontalLayout) return;
        // Capture a Unity-aware reference so button callbacks can detect a
        // modal destroyed by another menu transition.
        auto safeModal = UnityW(modal);

        BSML::Lite::CreateUIButton(horizontalLayout, "Open Instructions", []() {
            UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#-quick-start");
        });
        BSML::Lite::CreateUIButton(horizontalLayout, "Close", [safeModal]() mutable {
            getConfig().FirstTime.SetValue(false);
            if (safeModal) safeModal->Hide();
        });

        if (safeModal) safeModal->Show();

        return;
    }

    // The version check is informational and should run only once per menu
    // instance. Repeating it on every activation creates overlapping callbacks.
    if (!firstActivation) return;

    std::shared_future<WebUtils::JsonResponse> future = CreateRequest("GET", "/version", {});
    UnityW<MainMenuViewController> menuController(self);

    BSML::MainThreadScheduler::AwaitFuture<WebUtils::JsonResponse>(
        future,
        [future, menuController]() mutable -> void
        {
            // Async callbacks must not retain a raw Unity controller. UnityW
            // reports destruction safely if the menu was replaced while the
            // desktop companion request was in flight.
            if (!menuController) return;

            try {
                const auto& result = future.get();

                if (result.IsSuccessful()) {
                    const auto& parsed = result.GetParsedData();
                    if (!parsed.IsObject() || !parsed.HasMember("version") || !parsed["version"].IsString()) {
                        logger.warn("Desktop companion returned a successful response without a string version");
                        return;
                    }
                    const std::string version = parsed["version"].GetString();

                    std::shared_future<WebUtils::JsonResponse> githubFutureData = GetLatestGithub();

                    BSML::MainThreadScheduler::AwaitFuture<WebUtils::JsonResponse>(
                        githubFutureData,
                        [githubFutureData, menuController, version]() mutable -> void {
                            if (!menuController) return;

                            try {
                                const auto& githubResult = githubFutureData.get();
                                if (!githubResult.IsSuccessful()) {
                                    logger.warn("GitHub version check returned an unsuccessful response");
                                    return;
                                }
                                const auto& parsed = githubResult.GetParsedData();
                                if (!parsed.IsObject() || !parsed.HasMember("tag_name") || !parsed["tag_name"].IsString()) {
                                    logger.warn("GitHub release response did not contain a string tag_name");
                                    return;
                                }
                                const std::string latestVersion = parsed["tag_name"].GetString();

                                if (version != latestVersion) {
                                    auto modal = BSML::Lite::CreateModal(menuController->transform, {100, 40}, []() {});
                                    if (!modal) return;

                                    auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);
                                    if (!verticalLayout) return;

                                    auto text = BSML::Lite::CreateText(verticalLayout, "It seems like you're using an outdated version of the local server.\nPlease update it to latest whenever you can.");

                                    if (text) {
                                        text->set_enableWordWrapping(true);
                                        text->set_alignment(TMPro::TextAlignmentOptions::Center);
                                    }

                                    auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);
                                    if (!horizontalLayout) return;
                                    auto safeModal = UnityW(modal);

                                    BSML::Lite::CreateUIButton(horizontalLayout, "Update", [safeModal]() mutable {
                                        CreateRequest("POST", "/update", {});
                                        if (safeModal) safeModal->Hide();
                                    });
                                    BSML::Lite::CreateUIButton(horizontalLayout, "Close", [safeModal]() mutable {
                                        if (safeModal) safeModal->Hide();
                                    });

                                    if (safeModal) safeModal->Show();
                                }
                            } catch (const std::exception& error) {
                                // Future failures are expected when GitHub is
                                // offline; contain them inside this UI callback.
                                logger.warn("DidActivate GitHub version completion failed safely: {}", error.what());
                            } catch (...) {
                                logger.warn("DidActivate GitHub version completion failed with a non-standard exception");
                            }
                        });
                    return;
                }

                logger.debug("Failed to retrieve version.");

                auto modal = BSML::Lite::CreateModal(menuController->transform, {105, 40}, []() {});
                if (!modal) return;

                auto verticalLayout = BSML::Lite::CreateVerticalLayoutGroup(modal);
                if (!verticalLayout) return;

                auto text = BSML::Lite::CreateText(verticalLayout, "Your local server could not be updated due to not being able to connect.\nPlease open the instructions to download the new version of the local server");

                if (text) {
                    text->set_enableWordWrapping(true);
                    text->set_alignment(TMPro::TextAlignmentOptions::Center);
                }

                auto horizontalLayout = BSML::Lite::CreateHorizontalLayoutGroup(verticalLayout);
                if (!horizontalLayout) return;
                auto safeModal = UnityW(modal);

                BSML::Lite::CreateUIButton(horizontalLayout, "Open Instructions", []() {
                    UnityEngine::Application::OpenURL("https://github.com/RainzDev/BSQ_DiscordRichPresence#2-install-local-server");
                });
                BSML::Lite::CreateUIButton(horizontalLayout, "Close", [safeModal]() mutable {
                    if (safeModal) safeModal->Hide();
                });

                if (safeModal) safeModal->Show();
            } catch (const std::exception& error) {
                // The desktop server being offline must not unwind through the
                // main-thread scheduler and terminate Beat Saber.
                logger.warn("DidActivate Desktop companion version completion failed safely: {}", error.what());
            } catch (...) {
                logger.warn("DidActivate Desktop companion version completion failed with a non-standard exception");
            }
        });
}

void InstallUIHooks() {
    INSTALL_HOOK(logger, MainMenuViewController_DidActivate);
}
