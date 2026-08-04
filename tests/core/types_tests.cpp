#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "mistercast/types.hpp"

using namespace mistercast;
using testing::AllOf;
using testing::HasSubstr;
using testing::Optional;

namespace {

Modeline ntsc240p() { return Modeline::safeDefault(); }

TEST(Modeline, SafeDefaultIsAValidSixtyHertzMode) {
  const auto mode = ntsc240p();
  EXPECT_FALSE(mode.validate().has_value());
  EXPECT_EQ(mode.hActive, 320);
  EXPECT_EQ(mode.vActive, 240);
  EXPECT_FALSE(mode.interlaced);
  EXPECT_NEAR(mode.refreshHz(), 60.0, 1.0);
}

TEST(Modeline, InterlacedRefreshCountsFieldsRatherThanFrames) {
  auto progressive = ntsc240p();
  auto interlaced = progressive;
  interlaced.interlaced = true;
  EXPECT_DOUBLE_EQ(interlaced.refreshHz(), progressive.refreshHz() * 2);
}

TEST(Modeline, PixelClockMustBeFiniteAndWithinRange) {
  auto mode = ntsc240p();
  mode.pixelClockMHz = 0.1;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("pixel clock")));
  mode.pixelClockMHz = 400.5;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("pixel clock")));
  mode.pixelClockMHz = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("pixel clock")));
  mode.pixelClockMHz = std::numeric_limits<double>::infinity();
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("pixel clock")));
  mode.pixelClockMHz = 400.0;
  EXPECT_FALSE(mode.validate().has_value());
}

TEST(Modeline, HorizontalTimingsMustBeOrderedAndBounded) {
  auto mode = ntsc240p();
  mode.hActive = 0;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("horizontal")));
  mode = ntsc240p();
  mode.hActive = 4097;
  mode.hBegin = 4098;
  mode.hEnd = 4099;
  mode.hTotal = 4100;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("horizontal")));
  mode = ntsc240p();
  mode.hBegin = mode.hActive;  // needs active < begin
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("horizontal")));
  mode = ntsc240p();
  mode.hEnd = uint16_t(mode.hBegin - 1);  // needs begin <= end
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("horizontal")));
  mode = ntsc240p();
  mode.hTotal = mode.hEnd;  // needs end < total
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("horizontal")));
  mode = ntsc240p();
  mode.hEnd = mode.hBegin;  // begin == end is allowed
  EXPECT_FALSE(mode.validate().has_value());
}

TEST(Modeline, VerticalTimingsMustBeOrderedAndBounded) {
  auto mode = ntsc240p();
  mode.vActive = 0;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("vertical")));
  mode = ntsc240p();
  mode.vActive = 2161;
  mode.vBegin = 2162;
  mode.vEnd = 2163;
  mode.vTotal = 2164;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("vertical")));
  mode = ntsc240p();
  mode.vBegin = mode.vActive;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("vertical")));
  mode = ntsc240p();
  mode.vEnd = uint16_t(mode.vBegin - 1);
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("vertical")));
  mode = ntsc240p();
  mode.vTotal = mode.vEnd;
  EXPECT_THAT(mode.validate(), Optional(HasSubstr("vertical")));
}

TEST(SourceOptions, SizeMustBeWithinOneToEightThousandOneHundredNinetyTwo) {
  SourceOptions options;
  EXPECT_FALSE(options.validate().has_value());
  options.width = 0;
  EXPECT_THAT(options.validate(), Optional(HasSubstr("source size")));
  options = {};
  options.height = 0;
  EXPECT_THAT(options.validate(), Optional(HasSubstr("source size")));
  options = {};
  options.width = 8193;
  EXPECT_THAT(options.validate(), Optional(HasSubstr("source size")));
  options = {};
  options.height = 8193;
  EXPECT_THAT(options.validate(), Optional(HasSubstr("source size")));
  options = {};
  options.width = options.height = 8192;
  EXPECT_FALSE(options.validate().has_value());
}

TEST(SourceOptions, FrameDelayIsLimitedToTen) {
  SourceOptions options;
  options.frameDelay = 10;
  EXPECT_FALSE(options.validate().has_value());
  options.frameDelay = 11;
  EXPECT_THAT(options.validate(), Optional(HasSubstr("frame delay")));
}

TEST(SourceOptions, SamplingModeMustBeOneOfTheThreeSupportedFilters) {
  SourceOptions options;
  for (auto sampling : {SamplingMode::Point, SamplingMode::Bilinear,
                        SamplingMode::LineBlend}) {
    options.sampling = sampling;
    EXPECT_FALSE(options.validate().has_value());
  }
  options.sampling = static_cast<SamplingMode>(3);
  EXPECT_THAT(options.validate(), Optional(HasSubstr("sampling mode")));
}

