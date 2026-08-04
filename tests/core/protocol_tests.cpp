#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "mistercast/groovy_protocol.hpp"

using namespace mistercast;
using testing::HasSubstr;
using testing::Optional;

namespace {

// hActive * vActive * 3 must fit the core's framebuffer. 644x644x3 is 1244208
// bytes, just inside the limit; one more line does not fit.
Modeline atFramebufferLimit() {
  return {"limit", 20, 644, 645, 646, 647, 644, 645, 646, 647, false};
}

TEST(ValidateGroovyModeline, AcceptsEveryBundledPreset) {
  for (const auto& preset : bundledModelines())
    EXPECT_FALSE(validateGroovyModeline(preset).has_value()) << preset.name;
}

TEST(ValidateGroovyModeline, ReportsPlainTimingFailuresFirst) {
  auto mode = Modeline::safeDefault();
  mode.vTotal = 1;
  EXPECT_THAT(validateGroovyModeline(mode),
              Optional(HasSubstr("vertical timings")));
}

TEST(ValidateGroovyModeline, RejectsAnActiveAreaLargerThanTheFramebuffer) {
  auto mode = atFramebufferLimit();
  EXPECT_FALSE(validateGroovyModeline(mode).has_value())
      << "an active area of exactly " << GroovyFramebufferBytes
      << " bytes or fewer must be accepted";

  mode.vActive = 645;
  mode.vBegin = 646;
  mode.vEnd = 647;
  mode.vTotal = 648;
  EXPECT_THAT(validateGroovyModeline(mode),
              Optional(HasSubstr("exceeds Groovy_MiSTer frame buffer")));
}

TEST(ValidateGroovyModeline, ChecksTheProductWithoutOverflowing) {
  // 4096x2160x3 is far beyond the framebuffer and also beyond 32 bits when
  // multiplied naively, so this pins the widening in the check itself.
  const Modeline huge{"huge",  100,  4096, 4097, 4098, 4099,
                      2160,   2161, 2162, 2163, false};
  EXPECT_THAT(validateGroovyModeline(huge),
              Optional(HasSubstr("exceeds Groovy_MiSTer frame buffer")));
}

TEST(ValidateGroovyConfig, AcceptsTheDefaultConfiguration) {
  EXPECT_FALSE(validateGroovyConfig(AppConfig{}).has_value());
}

TEST(ValidateGroovyConfig, ReportsPlainConfigurationFailuresFirst) {
  AppConfig config;
  config.version = 7;
  EXPECT_THAT(validateGroovyConfig(config), Optional(HasSubstr("version")));
}

TEST(ValidateGroovyConfig, RejectsAnActiveModelineTheCoreCannotHold) {
  AppConfig config;
  config.modeline = {"1024x768", 65.0,  1024, 1048, 1184,
                     1344,       768,  771,  777,  806, false};
  // Timings alone are legal, so only the protocol limit can reject this.
  EXPECT_FALSE(config.modeline.validate().has_value());
  EXPECT_THAT(validateGroovyConfig(config),
              Optional(HasSubstr("exceeds Groovy_MiSTer frame buffer")));
}

TEST(ValidateGroovyConfig, NamesACustomModelineTheCoreCannotHold) {
  AppConfig config;
  Modeline oversized{"Too big", 65.0,  1024, 1048, 1184,
                     1344,      768,  771,  777,  806, false};
  config.customModelines = {Modeline::safeDefault(), oversized};
  EXPECT_THAT(validateGroovyConfig(config),
              Optional(testing::AllOf(
                  HasSubstr("Too big"),
                  HasSubstr("exceeds Groovy_MiSTer frame buffer"))));
}

TEST(ProtocolConstants, DescribeAStandardEthernetPath) {
  // A 1472-byte payload plus the 20-byte IPv4 and 8-byte UDP headers is exactly
  // a 1500-byte MTU, which is what the pacing and MTU checks assume.
  EXPECT_EQ(GroovyUdpPayloadBytes + 20 + 8, 1500u);
  EXPECT_EQ(GroovyUdpWireOverheadBytes, 66u);
  EXPECT_EQ(GroovyFramebufferBytes, 1245312u);
}

}  // namespace
