#pragma once

#include <string>
#include "config-utils/shared/config-utils.hpp"


DECLARE_CONFIG(Config) {
    CONFIG_VALUE(PCIPSetting, std::string, "Private IP", "");
    CONFIG_VALUE(PortSetting, std::string, "Port", "8080");
    CONFIG_VALUE(FirstTime, bool, "First Time", true);
};