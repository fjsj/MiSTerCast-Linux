#pragma once
#include "mistercast/types.hpp"
#include <filesystem>
namespace mistercast {
std::filesystem::path configPath();
AppConfig loadConfig(const std::filesystem::path&, std::string* warning = nullptr);
bool saveConfig(const AppConfig&, const std::filesystem::path&, std::string& error);
}
