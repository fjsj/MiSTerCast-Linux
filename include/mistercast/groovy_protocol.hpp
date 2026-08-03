#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "mistercast/types.hpp"

namespace mistercast {
inline constexpr size_t GroovyFramebufferBytes = 1245312;
inline constexpr size_t GroovyUdpPayloadBytes = 1472;
inline constexpr size_t GroovyUdpWireOverheadBytes = 66;

std::optional<std::string> validateGroovyModeline(const Modeline& modeline);
std::optional<std::string> validateGroovyConfig(const AppConfig& config);
}  // namespace mistercast
