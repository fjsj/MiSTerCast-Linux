#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "mistercast/transform.hpp"

using namespace mistercast;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::HasSubstr;

namespace {

Modeline testModeline(uint16_t width, uint16_t height,
                      bool interlaced = false) {
  return {"test", 1.0, width, uint16_t(width + 1), uint16_t(width + 2),
          uint16_t(width + 3), height, uint16_t(height + 1),
          uint16_t(height + 2), uint16_t(height + 3), interlaced};
}

// Every channel of every pixel gets the same value, so a transform's output can
// be compared as one value per pixel.
Frame grayFrame(uint32_t width, uint32_t height,
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

Frame rampFrame(uint32_t width, uint32_t height) {
  std::vector<uint8_t> values(size_t(width) * height);
  for (size_t i = 0; i < values.size(); ++i) values[i] = uint8_t(i);
  return grayFrame(width, height, values);
}

std::vector<uint8_t> perPixel(const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> result;
  for (size_t i = 0; i < rgb.size(); i += 3) result.push_back(rgb[i]);
  return result;
}

SourceOptions customCrop(uint16_t width = 320, uint16_t height = 240) {
  SourceOptions options;
  options.crop = CropMode::Custom;
  options.width = width;
  options.height = height;
  return options;
}

constexpr SamplingMode kAllSampling[] = {
    SamplingMode::Point, SamplingMode::Bilinear, SamplingMode::LineBlend};

// ---------------------------------------------------------------- calculateCrop

TEST(CalculateCrop, RejectsAnEmptyCaptureFrame) {
  CropRect rect;
  std::string error;
  const auto options = customCrop();
  const auto mode = Modeline::safeDefault();
  EXPECT_FALSE(calculateCrop(0, 1080, options, mode, rect, error));
  EXPECT_EQ(error, "capture frame has zero dimensions");
  EXPECT_FALSE(calculateCrop(1920, 0, options, mode, rect, error));
  EXPECT_EQ(error, "capture frame has zero dimensions");
}

TEST(CalculateCrop, RejectsACustomSizeOfZero) {
  CropRect rect;
  std::string error;
  auto options = customCrop(0, 0);
  EXPECT_FALSE(
      calculateCrop(1920, 1080, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(error, "crop is empty");
}

TEST(CalculateCrop, AQuarterTurnOfAOnePixelTallSourceHasNothingToCrop) {
  // The inverse aspect a rotated 4:3 result needs is 1 * 3 / 4, which is zero
  // columns; that has to be refused rather than streamed as an empty frame.
  CropRect rect;
  std::string error;
  auto options = customCrop();
  options.crop = CropMode::Full43;
  options.rotation = Rotation::CW90;
  EXPECT_FALSE(
      calculateCrop(100, 1, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(error, "crop is empty");
}

TEST(CalculateCrop, IntegerMultiplesFollowTheModelineActiveArea) {
  CropRect rect;
  std::string error;
  auto options = customCrop(64, 64);
  const auto mode = Modeline::safeDefault();  // 320x240 active
  const struct {
    CropMode crop;
    uint32_t width, height;
  } cases[] = {{CropMode::X1, 320, 240},
               {CropMode::X2, 640, 480},
               {CropMode::X3, 960, 720},
               {CropMode::X4, 1280, 960},
               {CropMode::X5, 1600, 1200}};
  for (const auto& test : cases) {
    options.crop = test.crop;
    ASSERT_TRUE(calculateCrop(3840, 2160, options, mode, rect, error)) << error;
    EXPECT_EQ(rect.width, test.width) << toString(test.crop);
    EXPECT_EQ(rect.height, test.height) << toString(test.crop);
  }
}

TEST(CalculateCrop, IntegerMultiplesAreClampedToTheSource) {
  CropRect rect;
  std::string error;
  auto options = customCrop();
  options.crop = CropMode::X5;
  ASSERT_TRUE(calculateCrop(800, 600, options, Modeline::safeDefault(), rect,
                            error));
  EXPECT_EQ(rect.width, 800u);
  EXPECT_EQ(rect.height, 600u);
}

TEST(CalculateCrop, FullAspectCropsUseTheWholeSourceHeight) {
  CropRect rect;
  std::string error;
  auto options = customCrop();
  options.crop = CropMode::Full43;
  ASSERT_TRUE(calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect,
                            error));
  EXPECT_EQ(rect.width, 2880u);
  EXPECT_EQ(rect.height, 2160u);

  options.crop = CropMode::Full54;
  ASSERT_TRUE(calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect,
                            error));
  EXPECT_EQ(rect.width, 2700u);
  EXPECT_EQ(rect.height, 2160u);
}

TEST(CalculateCrop, AQuarterTurnCarriesTheInverseAspect) {
  CropRect rect;
  std::string error;
  auto options = customCrop();
  options.crop = CropMode::Full43;
  for (auto rotation : {Rotation::CW90, Rotation::CCW90}) {
    options.rotation = rotation;
    ASSERT_TRUE(calculateCrop(3840, 2160, options, Modeline::safeDefault(),
                              rect, error));
    // 2160 * 3 / 4: cropping taller-than-wide so the rotated result is 4:3.
    EXPECT_EQ(rect.width, 1620u) << toString(rotation);
    EXPECT_EQ(rect.height, 2160u) << toString(rotation);
  }
  options.crop = CropMode::Full54;
  options.rotation = Rotation::CCW90;
  ASSERT_TRUE(
      calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.width, 1728u);  // 2160 * 4 / 5
}

TEST(CalculateCrop, AFullAspectCropIsClampedToANarrowSource) {
  CropRect rect;
  std::string error;
  auto options = customCrop();
  options.crop = CropMode::Full43;
  ASSERT_TRUE(
      calculateCrop(640, 1080, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.width, 640u);
  EXPECT_EQ(rect.height, 1080u);
}

TEST(CalculateCrop, PlacesTheRectangleForEveryAlignment) {
  CropRect rect;
  std::string error;
  auto options = customCrop(400, 200);
  const struct {
    Alignment alignment;
    uint32_t x, y;
  } cases[] = {{Alignment::Center, 300, 400},
               {Alignment::TopLeft, 0, 0},
               {Alignment::Top, 300, 0},
               {Alignment::TopRight, 600, 0},
               {Alignment::Right, 600, 400},
               {Alignment::BottomRight, 600, 800},
               {Alignment::Bottom, 300, 800},
               {Alignment::BottomLeft, 0, 800},
               {Alignment::Left, 0, 400}};
  for (const auto& test : cases) {
    options.alignment = test.alignment;
    ASSERT_TRUE(calculateCrop(1000, 1000, options, Modeline::safeDefault(),
                              rect, error))
        << toString(test.alignment);
    EXPECT_EQ(rect.x, test.x) << toString(test.alignment);
    EXPECT_EQ(rect.y, test.y) << toString(test.alignment);
    EXPECT_EQ(rect.width, 400u);
    EXPECT_EQ(rect.height, 200u);
  }
}

TEST(CalculateCrop, OffsetsMoveTheRectangleAndAreClampedIntoRange) {
  CropRect rect;
  std::string error;
  auto options = customCrop(640, 480);
  options.alignment = Alignment::TopLeft;
  options.xOffset = 100;
  options.yOffset = 50;
  ASSERT_TRUE(
      calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.x, 100u);
  EXPECT_EQ(rect.y, 50u);

  options.xOffset = 4000;
  options.yOffset = 2000;
  ASSERT_TRUE(
      calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.x, 3200u) << "clamped so the crop stays on screen";
  EXPECT_EQ(rect.y, 1680u);

  options.xOffset = -3000;
  options.yOffset = -2000;
  ASSERT_TRUE(
      calculateCrop(3840, 2160, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.x, 0u);
  EXPECT_EQ(rect.y, 0u);
}

TEST(CalculateCrop, ACropFillingTheSourceLeavesNoRoomForOffsets) {
  CropRect rect;
  std::string error;
  auto options = customCrop(100, 100);
  options.xOffset = 40;
  options.yOffset = 40;
  ASSERT_TRUE(
      calculateCrop(100, 100, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.x, 0u);
  EXPECT_EQ(rect.y, 0u);
}

// -------------------------------------------------------------- transformRgb24

TEST(TransformRgb24, RejectsAnInvalidModeline) {
  auto frame = grayFrame(1, 1, {42});
  auto options = customCrop(1, 1);
  auto mode = testModeline(1, 1);
  mode.hTotal = 1;
  std::vector<uint8_t> rgb;
  std::string error;
  EXPECT_FALSE(transformRgb24(frame, {0, 0, 1, 1}, options, mode, 0, rgb,
                              error));
  EXPECT_THAT(error, HasSubstr("horizontal timings"));
}

TEST(TransformRgb24, RejectsAMalformedPixelBuffer) {
  auto options = customCrop(2, 4);
  const auto mode = testModeline(2, 4);
  std::vector<uint8_t> rgb;
  std::string error;

  auto truncated = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  truncated.bgra.pop_back();
  EXPECT_FALSE(transformRgb24(truncated, {0, 0, 2, 4}, options, mode, 0, rgb,
                              error));
  EXPECT_EQ(error, "invalid BGRA frame buffer");

  auto shortStride = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  shortStride.stride = 4;  // less than width * 4
  EXPECT_FALSE(transformRgb24(shortStride, {0, 0, 2, 4}, options, mode, 0, rgb,
                              error));
  EXPECT_EQ(error, "invalid BGRA frame buffer");
}

TEST(TransformRgb24, RejectsACropOutsideTheFrame) {
  const auto frame = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  const auto options = customCrop(2, 4);
  const auto mode = testModeline(2, 4);
  std::vector<uint8_t> rgb;
  std::string error;
  // An empty rectangle, and rectangles running past the right and bottom edges.
  for (const CropRect crop : {CropRect{0, 0, 0, 4}, CropRect{0, 0, 2, 0},
                              CropRect{1, 0, 2, 4}, CropRect{0, 1, 2, 4}}) {
    EXPECT_FALSE(transformRgb24(frame, crop, options, mode, 0, rgb, error));
    EXPECT_EQ(error, "crop rectangle lies outside the captured frame");
  }
}

TEST(TransformRgb24, RejectsAnUnsupportedSamplingMode) {
  const auto frame = grayFrame(1, 1, {42});
  auto options = customCrop(1, 1);
  options.sampling = static_cast<SamplingMode>(200);
  std::vector<uint8_t> rgb;
  std::string error;
  EXPECT_FALSE(transformRgb24(frame, {0, 0, 1, 1}, options, testModeline(1, 1),
                              0, rgb, error));
  EXPECT_EQ(error, "unsupported sampling mode");
}

TEST(TransformRgb24, RejectsAnInterlacedModeWithASingleActiveLine) {
  const auto frame = grayFrame(2, 2, {0, 1, 2, 3});
  const auto options = customCrop(2, 2);
  auto mode = testModeline(2, 1, /*interlaced=*/true);
  std::vector<uint8_t> rgb;
  std::string error;
  EXPECT_FALSE(transformRgb24(frame, {0, 0, 2, 2}, options, mode, 0, rgb,
                              error));
  EXPECT_EQ(error, "interlaced active height must be at least two");
}

TEST(CalculateCrop, AnUnknownAlignmentPlacesTheCropAtTheOrigin) {
  CropRect rect;
  std::string error;
  auto options = customCrop(400, 200);
  options.alignment = static_cast<Alignment>(200);
  ASSERT_TRUE(
      calculateCrop(1000, 1000, options, Modeline::safeDefault(), rect, error));
  EXPECT_EQ(rect.x, 0u);
  EXPECT_EQ(rect.y, 0u);
  EXPECT_EQ(rect.width, 400u);
}

TEST(TransformRgb24, AnUnknownRotationIsTreatedAsNoRotation) {
  const auto frame = grayFrame(2, 3, {0, 1, 2, 3, 4, 5});
  auto options = customCrop(2, 3);
  options.rotation = static_cast<Rotation>(200);
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    options.sampling = sampling;
    ASSERT_TRUE(transformRgb24(frame, {0, 0, 2, 3}, options, testModeline(2, 3),
                               0, rgb, error))
        << error;
    EXPECT_THAT(perPixel(rgb), ElementsAre(0, 1, 2, 3, 4, 5))
        << toString(sampling);
  }
}

TEST(TransformRgb24, BilinearHandlesAWeightThatRoundsToAWholeTexel) {
  // Reducing 257 columns to 256 makes the last output column's interpolation
  // weight round to exactly 1.0, which has to advance to the next texel rather
  // than blend with a weight the fixed-point maths cannot represent.
  std::vector<uint8_t> ramp(257);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = uint8_t(i * 255 / 256);
  const auto frame = grayFrame(257, 1, ramp);
  auto options = customCrop(257, 1);
  options.sampling = SamplingMode::Bilinear;
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 257, 1}, options,
                             testModeline(256, 1), 0, rgb, error))
      << error;
  const auto columns = perPixel(rgb);
  ASSERT_EQ(columns.size(), 256u);
  EXPECT_EQ(columns.front(), ramp.front());
  EXPECT_EQ(columns.back(), ramp.back());
  for (size_t i = 1; i < columns.size(); ++i)
    ASSERT_GE(columns[i], columns[i - 1]) << "a ramp must stay monotonic at " << i;
}

TEST(TransformRgb24, PointSamplingPicksNearestNeighbours) {
  const auto frame = rampFrame(4, 4);
  auto options = customCrop(4, 4);
  options.sampling = SamplingMode::Point;
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 4, 4}, options, testModeline(2, 2),
                             0, rgb, error));
  // Each output cell samples the source texel under its centre: rows 1 and 3,
  // columns 1 and 3 of the 4x4 ramp.
  EXPECT_THAT(perPixel(rgb), ElementsAre(5, 7, 13, 15));
}

