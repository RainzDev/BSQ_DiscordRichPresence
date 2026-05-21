#pragma once

#include <string>
#include "nlohmann/json.hpp"
#include "web-utils/shared/WebUtils.hpp"

std::future<WebUtils::JsonResponse> CreateRequest(std::string method, std::string URLPath, nlohmann::json jsonData);
std::future<WebUtils::JsonResponse> GetLatestGithub();