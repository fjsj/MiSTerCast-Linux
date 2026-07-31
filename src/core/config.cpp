#include "mistercast/config.hpp"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace mistercast {
std::filesystem::path configPath() {
  if (const char* value = std::getenv("XDG_CONFIG_HOME"); value && *value)
    return std::filesystem::path(value) / "mistercast/config.json";
  if (const char* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".config/mistercast/config.json";
  return std::filesystem::current_path() / "config.json";
}

static std::string escape(const std::string& value) {
  std::string result;
  for (char character : value) {
    if (character == '"' || character == '\\') result += '\\';
    if (character == '\n') {
      result += "\\n";
      continue;
    }
    result += character;
  }
  return result;
}

// Returns the document with the contents of every nested object and array
// removed, so that a top-level lookup cannot match a key of the same name
// inside customModelines. The key-matching below is positional, so without this
// the result depends on the order the writer happened to emit keys in.
static std::string topLevelScope(const std::string& json) {
  std::string result;
  int depth = 0;
  bool inString = false, escaped = false;
  for (char character : json) {
    const bool wasEscaped = escaped;
    escaped = false;
    if (!wasEscaped && inString && character == '\\') escaped = true;
    if (!wasEscaped && character == '"') inString = !inString;
    if (!inString && !wasEscaped && (character == '{' || character == '[')) {
      if (++depth <= 1) result += character;
      continue;
    }
    if (!inString && !wasEscaped && (character == '}' || character == ']')) {
      if (depth-- <= 1) result += character;
      continue;
    }
    if (depth <= 1) result += character;
  }
  return result;
}

static bool stringValue(const std::string& json, const char* key,
                        std::string& value) {
  const std::regex expression(std::string("\\\"") + key +
                              "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
  std::smatch match;
  if (!std::regex_search(json, match, expression)) return false;
  value = match[1];
  return true;
}

static bool numberValue(const std::string& json, const char* key,
                        double& value) {
  const std::regex expression(std::string("\\\"") + key +
                              "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
  std::smatch match;
  if (!std::regex_search(json, match, expression)) return false;
  try {
    value = std::stod(match[1]);
    return true;
  } catch (...) {
    return false;
  }
}

static bool boolValue(const std::string& json, const char* key, bool& value) {
  const std::regex expression(std::string("\\\"") + key +
                              "\\\"\\s*:\\s*(true|false)");
  std::smatch match;
  if (!std::regex_search(json, match, expression)) return false;
  value = match[1] == "true";
  return true;
}

static bool modelineObject(const std::string& json, Modeline& modeline) {
  double number = 0;
  stringValue(json, "name", modeline.name);
  auto get = [&](const char* key, auto& value) {
    double parsed = 0;
    if (!numberValue(json, key, parsed)) return false;
    value = static_cast<std::decay_t<decltype(value)>>(parsed);
    return true;
  };
  if (!numberValue(json, "pixelClockMHz", number)) return false;
  modeline.pixelClockMHz = number;
  if (!get("hActive", modeline.hActive) || !get("hBegin", modeline.hBegin) ||
      !get("hEnd", modeline.hEnd) || !get("hTotal", modeline.hTotal) ||
      !get("vActive", modeline.vActive) || !get("vBegin", modeline.vBegin) ||
      !get("vEnd", modeline.vEnd) || !get("vTotal", modeline.vTotal))
    return false;
  bool interlaced = false;
  boolValue(json, "interlaced", interlaced);
  modeline.interlaced = interlaced;
  return !modeline.validate();
}

AppConfig loadConfig(const std::filesystem::path& path, std::string* warning) {
  AppConfig config;
  // Cleared up front so a successful load cannot leave a caller looking at the
  // warning from a previous one.
  if (warning) warning->clear();
  std::ifstream file(path);
  if (!file) return config;
  std::stringstream buffer;
  buffer << file.rdbuf();
  const auto document = buffer.str();
  // Top-level keys are read from the scoped view; the array itself is parsed
  // from the full document below.
  const auto json = topLevelScope(document);
  double version = 0;
  if (!numberValue(json, "version", version) || version != 1) {
    if (warning)
      *warning = "invalid or unsupported config; safe defaults loaded";
    return AppConfig{};
  }
  config.version = 1;
  stringValue(json, "target", config.target);
  stringValue(json, "monitor", config.source.monitor);
  stringValue(json, "audioSink", config.source.audioSink);
  stringValue(json, "modelineName", config.modeline.name);
  auto number = [&](const char* key, auto& value) {
    double parsed = 0;
    if (numberValue(json, key, parsed))
      value = static_cast<std::decay_t<decltype(value)>>(parsed);
  };
  number("frameDelay", config.source.frameDelay);
  number("width", config.source.width);
  number("height", config.source.height);
  number("xOffset", config.source.xOffset);
  number("yOffset", config.source.yOffset);
  number("pixelClockMHz", config.modeline.pixelClockMHz);
  number("hActive", config.modeline.hActive);
  number("hBegin", config.modeline.hBegin);
  number("hEnd", config.modeline.hEnd);
  number("hTotal", config.modeline.hTotal);
  number("vActive", config.modeline.vActive);
  number("vBegin", config.modeline.vBegin);
  number("vEnd", config.modeline.vEnd);
  number("vTotal", config.modeline.vTotal);
  boolValue(json, "syncRefresh", config.source.syncRefresh);
  boolValue(json, "progressiveInterlaceBuffer",
            config.source.progressiveInterlaceBuffer);
  boolValue(json, "audio", config.source.audio);
  boolValue(json, "preview", config.source.preview);
  boolValue(json, "interlaced", config.modeline.interlaced);
  std::string value;
  if (stringValue(json, "alignment", value))
    parseAlignment(value, config.source.alignment);
  if (stringValue(json, "crop", value))
    parseCropMode(value, config.source.crop);
  if (stringValue(json, "rotation", value))
    parseRotation(value, config.source.rotation);
  const auto array = document.find("\"customModelines\"");
  if (array != std::string::npos) {
    const auto begin = document.find('[', array),
               end = document.find(']', begin);
    if (begin != std::string::npos && end != std::string::npos) {
      const std::regex object("\\{([^{}]*)\\}");
      for (std::sregex_iterator
               it(document.begin() + begin, document.begin() + end, object),
           last;
           it != last; ++it) {
        Modeline custom;
        if (modelineObject((*it)[1], custom))
          config.customModelines.push_back(std::move(custom));
      }
    }
  }
  if (auto error = config.validate()) {
    if (warning)
      *warning = "invalid config (" + *error + "); safe defaults loaded";
    return AppConfig{};
  }
  return config;
}

bool saveConfig(const AppConfig& config, const std::filesystem::path& path,
                std::string& error) {
  if (auto validation = config.validate()) {
    error = *validation;
    return false;
  }
  std::error_code filesystemError;
  std::filesystem::create_directories(path.parent_path(), filesystemError);
  if (filesystemError) {
    error = filesystemError.message();
    return false;
  }
  auto temporary = path;
  temporary += ".tmp." + std::to_string(::getpid());
  std::ofstream file(temporary, std::ios::trunc);
  if (!file) {
    error = "cannot create temporary configuration";
    return false;
  }
  file << "{\n  \"version\": 1,\n"
       << "  \"target\": \"" << escape(config.target) << "\",\n"
       << "  \"monitor\": \"" << escape(config.source.monitor) << "\",\n"
       << "  \"audioSink\": \"" << escape(config.source.audioSink) << "\",\n"
       << "  \"syncRefresh\": "
       << (config.source.syncRefresh ? "true" : "false") << ",\n"
       << "  \"progressiveInterlaceBuffer\": "
       << (config.source.progressiveInterlaceBuffer ? "true" : "false") << ",\n"
       << "  \"audio\": " << (config.source.audio ? "true" : "false") << ",\n"
       << "  \"preview\": " << (config.source.preview ? "true" : "false")
       << ",\n"
       << "  \"frameDelay\": " << config.source.frameDelay << ",\n"
       << "  \"alignment\": \"" << toString(config.source.alignment) << "\",\n"
       << "  \"crop\": \"" << toString(config.source.crop) << "\",\n"
       << "  \"width\": " << config.source.width
       << ", \"height\": " << config.source.height << ",\n"
       << "  \"xOffset\": " << config.source.xOffset
       << ", \"yOffset\": " << config.source.yOffset << ",\n"
       << "  \"rotation\": \"" << toString(config.source.rotation) << "\",\n"
       << "  \"modelineName\": \"" << escape(config.modeline.name) << "\",\n"
       << "  \"pixelClockMHz\": " << config.modeline.pixelClockMHz << ",\n"
       << "  \"hActive\": " << config.modeline.hActive
       << ", \"hBegin\": " << config.modeline.hBegin
       << ", \"hEnd\": " << config.modeline.hEnd
       << ", \"hTotal\": " << config.modeline.hTotal << ",\n"
       << "  \"vActive\": " << config.modeline.vActive
       << ", \"vBegin\": " << config.modeline.vBegin
       << ", \"vEnd\": " << config.modeline.vEnd
       << ", \"vTotal\": " << config.modeline.vTotal << ",\n"
       << "  \"interlaced\": "
       << (config.modeline.interlaced ? "true" : "false") << ",\n"
       << "  \"customModelines\": [";
  for (size_t index = 0; index < config.customModelines.size(); ++index) {
    const auto& modeline = config.customModelines[index];
    if (index) file << ',';
    file << "\n    {\"name\":\"" << escape(modeline.name)
         << "\",\"pixelClockMHz\":" << modeline.pixelClockMHz
         << ",\"hActive\":" << modeline.hActive
         << ",\"hBegin\":" << modeline.hBegin << ",\"hEnd\":" << modeline.hEnd
         << ",\"hTotal\":" << modeline.hTotal
         << ",\"vActive\":" << modeline.vActive
         << ",\"vBegin\":" << modeline.vBegin << ",\"vEnd\":" << modeline.vEnd
         << ",\"vTotal\":" << modeline.vTotal
         << ",\"interlaced\":" << (modeline.interlaced ? "true" : "false")
         << '}';
  }
  file << "\n  ]\n}\n";
  file.flush();
  if (!file) {
    error = "failed writing configuration";
    file.close();
    std::filesystem::remove(temporary);
    return false;
  }
  file.close();
  std::filesystem::rename(temporary, path, filesystemError);
  if (filesystemError) {
    error = filesystemError.message();
    std::filesystem::remove(temporary);
    return false;
  }
  return true;
}
}  // namespace mistercast