TEST(TransformRgb24, BilinearBlendsTheFourNeighbours) {
  auto options = customCrop(2, 2);
  options.sampling = SamplingMode::Bilinear;
  std::vector<uint8_t> rgb;
  std::string error;

  const auto quad = grayFrame(2, 2, {0, 64, 128, 255});
  ASSERT_TRUE(
      transformRgb24(quad, {0, 0, 2, 2}, options, testModeline(1, 1), 0, rgb,
                     error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(112));

  const auto pair = grayFrame(2, 1, {10, 110});
  ASSERT_TRUE(transformRgb24(pair, {0, 0, 2, 1}, options, testModeline(4, 1), 0,
                             rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(10, 35, 85, 110));
}

TEST(TransformRgb24, BilinearHandlesALargeReduction) {
  auto frame = grayFrame(18, 9, std::vector<uint8_t>(162));
  for (uint32_t y = 0; y < 9; ++y)
    for (uint32_t x = 0; x < 18; ++x)
      for (unsigned channel = 0; channel < 3; ++channel)
        frame.bgra[(size_t(y) * 18 + x) * 4 + channel] = uint8_t(y * 18 + x);
  auto options = customCrop(18, 9);
  options.sampling = SamplingMode::Bilinear;
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 18, 9}, options, testModeline(2, 1),
                             0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(76, 85));
}

