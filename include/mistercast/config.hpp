#pragma once
#include <filesystem>

#include "mistercast/types.hpp"
namespace mistercast {
std::filesystem::path configPath();
// Application persistence accepts only configurations the Groovy_MiSTer
// protocol can stream, including every saved custom modeline.
AppConfig loadGroovyConfig(const std::filesystem::path&,
                           std::string* warning = nullptr);
bool saveGroovyConfig(const AppConfig&, const std::filesystem::path&,
                      std::string& error);
}  // namespace mistercast
