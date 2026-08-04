#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "mistercast/types.hpp"
#include "mistercast/udp_pacing.hpp"

namespace mistercast {
inline constexpr size_t GroovyFramebufferBytes = 1245312;
inline constexpr size_t GroovyUdpPayloadBytes = 1472;
inline constexpr size_t GroovyUdpWireOverheadBytes = 66;

// The UDP video profile Groovy_MiSTer is driven with. The pacing rate, batch size
// and late tolerance are UdpVideoConfig's own defaults; only the Groovy-specific
// sizes are stated here.
UdpVideoConfig groovyVideoConfig() noexcept;

std::optional<std::string> validateGroovyModeline(const Modeline& modeline);
std::optional<std::string> validateGroovyConfig(const AppConfig& config);
}  // namespace mistercast