TEST(TransformRgb24, LineBlendAveragesTheLinesThatFallInEachOutputRow) {
  auto options = customCrop(1, 4);
  options.sampling = SamplingMode::LineBlend;
  std::vector<uint8_t> rgb;
  std::string error;

  auto frame = grayFrame(1, 4, {10, 30, 50, 70});
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 1, 4}, options, testModeline(1, 2),
                             0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(20, 60));

  // A fractional 5:2 ratio splits one source line's weight across two rows.
  frame = grayFrame(1, 5, {0, 10, 20, 30, 40});
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 1, 5}, options, testModeline(1, 2),
                             0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(8, 32));
}

TEST(TransformRgb24, LineBlendEnlargesWithoutBanding) {
  auto options = customCrop(1, 2);
  options.sampling = SamplingMode::LineBlend;
  std::vector<uint8_t> rgb;
  std::string error;
  const auto frame = grayFrame(1, 2, {20, 80});
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 1, 2}, options, testModeline(1, 3),
                             0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(20, 50, 80));
}

TEST(TransformRgb24, LineBlendOfAFlatImageIsExactlyThatValue) {
  // Fixed-point reciprocal division must not drift on any ratio, so a constant
  // input has to come out unchanged rather than off by one.
  auto options = customCrop(3, 7);
  options.sampling = SamplingMode::LineBlend;
  std::vector<uint8_t> rgb;
  std::string error;
  for (uint32_t sourceHeight = 1; sourceHeight <= 40; ++sourceHeight) {
    const auto frame =
        grayFrame(3, sourceHeight, std::vector<uint8_t>(3 * sourceHeight, 123));
    for (uint16_t outputHeight : {1, 2, 3, 4, 5, 7, 8, 11, 16, 23}) {
      ASSERT_TRUE(transformRgb24(frame, {0, 0, 3, sourceHeight}, options,
                                 testModeline(2, outputHeight), 0, rgb, error))
          << error;
      EXPECT_THAT(rgb, testing::Each(uint8_t(123)))
          << sourceHeight << " lines into " << outputHeight;
    }
  }
}

