#pragma once

namespace HMUI {
    class ViewController;
}

class Hook_MainMenuViewController_DidActivate;

void DidActivate(HMUI::ViewController* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
void InstallUIHooks();