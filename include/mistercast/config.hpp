#pragma once
#include <filesystem>

#include "mistercast/types.hpp"
namespace mistercast {
std::filesystem::path configPath();
AppConfig loadConfig(const std::filesystem::path&,
                     std::string* warning = nullptr);
bool saveConfig(const AppConfig&, const std::filesystem::path&,
                std::string& error);
}  // namespace mistercast