TEST(TransformRgb24, LineBlendReducesAFullHdColumnCorrectly) {
  std::vector<uint8_t> lines(1080);
  for (size_t y = 0; y < lines.size(); ++y) lines[y] = uint8_t(y % 251);
  const auto frame = grayFrame(1, 1080, lines);
  auto options = customCrop(1, 1080);
  options.sampling = SamplingMode::LineBlend;
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 1, 1080}, options,
                             testModeline(1, 240), 0, rgb, error));
  const auto rows = perPixel(rgb);
  ASSERT_EQ(rows.size(), 240u);
  EXPECT_EQ(rows[0], 2);
  EXPECT_EQ(rows[1], 6);
}

TEST(TransformRgb24, EveryFilterRotatesTheSameWay) {
  const auto frame = grayFrame(2, 3, {0, 1, 2, 3, 4, 5});
  const struct {
    Rotation rotation;
    uint16_t width, height;
    std::vector<uint8_t> expected;
  } rotations[] = {
      {Rotation::None, 2, 3, {0, 1, 2, 3, 4, 5}},
      {Rotation::Flip180, 2, 3, {5, 4, 3, 2, 1, 0}},
      {Rotation::CW90, 3, 2, {4, 2, 0, 5, 3, 1}},
      {Rotation::CCW90, 3, 2, {1, 3, 5, 0, 2, 4}},
  };
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    auto options = customCrop(2, 3);
    options.sampling = sampling;
    for (const auto& test : rotations) {
      options.rotation = test.rotation;
      ASSERT_TRUE(transformRgb24(frame, {0, 0, 2, 3}, options,
                                 testModeline(test.width, test.height), 0, rgb,
                                 error))
          << error;
      EXPECT_THAT(perPixel(rgb), ElementsAreArray(test.expected))
          << toString(sampling) << " / " << toString(test.rotation);
    }
  }
}