TEST(AppConfig, RejectsUnsupportedVersion) {
  AppConfig config;
  EXPECT_FALSE(config.validate().has_value());
  config.version = 2;
  EXPECT_THAT(config.validate(), Optional(HasSubstr("version")));
}

TEST(AppConfig, PropagatesSourceAndModelineFailures) {
  AppConfig config;
  config.source.frameDelay = 99;
  EXPECT_THAT(config.validate(), Optional(HasSubstr("frame delay")));

  config = {};
  config.modeline.hTotal = 1;
  EXPECT_THAT(config.validate(), Optional(HasSubstr("horizontal")));
}

TEST(AppConfig, NamesTheCustomModelineThatFailedValidation) {
  AppConfig config;
  auto broken = Modeline::safeDefault();
  broken.name = "My broken preset";
  broken.vTotal = 1;
  config.customModelines = {Modeline::safeDefault(), broken};
  EXPECT_THAT(config.validate(),
              Optional(AllOf(HasSubstr("My broken preset"),
                             HasSubstr("vertical timings"))));
}

// Round-tripping every enumerator keeps toString and the parser from drifting
// apart, and covers each entry of both name tables.
template <class Enum>
struct EnumCase {
  Enum value;
  const char* name;
};

TEST(EnumNames, AlignmentRoundTrips) {
  const EnumCase<Alignment> cases[] = {
      {Alignment::Center, "center"},
      {Alignment::TopLeft, "top-left"},
      {Alignment::Top, "top"},
      {Alignment::TopRight, "top-right"},
      {Alignment::Right, "right"},
      {Alignment::BottomRight, "bottom-right"},
      {Alignment::Bottom, "bottom"},
      {Alignment::BottomLeft, "bottom-left"},
      {Alignment::Left, "left"}};
  for (const auto& test : cases) {
    EXPECT_EQ(toString(test.value), test.name);
    Alignment parsed{};
    ASSERT_TRUE(parseAlignment(test.name, parsed)) << test.name;
    EXPECT_EQ(parsed, test.value);
  }
  Alignment unchanged = Alignment::Right;
  EXPECT_FALSE(parseAlignment("diagonal", unchanged));
  EXPECT_EQ(unchanged, Alignment::Right);
  EXPECT_EQ(toString(static_cast<Alignment>(9)), "unknown");
}

TEST(EnumNames, CropModeRoundTrips) {
  const EnumCase<CropMode> cases[] = {
      {CropMode::Custom, "custom"}, {CropMode::X1, "1x"},
      {CropMode::X2, "2x"},         {CropMode::X3, "3x"},
      {CropMode::X4, "4x"},         {CropMode::X5, "5x"},
      {CropMode::Full43, "4:3"},    {CropMode::Full54, "5:4"}};
  for (const auto& test : cases) {
    EXPECT_EQ(toString(test.value), test.name);
    CropMode parsed{};
    ASSERT_TRUE(parseCropMode(test.name, parsed)) << test.name;
    EXPECT_EQ(parsed, test.value);
  }
  CropMode unchanged = CropMode::X2;
  EXPECT_FALSE(parseCropMode("16:9", unchanged));
  EXPECT_EQ(unchanged, CropMode::X2);
  EXPECT_EQ(toString(static_cast<CropMode>(8)), "unknown");
}

TEST(EnumNames, RotationRoundTrips) {
  const EnumCase<Rotation> cases[] = {{Rotation::None, "none"},
                                      {Rotation::CW90, "cw90"},
                                      {Rotation::CCW90, "ccw90"},
                                      {Rotation::Flip180, "180"}};
  for (const auto& test : cases) {
    EXPECT_EQ(toString(test.value), test.name);
    Rotation parsed{};
    ASSERT_TRUE(parseRotation(test.name, parsed)) << test.name;
    EXPECT_EQ(parsed, test.value);
  }
  Rotation unchanged = Rotation::CW90;
  EXPECT_FALSE(parseRotation("270", unchanged));
  EXPECT_EQ(unchanged, Rotation::CW90);
  EXPECT_EQ(toString(static_cast<Rotation>(4)), "unknown");
}

TEST(EnumNames, SamplingModeRoundTrips) {
  const EnumCase<SamplingMode> cases[] = {
      {SamplingMode::Point, "point"},
      {SamplingMode::Bilinear, "bilinear"},
      {SamplingMode::LineBlend, "line-blend"}};
  for (const auto& test : cases) {
    EXPECT_EQ(toString(test.value), test.name);
    SamplingMode parsed{};
    ASSERT_TRUE(parseSamplingMode(test.name, parsed)) << test.name;
    EXPECT_EQ(parsed, test.value);
  }
  SamplingMode unchanged = SamplingMode::Bilinear;
  EXPECT_FALSE(parseSamplingMode("area", unchanged));
  EXPECT_EQ(unchanged, SamplingMode::Bilinear);
  EXPECT_EQ(toString(static_cast<SamplingMode>(3)), "unknown");
}

