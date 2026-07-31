#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mistercast {
enum class Alignment : uint8_t {
  Center,
  TopLeft,
  Top,
  TopRight,
  Right,
  BottomRight,
  Bottom,
  BottomLeft,
  Left
};
enum class CropMode : uint8_t { Custom, X1, X2, X3, X4, X5, Full43, Full54 };
enum class Rotation : uint8_t { None, CW90, CCW90, Flip180 };
enum class SessionState : uint8_t {
  Idle,
  Starting,
  Streaming,
  Stopping,
  Error
};

struct Modeline {
  std::string name;
  double pixelClockMHz{6.7};
  uint16_t hActive{320}, hBegin{336}, hEnd{367}, hTotal{426};
  uint16_t vActive{240}, vBegin{244}, vEnd{247}, vTotal{262};
  bool interlaced{false};
  std::optional<std::string> validate() const;
  double refreshHz() const;
  static Modeline safeDefault();
};

struct SourceOptions {
  std::string monitor;
  std::string audioSink;
  bool syncRefresh{true}, progressiveInterlaceBuffer{false}, audio{true},
      preview{true};
  uint16_t frameDelay{0}, width{320}, height{240};
  int16_t xOffset{0}, yOffset{0};
  Alignment alignment{Alignment::Center};
  CropMode crop{CropMode::Full43};
  Rotation rotation{Rotation::None};
  std::optional<std::string> validate() const;
};

struct AppConfig {
  uint32_t version{1};
  std::string target;
  SourceOptions source;
  Modeline modeline{Modeline::safeDefault()};
  std::vector<Modeline> customModelines;
  std::optional<std::string> validate() const;
};

struct Frame {
  uint32_t width{}, height{}, stride{};
  uint64_t sequence{};
  std::vector<uint8_t> bgra;
};
struct Monitor {
  std::string name;
  int16_t x{}, y{};
  uint16_t width{}, height{};
  bool primary{};
};
struct AudioSink {
  std::string name, description;
  bool isDefault{};
};
struct PcmBlock {
  uint32_t sampleRate{};
  uint64_t timestampNs{};
  std::vector<int16_t> samples;
};
struct SessionError {
  std::string component, message, hint;
};

std::string toString(Alignment value);
std::string toString(CropMode value);
std::string toString(Rotation value);
bool parseAlignment(const std::string&, Alignment&);
bool parseCropMode(const std::string&, CropMode&);
bool parseRotation(const std::string&, Rotation&);
bool parseModeline(const std::string&, Modeline&, std::string& error);
std::vector<Modeline> bundledModelines();
}  // namespace mistercast