TEST(TransformRgb24, FieldBufferSendsAlternatingHalfHeightFields) {
  const auto frame = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    auto options = customCrop(2, 4);
    options.sampling = sampling;
    options.progressiveInterlaceBuffer = false;
    const auto mode = testModeline(2, 4, /*interlaced=*/true);

    ASSERT_TRUE(
        transformRgb24(frame, {0, 0, 2, 4}, options, mode, 0, rgb, error));
    EXPECT_THAT(perPixel(rgb), ElementsAre(2, 3, 6, 7)) << toString(sampling);
    ASSERT_TRUE(
        transformRgb24(frame, {0, 0, 2, 4}, options, mode, 1, rgb, error));
    EXPECT_THAT(perPixel(rgb), ElementsAre(0, 1, 4, 5)) << toString(sampling);
  }
}

TEST(TransformRgb24, TheProgressiveBufferSendsEveryLineRegardlessOfField) {
  const auto frame = grayFrame(2, 4, {0, 1, 2, 3, 4, 5, 6, 7});
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    auto options = customCrop(2, 4);
    options.sampling = sampling;
    options.progressiveInterlaceBuffer = true;
    const auto mode = testModeline(2, 4, /*interlaced=*/true);
    for (uint8_t field : {0, 1}) {
      ASSERT_TRUE(transformRgb24(frame, {0, 0, 2, 4}, options, mode, field, rgb,
                                 error));
      EXPECT_THAT(perPixel(rgb), ElementsAre(0, 1, 2, 3, 4, 5, 6, 7))
          << toString(sampling) << " field " << unsigned(field);
    }
  }
}

