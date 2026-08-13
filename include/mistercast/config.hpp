#pragma once
#include <filesystem>

#include "mistercast/types.hpp"
namespace mistercast {
std::filesystem::path configPath();
// Application persistence accepts only configurations the Groovy_MiSTer
// protocol can stream, including every saved custom modeline.
AppConfig loadGroovyConfig(const std::filesystem::path&,
                           std::string* warning = nullptr);
// The exact document saveGroovyConfig writes, so two configurations can be
// compared for persistence purposes without touching the filesystem.
std::string serializeGroovyConfig(const AppConfig&);
bool saveGroovyConfig(const AppConfig&, const std::filesystem::path&,
                      std::string& error);

// The ScreenCast portal's restore token lives beside config.json rather than in
// it, for two reasons: it is cached permission state rather than a setting the
// user chose, so it must not be caught up in "settings are saved only on
// request", and it is a capability handle, so it is written user-only instead of
// with the configuration's default permissions.
std::filesystem::path portalRestoreTokenPath();
std::string loadPortalRestoreToken(const std::filesystem::path&);
bool savePortalRestoreToken(const std::string& token,
                            const std::filesystem::path&, std::string& error);
}  // namespace mistercast
