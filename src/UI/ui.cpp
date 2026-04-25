#include "ui.hpp"
#include "main.hpp"
#include "../config.hpp"
#include "bsml/shared/BSML.hpp"
#include "UnityEngine/Application.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"

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
    }
}

void InstallUIHooks() {
    INSTALL_HOOK(logger, MainMenuViewController_DidActivate);
}