TEST(TransformRgb24, TheCropDerivingOverloadSamplesTheComputedRectangle) {
  const auto frame = grayFrame(4, 2, {0, 1, 2, 3, 4, 5, 6, 7});
  auto options = customCrop(4, 2);
  auto mode = testModeline(2, 2);
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, options, mode, 0, rgb, error));
  EXPECT_EQ(rgb.size(), 12u);
  EXPECT_THAT(perPixel(rgb), ElementsAre(1, 3, 5, 7));

  mode.interlaced = true;
  ASSERT_TRUE(transformRgb24(frame, options, mode, 0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(5, 7));
  ASSERT_TRUE(transformRgb24(frame, options, mode, 1, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(1, 3));

  options.progressiveInterlaceBuffer = true;
  ASSERT_TRUE(transformRgb24(frame, options, mode, 1, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(1, 3, 5, 7));
}

TEST(TransformRgb24, TheCropDerivingOverloadReportsCropFailures) {
  Frame empty;
  const auto options = customCrop();
  std::vector<uint8_t> rgb;
  std::string error;
  EXPECT_FALSE(transformRgb24(empty, options, Modeline::safeDefault(), 0, rgb,
                              error));
  EXPECT_EQ(error, "capture frame has zero dimensions");
}

TEST(TransformRgb24, SamplesOnlyInsideAnOffsetCropRectangle) {
  const auto frame = rampFrame(4, 4);
  auto options = customCrop(2, 2);
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    options.sampling = sampling;
    ASSERT_TRUE(transformRgb24(frame, {2, 2, 2, 2}, options, testModeline(2, 2),
                               0, rgb, error));
    // Rows 2 and 3, columns 2 and 3 of the ramp.
    EXPECT_THAT(perPixel(rgb), ElementsAre(10, 11, 14, 15))
        << toString(sampling);
  }
}

TEST(TransformRgb24, ToleratesPaddingBetweenRows) {
  Frame frame;
  frame.width = 2;
  frame.height = 2;
  frame.stride = 2 * 4 + 16;  // padded rows, as a real X11 image can have
  frame.bgra.assign(size_t(frame.stride) * frame.height, 0);
  const uint8_t values[2][2] = {{10, 20}, {30, 40}};
  for (uint32_t y = 0; y < 2; ++y)
    for (uint32_t x = 0; x < 2; ++x)
      for (unsigned channel = 0; channel < 3; ++channel)
        frame.bgra[size_t(y) * frame.stride + x * 4 + channel] = values[y][x];
  auto options = customCrop(2, 2);
  std::vector<uint8_t> rgb;
  std::string error;
  ASSERT_TRUE(transformRgb24(frame, {0, 0, 2, 2}, options, testModeline(2, 2),
                             0, rgb, error));
  EXPECT_THAT(perPixel(rgb), ElementsAre(10, 20, 30, 40));
}

TEST(TransformRgb24, PreservesChannelOrderFromBgraToBgr) {
  Frame frame;
  frame.width = frame.height = 1;
  frame.stride = 4;
  frame.bgra = {0x11, 0x22, 0x33, 0xff};
  auto options = customCrop(1, 1);
  std::vector<uint8_t> rgb;
  std::string error;
  for (auto sampling : kAllSampling) {
    options.sampling = sampling;
    ASSERT_TRUE(transformRgb24(frame, {0, 0, 1, 1}, options, testModeline(1, 1),
                               0, rgb, error));
    EXPECT_THAT(rgb, ElementsAre(0x11, 0x22, 0x33)) << toString(sampling);
  }
}

// ------------------------------------------------------------- normalizeToBgra

TEST(NormalizeToBgra, TakesTheNativeThirtyTwoBitFastPath) {
  const uint8_t pixels[] = {0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 2, 1, 8, 32, 0xff0000,
                              0xff00, 0xff, true, frame, error))
      << error;
  EXPECT_EQ(frame.width, 2u);
  EXPECT_EQ(frame.height, 1u);
  EXPECT_EQ(frame.stride, 8u);
  EXPECT_EQ(frame.sequence, 0u);
  // The pipeline ignores the X padding byte, so the fast path copies whatever
  // was there rather than spending a pass writing an opaque alpha.
  EXPECT_THAT(frame.bgra,
              ElementsAre(0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00, 0x00));
}

