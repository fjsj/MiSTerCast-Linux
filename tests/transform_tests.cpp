#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

#include "mistercast/transform.hpp"

using namespace mistercast;

static int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)

static Modeline testModeline(uint16_t width, uint16_t height,
                             bool interlaced = false) {
  return {"test", 1.0, width, uint16_t(width + 1), uint16_t(width + 2),
          uint16_t(width + 3), height, uint16_t(height + 1),
          uint16_t(height + 2), uint16_t(height + 3), interlaced};
}

static Frame grayFrame(uint32_t width, uint32_t height,
                       const std::vector<uint8_t>& values) {
  Frame frame;
  frame.width = width;
  frame.height = height;
  frame.stride = width * 4;
  frame.bgra.resize(size_t(frame.stride) * height);
  for (size_t i = 0; i < values.size(); ++i) {
    frame.bgra[i * 4] = values[i];
    frame.bgra[i * 4 + 1] = values[i];
    frame.bgra[i * 4 + 2] = values[i];
    frame.bgra[i * 4 + 3] = 255;
  }
  return frame;
}

static std::vector<uint8_t> blueValues(const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < rgb.size(); i += 3) result.push_back(rgb[i]);
  return result;
}

static void checkSamplingTransforms() {
  SourceOptions source;
  source.crop = CropMode::Custom;
  std::vector<uint8_t> rgb;
  std::string error;

  SamplingMode parsed = SamplingMode::Point;
  CHECK(parseSamplingMode("point", parsed) && parsed == SamplingMode::Point);
  CHECK(parseSamplingMode("bilinear", parsed) &&
        parsed == SamplingMode::Bilinear);
  CHECK(parseSamplingMode("line-blend", parsed) &&
        parsed == SamplingMode::LineBlend);
  CHECK(!parseSamplingMode("area", parsed));
  CHECK(toString(SamplingMode::LineBlend) == "line-blend");
  source.sampling = static_cast<SamplingMode>(255);
  CHECK(source.validate() == "unsupported sampling mode");
  AppConfig invalidConfig;
  invalidConfig.source = source;
  CHECK(invalidConfig.validate() == "unsupported sampling mode");
  auto validFrame = grayFrame(1, 1, {42});
  CHECK(!transformRgb24(validFrame, {0, 0, 1, 1}, source,
                        testModeline(1, 1), 0, rgb, error));
  CHECK(error == "unsupported sampling mode");

  source.sampling = SamplingMode::Bilinear;
  auto frame = grayFrame(2, 2, {0, 64, 128, 255});
  CHECK(transformRgb24(frame, {0, 0, 2, 2}, source, testModeline(1, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({112}));

  frame = grayFrame(2, 1, {10, 110});
  CHECK(transformRgb24(frame, {0, 0, 2, 1}, source, testModeline(4, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({10, 35, 85, 110}));

  frame = grayFrame(18, 9, std::vector<uint8_t>(162));
  for (uint32_t y = 0; y < 9; ++y)
    for (uint32_t x = 0; x < 18; ++x)
      for (unsigned channel = 0; channel < 3; ++channel)
        frame.bgra[(size_t(y) * 18 + x) * 4 + channel] = uint8_t(y * 18 + x);
  CHECK(transformRgb24(frame, {0, 0, 18, 9}, source, testModeline(2, 1), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({76, 85}));

  source.sampling = SamplingMode::LineBlend;
  frame = grayFrame(1, 4, {10, 30, 50, 70});
  CHECK(transformRgb24(frame, {0, 0, 1, 4}, source, testModeline(1, 2), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({20, 60}));
  frame = grayFrame(1, 5, {0, 10, 20, 30, 40});
  CHECK(transformRgb24(frame, {0, 0, 1, 5}, source, testModeline(1, 2), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({8, 32}));
  std::vector<uint8_t> hdLines(1080);
  for (size_t y = 0; y < hdLines.size(); ++y) hdLines[y] = uint8_t(y % 251);
  frame = grayFrame(1, 1080, hdLines);
  CHECK(transformRgb24(frame, {0, 0, 1, 1080}, source,
                       testModeline(1, 240), 0, rgb, error));
  const auto reducedLines = blueValues(rgb);
  CHECK(reducedLines.size() == 240 && reducedLines[0] == 2 &&
        reducedLines[1] == 6);
  frame = grayFrame(1, 2, {20, 80});
  CHECK(transformRgb24(frame, {0, 0, 1, 2}, source, testModeline(1, 3), 0,
                       rgb, error));
  CHECK(blueValues(rgb) == std::vector<uint8_t>({20, 50, 80}));
  frame = grayFrame(3, 7, std::vector<uint8_t>(21, 123));
  CHECK(transformRgb24(frame, {0, 0, 3, 7}, source, testModeline(2, 4), 0,
                       rgb, error));
  CHECK(std::all_of(rgb.begin(), rgb.end(), [](uint8_t x) { return x == 123; }));

  frame = grayFrame(2, 3, {0, 1, 2, 3, 4, 5});
  struct RotationCase {
    Rotation rotation;
    uint16_t width, height;
    std::vector<uint8_t> expected;
  };
  const RotationCase rotations[] = {
      {Rotation::None, 2, 3, {0, 1, 2, 3, 4, 5}},
      {Rotation::Flip180, 2, 3, {5, 4, 3, 2, 1, 0}},
      {Rotation::CW90, 3, 2, {4, 2, 0, 5, 3, 1}},
      {Rotation::CCW90, 3, 2, {1, 3, 5, 0, 2, 4}},
  };
  for (const auto mode : {SamplingMode::Point, SamplingMode::Bilinear,
                          SamplingMode::LineBlend}) {
    source.sampling = mode;
    for (const auto& rotation : rotations) {
      source.rotation = rotation.rotation;
      CHECK(transformRgb24(frame, {0, 0, 2, 3}, source,
                           testModeline(rotation.width, rotation.height), 0,
                           rgb, error));
      CHECK(blueValues(rgb) == rotation.expected);
    }
  }

  source.rotation = Rotation::None;
  frame = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  for (const auto mode : {SamplingMode::Point, SamplingMode::Bilinear,
                          SamplingMode::LineBlend}) {
    source.sampling = mode;
    auto interlaced = testModeline(2, 4, true);
    source.progressiveInterlaceBuffer = false;
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 0, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({2, 3, 6, 7}));
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 1, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({0, 1, 4, 5}));
    source.progressiveInterlaceBuffer = true;
    CHECK(transformRgb24(frame, {0, 0, 2, 4}, source, interlaced, 1, rgb,
                         error));
    CHECK(blueValues(rgb) == std::vector<uint8_t>({0, 1, 2, 3, 4, 5, 6, 7}));
    Frame malformed = frame;
    malformed.bgra.pop_back();
    CHECK(!transformRgb24(malformed, {0, 0, 2, 4}, source, interlaced, 0,
                          rgb, error));
    CHECK(!transformRgb24(frame, {1, 0, 2, 4}, source, interlaced, 0, rgb,
                          error));
  }
}

static void checkCropPointAndNormalization() {
  std::string error;
  SourceOptions crop;
  Modeline cropMode = Modeline::safeDefault();
  CropRect rect;
  crop.crop = CropMode::X1;
  crop.width = crop.height = 64;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 320 && rect.height == 240);
  crop.crop = CropMode::X3;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 960 && rect.height == 720);
  crop.crop = CropMode::Full43;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 2880 && rect.height == 2160);
  crop.rotation = Rotation::CW90;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 1620 && rect.height == 2160);
  crop.rotation = Rotation::CCW90;
  crop.crop = CropMode::Full54;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.width == 1728 && rect.height == 2160);
  crop.rotation = Rotation::None;
  crop.crop = CropMode::Custom;
  crop.width = 640;
  crop.height = 480;
  crop.alignment = Alignment::TopLeft;
  crop.xOffset = 4000;
  crop.yOffset = 2000;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.x == 3200 && rect.y == 1680);
  crop.xOffset = -3000;
  crop.yOffset = -2000;
  CHECK(calculateCrop(3840, 2160, crop, cropMode, rect, error));
  CHECK(rect.x == 0 && rect.y == 0);

  Frame frame = grayFrame(4, 2, {0, 1, 2, 3, 4, 5, 6, 7});
  SourceOptions source;
  source.crop = CropMode::Custom;
  source.width = 4;
  source.height = 2;
  auto modeline = testModeline(2, 2);
  std::vector<uint8_t> rgb;
  CHECK(transformRgb24(frame, source, modeline, 0, rgb, error));
  CHECK(rgb.size() == 12 && rgb[0] == 1 && rgb[9] == 7);
  modeline.interlaced = true;
  CHECK(transformRgb24(frame, source, modeline, 0, rgb, error));
  CHECK(rgb.size() == 6 && rgb[0] == 5);
  CHECK(transformRgb24(frame, source, modeline, 1, rgb, error));
  CHECK(rgb.size() == 6 && rgb[0] == 1);
  source.progressiveInterlaceBuffer = true;
  CHECK(transformRgb24(frame, source, modeline, 1, rgb, error));
  CHECK(rgb.size() == 12 && rgb[0] == 1 && rgb[9] == 7);

  uint8_t pixel[] = {0x00, 0x00, 0xff, 0x00};
  Frame normalized;
  CHECK(normalizeToBgra(pixel, 4, 1, 1, 4, 32, 0xff0000, 0xff00, 0xff,
                        true, normalized, error));
  CHECK(normalized.bgra[0] == 0 && normalized.bgra[1] == 0 &&
        normalized.bgra[2] == 255);
}

int main() {
  checkSamplingTransforms();
  checkCropPointAndNormalization();
  if (failed) std::cerr << failed << " test(s) failed\n";
  return failed ? 1 : 0;
}
