#include "mistercast/types.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace mistercast {
std::optional<std::string> Modeline::validate() const {
  if (!std::isfinite(pixelClockMHz) || pixelClockMHz <= 0.1 ||
      pixelClockMHz > 400.0)
    return "pixel clock must be between 0.1 and 400 MHz";
  if (!hActive || hActive > 4096 ||
      !(hActive < hBegin && hBegin <= hEnd && hEnd < hTotal))
    return "horizontal timings must be ordered active < begin <= end < total";
  if (!vActive || vActive > 2160 ||
      !(vActive < vBegin && vBegin <= vEnd && vEnd < vTotal))
    return "vertical timings must be ordered active < begin <= end < total";
  return {};
}
double Modeline::refreshHz() const {
  return pixelClockMHz * 1000000.0 / hTotal / vTotal * (interlaced ? 2.0 : 1.0);
}
Modeline Modeline::safeDefault() {
  return {"320x240 NTSC (60Hz)",
          6.7,
          320,
          336,
          367,
          426,
          240,
          244,
          247,
          262,
          false};
}
std::optional<std::string> SourceOptions::validate() const {
  if (!width || !height || width > 8192 || height > 8192)
    return "source size must be between 1 and 8192 pixels";
  if (frameDelay > 10) return "frame delay must be automatic (0) or 1-10";
  if (sampling != SamplingMode::Point && sampling != SamplingMode::Bilinear &&
      sampling != SamplingMode::LineBlend)
    return "unsupported sampling mode";
  return {};
}
std::optional<std::string> AppConfig::validate() const {
  if (version != 1) return "unsupported configuration version";
  if (auto e = source.validate()) return e;
  if (auto e = modeline.validate()) return e;
  for (const auto& custom : customModelines)
    if (auto e = custom.validate())
      return "invalid custom modeline '" + custom.name + "': " + *e;
  return {};
}
template <class E>
static std::string enumString(E v, const char* const* names, size_t count) {
  auto i = static_cast<size_t>(v);
  return i < count ? names[i] : "unknown";
}
std::string toString(Alignment v) {
  static const char* n[] = {"center",    "top-left",    "top",
                            "top-right", "right",       "bottom-right",
                            "bottom",    "bottom-left", "left"};
  return enumString(v, n, 9);
}
std::string toString(CropMode v) {
  static const char* n[] = {"custom", "1x", "2x",  "3x",
                            "4x",     "5x", "4:3", "5:4"};
  return enumString(v, n, 8);
}
std::string toString(Rotation v) {
  static const char* n[] = {"none", "cw90", "ccw90", "180"};
  return enumString(v, n, 4);
}
std::string toString(SamplingMode v) {
  static const char* n[] = {"point", "bilinear", "line-blend"};
  return enumString(v, n, 3);
}
template <class E>
static bool parseEnum(const std::string& s, E& out, const char* const* names,
                      size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (s == names[i]) {
      out = static_cast<E>(i);
      return true;
    }
  return false;
}
bool parseAlignment(const std::string& s, Alignment& o) {
  static const char* n[] = {"center",    "top-left",    "top",
                            "top-right", "right",       "bottom-right",
                            "bottom",    "bottom-left", "left"};
  return parseEnum(s, o, n, 9);
}
bool parseCropMode(const std::string& s, CropMode& o) {
  static const char* n[] = {"custom", "1x", "2x",  "3x",
                            "4x",     "5x", "4:3", "5:4"};
  return parseEnum(s, o, n, 8);
}
bool parseRotation(const std::string& s, Rotation& o) {
  static const char* n[] = {"none", "cw90", "ccw90", "180"};
  return parseEnum(s, o, n, 4);
}
bool parseSamplingMode(const std::string& s, SamplingMode& o) {
  static const char* n[] = {"point", "bilinear", "line-blend"};
  return parseEnum(s, o, n, 3);
}
bool parseModeline(const std::string& text, Modeline& out, std::string& error) {
  std::istringstream in(text);
  Modeline m;
  m.name = "Custom";
  int interlace = 0;
  if (!(in >> m.pixelClockMHz >> m.hActive >> m.hBegin >> m.hEnd >> m.hTotal >>
        m.vActive >> m.vBegin >> m.vEnd >> m.vTotal >> interlace) ||
      (interlace != 0 && interlace != 1)) {
    error =
        "modeline needs: clock hactive hbegin hend htotal vactive vbegin vend "
        "vtotal interlace(0|1)";
    return false;
  }
  std::string extra;
  if (in >> extra) {
    error = "unexpected text after modeline";
    return false;
  }
  m.interlaced = interlace;
  if (auto e = m.validate()) {
    error = *e;
    return false;
  }
  out = std::move(m);
  return true;
}
std::vector<Modeline> bundledModelines() {
  return {{"256x240 NTSC (60Hz)", 4.905, 256, 264, 287, 312, 240, 241, 244, 262,
           false},
          Modeline::safeDefault(),
          {"320x480i NTSC (60Hz)", 6.7, 320, 336, 367, 426, 480, 488, 493, 525,
           true},
          {"640x480i NTSC (60Hz)", 12.336, 640, 662, 720, 784, 480, 488, 494,
           525, true},
          {"720x480i NTSC (60Hz)", 13.846, 720, 744, 809, 880, 480, 488, 494,
           525, true},
          {"256x240 PAL (50Hz)", 5.32, 256, 269, 294, 341, 240, 270, 273, 312,
           false},
          {"320x240 PAL (50Hz)", 6.66, 320, 336, 367, 426, 240, 270, 273, 312,
           false},
          {"320x480i PAL (50Hz)", 6.66, 320, 336, 367, 426, 480, 540, 545, 625,
           true},
          {"640x480i PAL (50Hz)", 13.32, 640, 672, 734, 852, 480, 540, 545, 625,
           true},
          {"720x576i PAL (50Hz)", 13.875, 720, 741, 806, 888, 576, 581, 586,
           625, true}};
}
}  // namespace mistercast