TEST(NormalizeToBgra, TheFastPathCopiesRowByRowWhenTheSourceIsPadded) {
  const uint8_t pixels[] = {0x01, 0x02, 0x03, 0xff, 0xaa, 0xbb,
                            0x04, 0x05, 0x06, 0xff, 0xcc, 0xdd};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 1, 2, 6, 32, 0xff0000,
                              0xff00, 0xff, true, frame, error))
      << error;
  EXPECT_EQ(frame.stride, 4u);
  EXPECT_THAT(frame.bgra, ElementsAre(0x01, 0x02, 0x03, 0xff, 0x04, 0x05, 0x06,
                                      0xff));
}

TEST(NormalizeToBgra, ExpandsSixteenBitRgb565) {
  // Red 0xf800, green 0x07e0, blue 0x001f: pure red then pure blue.
  const uint8_t pixels[] = {0x00, 0xf8, 0x1f, 0x00};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 2, 1, 4, 16, 0xf800,
                              0x07e0, 0x001f, true, frame, error))
      << error;
  EXPECT_THAT(frame.bgra,
              ElementsAre(0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff));
}

TEST(NormalizeToBgra, ExpandsTwentyFourBitPixels) {
  const uint8_t pixels[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 2, 1, 6, 24, 0xff0000,
                              0xff00, 0xff, true, frame, error))
      << error;
  EXPECT_THAT(frame.bgra,
              ElementsAre(0x10, 0x20, 0x30, 0xff, 0x40, 0x50, 0x60, 0xff));
}

TEST(NormalizeToBgra, HonoursMostSignificantByteFirstServers) {
  const uint8_t pixels[] = {0x00, 0xff, 0x00, 0x00};
  Frame lsb, msb;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 1, 1, 4, 32, 0xff0000,
                              0xff00, 0xff, true, lsb, error));
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 1, 1, 4, 32, 0xff0000,
                              0xff00, 0xff, false, msb, error));
  // The same bytes read in the other order name a different channel. Only the
  // little-endian case takes the row-copy fast path, which passes the source
  // padding byte through untouched instead of writing an opaque alpha.
  EXPECT_THAT(std::vector<uint8_t>(lsb.bgra.begin(), lsb.bgra.begin() + 3),
              ElementsAre(0x00, 0xff, 0x00));
  EXPECT_THAT(msb.bgra, ElementsAre(0x00, 0x00, 0xff, 0xff));
}

