#pragma once

#include <string>
#include "config-utils/shared/config-utils.hpp"


DECLARE_CONFIG(Config) {
    CONFIG_VALUE(PCIPSetting, std::string, "Private IP", "");
    CONFIG_VALUE(PortSetting, std::string, "Port", "8080");
    CONFIG_VALUE(FirstTime, bool, "First Time", true);

    // Keep Quest transport/privacy settings separate from the desktop payload;
    // switching back to the companion therefore preserves its original format.
    CONFIG_VALUE(UseQuestDiscord, bool, "Use Quest Discord App", false);

    // Store the native Quest activity cadence independently from the desktop
    // companion's fixed ten-second update loop. Older configs omit this entry
    // and therefore retain the established ten-second default automatically.
    CONFIG_VALUE(QuestUpdateIntervalSeconds, int, "Quest Discord Update Interval Seconds", 10);

    // Each value controls one independently optional Discord activity field so
    // disabled data is omitted instead of replaced with misleading zero text.
    CONFIG_VALUE(QuestShowStatus, bool, "Show Activity Status", true);
    CONFIG_VALUE(QuestShowSongTitle, bool, "Show Song Title", true);
    CONFIG_VALUE(QuestShowSongAuthor, bool, "Show Song Author", true);
    CONFIG_VALUE(QuestShowMappers, bool, "Show Mappers", true);
    CONFIG_VALUE(QuestShowDifficulty, bool, "Show Difficulty", true);
    CONFIG_VALUE(QuestShowScore, bool, "Show Score", true);
    // Combo is a native Quest activity field only. Keeping its preference in
    // the Quest group prevents this addition from changing the payload expected
    // by the unmodified Desktop Companion application.
    CONFIG_VALUE(QuestShowCombo, bool, "Show Current Combo", true);
    CONFIG_VALUE(QuestShowMisses, bool, "Show Missed Notes", true);
    CONFIG_VALUE(QuestShowBadCuts, bool, "Show Bad Cuts", true);
    CONFIG_VALUE(QuestShowBombs, bool, "Show Bombs Hit", true);
    CONFIG_VALUE(QuestShowTimer, bool, "Show Song Timer", true);
    CONFIG_VALUE(QuestShowCoverArt, bool, "Show Cover Art", true);
    CONFIG_VALUE(QuestShowPlatform, bool, "Show Meta Quest Badge", true);
    CONFIG_VALUE(QuestShowMultiplayer, bool, "Show Multiplayer Details", true);
};

// Treat hand-edited or obsolete config values as the established ten-second
// default. Keeping validation in one helper ensures the UI and scheduler cannot
// disagree about which native Quest update intervals are supported.
inline int GetQuestDiscordUpdateIntervalSeconds() {
    const int configured = getConfig().QuestUpdateIntervalSeconds.GetValue();
    return configured == 5 || configured == 15 ? configured : 10;
}