TEST(ParseModeline, AcceptsTheDocumentedTenFieldForm) {
  Modeline mode;
  std::string error;
  ASSERT_TRUE(
      parseModeline("6.7 320 336 367 426 240 244 247 262 0", mode, error))
      << error;
  EXPECT_EQ(mode.name, "Custom");
  EXPECT_DOUBLE_EQ(mode.pixelClockMHz, 6.7);
  EXPECT_EQ(mode.hActive, 320);
  EXPECT_EQ(mode.hBegin, 336);
  EXPECT_EQ(mode.hEnd, 367);
  EXPECT_EQ(mode.hTotal, 426);
  EXPECT_EQ(mode.vActive, 240);
  EXPECT_EQ(mode.vBegin, 244);
  EXPECT_EQ(mode.vEnd, 247);
  EXPECT_EQ(mode.vTotal, 262);
  EXPECT_FALSE(mode.interlaced);
}

TEST(ParseModeline, InterlaceFlagOfOneSetsTheInterlacedMode) {
  Modeline mode;
  std::string error;
  ASSERT_TRUE(
      parseModeline("12.336 640 662 720 784 480 488 494 525 1", mode, error))
      << error;
  EXPECT_TRUE(mode.interlaced);
}

TEST(ParseModeline, RejectsMalformedInput) {
  Modeline mode;
  std::string error;
  EXPECT_FALSE(parseModeline("nonsense", mode, error));
  EXPECT_THAT(error, HasSubstr("modeline needs"));

  error.clear();
  EXPECT_FALSE(parseModeline("", mode, error));
  EXPECT_THAT(error, HasSubstr("modeline needs"));

  // Nine fields: one short of the required ten.
  error.clear();
  EXPECT_FALSE(
      parseModeline("6.7 320 336 367 426 240 244 247 262", mode, error));
  EXPECT_THAT(error, HasSubstr("modeline needs"));
}

TEST(ParseModeline, InterlaceFlagMustBeZeroOrOne) {
  Modeline mode;
  std::string error;
  EXPECT_FALSE(
      parseModeline("6.7 320 336 367 426 240 244 247 262 2", mode, error));
  EXPECT_THAT(error, HasSubstr("interlace"));
}

TEST(ParseModeline, RejectsTrailingText) {
  Modeline mode;
  std::string error;
  EXPECT_FALSE(parseModeline("6.7 320 336 367 426 240 244 247 262 0 extra",
                             mode, error));
  EXPECT_EQ(error, "unexpected text after modeline");
}

TEST(ParseModeline, RejectsTimingsThatFailValidation) {
  Modeline mode;
  std::string error;
  EXPECT_FALSE(
      parseModeline("6.7 320 336 367 300 240 244 247 262 0", mode, error));
  EXPECT_THAT(error, HasSubstr("horizontal timings"));
}

TEST(ParseModeline, LeavesTheOutputUntouchedOnFailure) {
  Modeline mode = Modeline::safeDefault();
  std::string error;
  EXPECT_FALSE(parseModeline("garbage", mode, error));
  EXPECT_EQ(mode.hActive, Modeline::safeDefault().hActive);
}

TEST(BundledModelines, AreAllValidAndUniquelyNamed) {
  const auto presets = bundledModelines();
  ASSERT_FALSE(presets.empty());
  std::vector<std::string> names;
  for (const auto& preset : presets) {
    EXPECT_FALSE(preset.validate().has_value()) << preset.name;
    EXPECT_FALSE(preset.name.empty());
    names.push_back(preset.name);
  }
  EXPECT_THAT(names, testing::Each(testing::Not(testing::IsEmpty())));
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::unique(names.begin(), names.end()), names.end())
      << "preset names must be unique so the GUI can select one by name";
  EXPECT_THAT(names, testing::Contains(Modeline::safeDefault().name));
}

TEST(BundledModelines, CoverBothProgressiveAndInterlacedRefreshFamilies) {
  bool sawInterlaced = false, sawProgressive = false, sawFifty = false,
       sawSixty = false;
  for (const auto& preset : bundledModelines()) {
    (preset.interlaced ? sawInterlaced : sawProgressive) = true;
    const auto refresh = preset.refreshHz();
    if (refresh > 49 && refresh < 51) sawFifty = true;
    if (refresh > 59 && refresh < 61) sawSixty = true;
  }
  EXPECT_TRUE(sawInterlaced);
  EXPECT_TRUE(sawProgressive);
  EXPECT_TRUE(sawFifty);
  EXPECT_TRUE(sawSixty);
}

}  // namespace