TEST(NormalizeToBgra, ThirtyTwoBitPixelsWithOtherMasksTakeTheGenericPath) {
  // Only the native BGRX layout can be row-copied; a swapped mask has to be
  // expanded channel by channel, alpha included.
  const uint8_t pixels[] = {0x11, 0x22, 0x33, 0x44};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 1, 1, 4, 32,
                              /*redMask=*/0x000000ff, /*greenMask=*/0x0000ff00,
                              /*blueMask=*/0x00ff0000, true, frame, error))
      << error;
  EXPECT_THAT(frame.bgra, ElementsAre(0x33, 0x22, 0x11, 0xff));
}

TEST(NormalizeToBgra, AMissingChannelMaskYieldsZeroForThatChannel) {
  const uint8_t pixels[] = {0xff, 0xff, 0xff, 0xff};
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels, sizeof(pixels), 1, 1, 4, 32, 0xff0000,
                              0x00, 0xff, true, frame, error))
      << error;
  EXPECT_EQ(frame.bgra[1], 0) << "no green mask means no green";
  EXPECT_EQ(frame.bgra[0], 0xff);
  EXPECT_EQ(frame.bgra[2], 0xff);
}

TEST(NormalizeToBgra, RejectsUnsupportedPixelDepths) {
  const uint8_t pixels[64]{};
  Frame frame;
  std::string error;
  for (uint8_t bitsPerPixel : {1, 8, 40, 64}) {
    EXPECT_FALSE(normalizeToBgra(pixels, sizeof(pixels), 1, 1, 8, bitsPerPixel,
                                 0xff0000, 0xff00, 0xff, true, frame, error))
        << unsigned(bitsPerPixel);
    EXPECT_EQ(error, "unsupported or truncated X11 pixel buffer");
  }
}

TEST(NormalizeToBgra, RejectsAStrideNarrowerThanTheRow) {
  const uint8_t pixels[64]{};
  Frame frame;
  std::string error;
  EXPECT_FALSE(normalizeToBgra(pixels, sizeof(pixels), 4, 1, 8, 32, 0xff0000,
                               0xff00, 0xff, true, frame, error));
  EXPECT_EQ(error, "unsupported or truncated X11 pixel buffer");
}

TEST(NormalizeToBgra, RejectsATruncatedBuffer) {
  const uint8_t pixels[4]{};
  Frame frame;
  std::string error;
  EXPECT_FALSE(normalizeToBgra(pixels, sizeof(pixels), 1, 2, 4, 32, 0xff0000,
                               0xff00, 0xff, true, frame, error));
  EXPECT_EQ(error, "unsupported or truncated X11 pixel buffer");
}

TEST(NormalizeToBgra, ReusesTheCallersBufferWithoutReallocatingPerFrame) {
  std::vector<uint8_t> pixels(size_t(64) * 64 * 4, 0x7f);
  Frame frame;
  std::string error;
  ASSERT_TRUE(normalizeToBgra(pixels.data(), pixels.size(), 64, 64, 64 * 4, 32,
                              0xff0000, 0xff00, 0xff, true, frame, error));
  const auto* data = frame.bgra.data();
  const auto capacity = frame.bgra.capacity();
  frame.sequence = 99;
  ASSERT_TRUE(normalizeToBgra(pixels.data(), pixels.size(), 64, 64, 64 * 4, 32,
                              0xff0000, 0xff00, 0xff, true, frame, error));
  EXPECT_EQ(frame.bgra.data(), data) << "a same-sized frame must not reallocate";
  EXPECT_EQ(frame.bgra.capacity(), capacity);
  EXPECT_EQ(frame.sequence, 0u) << "normalize resets the sequence number";
}

}  // namespace
