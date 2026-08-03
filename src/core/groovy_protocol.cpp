#include "mistercast/groovy_protocol.hpp"

namespace mistercast {
std::optional<std::string> validateGroovyModeline(const Modeline& modeline) {
  if (auto error = modeline.validate()) return error;
  if (static_cast<uint64_t>(modeline.hActive) * modeline.vActive * 3 >
      GroovyFramebufferBytes)
    return "active image exceeds Groovy_MiSTer frame buffer";
  return {};
}

std::optional<std::string> validateGroovyConfig(const AppConfig& config) {
  if (auto error = config.validate()) return error;
  if (auto error = validateGroovyModeline(config.modeline)) return error;
  for (const auto& custom : config.customModelines)
    if (auto error = validateGroovyModeline(custom))
      return "invalid custom modeline '" + custom.name + "': " + *error;
  return {};
}
}  // namespace mistercast
