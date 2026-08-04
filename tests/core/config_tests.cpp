#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_directory.hpp"

using namespace mistercast;
using mistercast::test::ScopedEnvironment;
using mistercast::test::TemporaryDirectory;
using testing::HasSubstr;
using testing::IsEmpty;

namespace {

AppConfig populatedConfig() {
  AppConfig config;
  config.target = "mister.local";
  config.source.monitor = "DP-0";
  config.source.capturePreference = CapturePreference::Window;
  config.source.audioSink = "alsa_output.pci-0000_00_1f.3.analog-stereo";
  config.source.syncRefresh = false;
  config.source.progressiveInterlaceBuffer = true;
  config.source.audio = false;
  config.source.preview = false;
  config.source.frameDelay = 7;
  config.source.width = 640;
  config.source.height = 480;
  config.source.xOffset = -12;
  config.source.yOffset = 34;
  config.source.alignment = Alignment::BottomRight;
  config.source.crop = CropMode::X2;
  config.source.rotation = Rotation::CCW90;
  config.source.sampling = SamplingMode::LineBlend;
  config.modeline = bundledModelines().at(3);
  return config;
}

TEST(ConfigPath, PrefersXdgConfigHome) {
  const ScopedEnvironment xdg("XDG_CONFIG_HOME", "/xdg-root");
  EXPECT_EQ(configPath(),
            std::filesystem::path("/xdg-root/mistercast/config.json"));
}

TEST(ConfigPath, FallsBackToHomeWhenXdgIsUnsetOrEmpty) {
  const ScopedEnvironment home("HOME", "/home/tester");
  {
    const ScopedEnvironment xdg("XDG_CONFIG_HOME", nullptr);
    EXPECT_EQ(configPath(),
              std::filesystem::path("/home/tester/.config/mistercast/config.json"));
  }
  const ScopedEnvironment empty("XDG_CONFIG_HOME", "");
  EXPECT_EQ(configPath(),
            std::filesystem::path("/home/tester/.config/mistercast/config.json"));
}

TEST(ConfigPath, FallsBackToTheWorkingDirectoryWithNoEnvironmentAtAll) {
  const ScopedEnvironment xdg("XDG_CONFIG_HOME", nullptr);
  const ScopedEnvironment home("HOME", nullptr);
  EXPECT_EQ(configPath(), std::filesystem::current_path() / "config.json");

  const ScopedEnvironment emptyHome("HOME", "");
  EXPECT_EQ(configPath(), std::filesystem::current_path() / "config.json");
}

TEST(SaveAndLoad, RoundTripsEverySetting) {
  const TemporaryDirectory directory("config-roundtrip");
  const auto path = directory.file("config.json");
  const auto saved = populatedConfig();
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(saved, path, error)) << error;

  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(warning, IsEmpty());
  EXPECT_EQ(loaded.version, 1u);
  EXPECT_EQ(loaded.target, saved.target);
  EXPECT_EQ(loaded.source.monitor, saved.source.monitor);
  EXPECT_EQ(loaded.source.capturePreference, CapturePreference::Window);
  EXPECT_EQ(loaded.source.audioSink, saved.source.audioSink);
  EXPECT_EQ(loaded.source.syncRefresh, false);
  EXPECT_TRUE(loaded.source.progressiveInterlaceBuffer);
  EXPECT_FALSE(loaded.source.audio);
  EXPECT_FALSE(loaded.source.preview);
  EXPECT_EQ(loaded.source.frameDelay, 7);
  EXPECT_EQ(loaded.source.width, 640);
  EXPECT_EQ(loaded.source.height, 480);
  EXPECT_EQ(loaded.source.xOffset, -12);
  EXPECT_EQ(loaded.source.yOffset, 34);
  EXPECT_EQ(loaded.source.alignment, Alignment::BottomRight);
  EXPECT_EQ(loaded.source.crop, CropMode::X2);
  EXPECT_EQ(loaded.source.rotation, Rotation::CCW90);
  EXPECT_EQ(loaded.source.sampling, SamplingMode::LineBlend);
  EXPECT_EQ(loaded.modeline.hActive, saved.modeline.hActive);
  EXPECT_EQ(loaded.modeline.vTotal, saved.modeline.vTotal);
  EXPECT_EQ(loaded.modeline.interlaced, saved.modeline.interlaced);
  EXPECT_DOUBLE_EQ(loaded.modeline.pixelClockMHz, saved.modeline.pixelClockMHz);
  EXPECT_EQ(loaded.modeline.name, saved.modeline.name);
}

TEST(SaveAndLoad, RoundTripsMonitorCapturePreference) {
  const TemporaryDirectory directory("config-monitor-mode");
  auto config = populatedConfig();
  config.source.capturePreference = CapturePreference::Monitor;
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(config, directory.file("c.json"), error));
  EXPECT_EQ(loadGroovyConfig(directory.file("c.json")).source.capturePreference,
            CapturePreference::Monitor);
}

TEST(SaveAndLoad, NeverPersistsTheTransientWindowSelection) {
  const TemporaryDirectory directory("config-no-window");
  const auto path = directory.file("config.json");
  auto config = populatedConfig();
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(config, path, error));
  const auto json = TemporaryDirectory::read(path);
  EXPECT_THAT(json, testing::Not(HasSubstr("windowId")));
  EXPECT_THAT(json, testing::Not(HasSubstr("windowTitle")));
}

TEST(SaveAndLoad, RoundTripsSeveralCustomModelines) {
  const TemporaryDirectory directory("config-customs");
  const auto path = directory.file("config.json");
  auto config = populatedConfig();
  auto first = Modeline::safeDefault();
  first.name = "First preset";
  auto second = bundledModelines().at(2);
  second.name = "Second preset";
  config.customModelines = {first, second};
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(config, path, error)) << error;

  const auto loaded = loadGroovyConfig(path);
  ASSERT_EQ(loaded.customModelines.size(), 2u);
  EXPECT_EQ(loaded.customModelines[0].name, "First preset");
  EXPECT_EQ(loaded.customModelines[1].name, "Second preset");
  EXPECT_TRUE(loaded.customModelines[1].interlaced);
}

TEST(SaveAndLoad, EscapesQuotesBackslashesAndNewlinesInStrings) {
  const TemporaryDirectory directory("config-escapes");
  const auto path = directory.file("config.json");
  AppConfig config;
  config.target = "host";
  config.source.monitor = R"(quote" backslash\ )"
                          "\n"
                          "after";
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(config, path, error)) << error;
  const auto json = TemporaryDirectory::read(path);
  EXPECT_THAT(json, HasSubstr(R"(backslash\\)"));
  EXPECT_THAT(json, HasSubstr(R"(quote\")"));
  EXPECT_THAT(json, testing::Not(HasSubstr("backslash\\ \nafter")))
      << "a literal newline must not be written into a JSON string";
  EXPECT_THAT(json, HasSubstr(R"(\n)"));
}

TEST(SaveGroovyConfig, RefusesAConfigurationTheProtocolCannotStream) {
  const TemporaryDirectory directory("config-invalid");
  const auto path = directory.file("config.json");
  auto config = populatedConfig();
  config.modeline = Modeline{"1024x768", 65.0,  1024, 1048, 1184,
                             1344,       768,  771,  777,  806, false};
  std::string error;
  EXPECT_FALSE(saveGroovyConfig(config, path, error));
  EXPECT_EQ(error, "active image exceeds Groovy_MiSTer frame buffer");
  EXPECT_FALSE(std::filesystem::exists(path))
      << "a rejected configuration must not be written at all";
}

TEST(SaveGroovyConfig, ReportsAnUncreatableParentDirectory) {
  const TemporaryDirectory directory("config-not-a-dir");
  const auto blocker = directory.write("blocker", "not a directory");
  std::string error;
  EXPECT_FALSE(
      saveGroovyConfig(AppConfig{}, blocker / "nested" / "config.json", error));
  EXPECT_THAT(error, testing::Not(IsEmpty()));
}

TEST(SaveGroovyConfig, ReportsAnUnwritableDirectory) {
  const TemporaryDirectory directory("config-readonly");
  const auto nested = directory.file("readonly");
  std::filesystem::create_directories(nested);
  std::filesystem::permissions(nested, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_exec);
  std::string error;
  const bool saved =
      saveGroovyConfig(AppConfig{}, nested / "config.json", error);
  std::filesystem::permissions(nested, std::filesystem::perms::owner_all);
  if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
  EXPECT_FALSE(saved);
  EXPECT_EQ(error, "cannot create temporary configuration");
}

TEST(SaveGroovyConfig, ReportsAFailedAtomicReplacement) {
  const TemporaryDirectory directory("config-rename");
  // A non-empty directory occupying the destination name cannot be replaced by
  // rename(2), which is the last failure the atomic write can hit.
  const auto path = directory.file("config.json");
  std::filesystem::create_directories(path);
  directory.write("config.json/occupied", "x");
  std::string error;
  EXPECT_FALSE(saveGroovyConfig(AppConfig{}, path, error));
  EXPECT_THAT(error, testing::Not(IsEmpty()));
}

TEST(LoadGroovyConfig, AMissingFileYieldsDefaultsWithoutAWarning) {
  const TemporaryDirectory directory("config-missing");
  std::string warning = "stale";
  const auto loaded = loadGroovyConfig(directory.file("absent.json"), &warning);
  EXPECT_THAT(warning, IsEmpty()) << "a previous warning must be cleared";
  EXPECT_EQ(loaded.target, "");
  EXPECT_EQ(loaded.modeline.hActive, Modeline::safeDefault().hActive);
}

TEST(LoadGroovyConfig, WorksWithoutAWarningPointer) {
  const TemporaryDirectory directory("config-no-warning-out");
  const auto path = directory.write("config.json", "not json at all");
  EXPECT_EQ(loadGroovyConfig(path).target, "");
}

TEST(LoadGroovyConfig, UnparsableContentFallsBackToSafeDefaults) {
  const TemporaryDirectory directory("config-broken");
  const auto path = directory.write("config.json", "broken");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(loaded.target, IsEmpty());
  EXPECT_THAT(warning, HasSubstr("safe defaults"));
}

TEST(LoadGroovyConfig, AnUnsupportedVersionFallsBackToSafeDefaults) {
  const TemporaryDirectory directory("config-version");
  const auto path = directory.write(
      "config.json", R"({"version": 2, "target": "must-not-load"})");
  std::string warning;
  EXPECT_THAT(loadGroovyConfig(path, &warning).target, IsEmpty());
  EXPECT_THAT(warning, HasSubstr("unsupported config"));
}

TEST(LoadGroovyConfig, AnUnknownCaptureModeFallsBackToSafeDefaults) {
  const TemporaryDirectory directory("config-capture-mode");
  const auto path = directory.write(
      "config.json",
      R"({"version":1,"target":"must-not-load","captureMode":"portal"})");
  std::string warning;
  EXPECT_THAT(loadGroovyConfig(path, &warning).target, IsEmpty());
  EXPECT_THAT(warning, HasSubstr("capture mode"));
}

TEST(LoadGroovyConfig, AnUnknownSamplingModeFallsBackToSafeDefaults) {
  const TemporaryDirectory directory("config-sampling");
  const auto path = directory.write(
      "config.json",
      R"({"version":1,"target":"must-not-load","sampling":"area"})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(loaded.target, IsEmpty());
  EXPECT_EQ(loaded.source.sampling, SamplingMode::Point);
  EXPECT_THAT(warning, HasSubstr("sampling mode"));
}

TEST(LoadGroovyConfig, TimingsTheProtocolCannotStreamFallBackToSafeDefaults) {
  const TemporaryDirectory directory("config-oversize");
  const auto path = directory.write("config.json", R"({
  "version": 1,
  "target": "must-not-load.local",
  "pixelClockMHz": 65.0,
  "hActive": 1024, "hBegin": 1048, "hEnd": 1184, "hTotal": 1344,
  "vActive": 768, "vBegin": 771, "vEnd": 777, "vTotal": 806,
  "interlaced": false
})");
  std::string warning;
  EXPECT_THAT(loadGroovyConfig(path, &warning).target, IsEmpty());
  EXPECT_THAT(warning, HasSubstr("exceeds Groovy_MiSTer frame buffer"));
}

TEST(LoadGroovyConfig, IgnoresUnknownAlignmentCropAndRotationNames) {
  const TemporaryDirectory directory("config-unknown-enums");
  const auto path = directory.write("config.json", R"({
  "version": 1, "target": "keep.local",
  "alignment": "diagonal", "crop": "16:9", "rotation": "270"
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  // Unlike sampling, these three degrade to the default rather than rejecting
  // the whole file.
  EXPECT_EQ(loaded.target, "keep.local");
  EXPECT_THAT(warning, IsEmpty());
  EXPECT_EQ(loaded.source.alignment, Alignment::Center);
  EXPECT_EQ(loaded.source.crop, CropMode::Full43);
  EXPECT_EQ(loaded.source.rotation, Rotation::None);
}

TEST(LoadGroovyConfig, TopLevelKeysAreNotShadowedByCustomModelineKeys) {
  const TemporaryDirectory directory("config-shadowing");
  // The decoy array comes first, so a naive positional search would read its
  // timings as the active modeline.
  const auto path = directory.write("config.json", R"({
  "version": 1,
  "customModelines": [
    {"name":"decoy","pixelClockMHz":9.9,"hActive":111,"hBegin":112,
     "hEnd":113,"hTotal":140,"vActive":222,"vBegin":223,"vEnd":224,
     "vTotal":240,"interlaced":true}
  ],
  "target": "real.local",
  "pixelClockMHz": 25.175,
  "hActive": 640, "hBegin": 656, "hEnd": 752, "hTotal": 800,
  "vActive": 480, "vBegin": 490, "vEnd": 492, "vTotal": 525,
  "interlaced": false
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(warning, IsEmpty());
  EXPECT_EQ(loaded.target, "real.local");
  EXPECT_EQ(loaded.modeline.hActive, 640);
  EXPECT_EQ(loaded.modeline.vTotal, 525);
  EXPECT_FALSE(loaded.modeline.interlaced);
  ASSERT_EQ(loaded.customModelines.size(), 1u);
  EXPECT_EQ(loaded.customModelines[0].hActive, 111);
  EXPECT_TRUE(loaded.customModelines[0].interlaced);
}

TEST(LoadGroovyConfig, BracesAndQuotesInsideStringsDoNotConfuseScoping) {
  const TemporaryDirectory directory("config-string-braces");
  const auto path = directory.write("config.json", R"({
  "version": 1,
  "monitor": "brace { inside \" and \\ too",
  "target": "real.local"
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(warning, IsEmpty());
  EXPECT_EQ(loaded.target, "real.local");
}

TEST(LoadGroovyConfig, SkipsCustomModelinesThatAreIncompleteOrInvalid) {
  const TemporaryDirectory directory("config-bad-customs");
  const auto path = directory.write("config.json", R"({
  "version": 1, "target": "real.local",
  "customModelines": [
    {"name":"missing-clock","hActive":320,"hBegin":336,"hEnd":367,
     "hTotal":426,"vActive":240,"vBegin":244,"vEnd":247,"vTotal":262},
    {"name":"missing-vtotal","pixelClockMHz":6.7,"hActive":320,"hBegin":336,
     "hEnd":367,"hTotal":426,"vActive":240,"vBegin":244,"vEnd":247},
    {"name":"unordered","pixelClockMHz":6.7,"hActive":320,"hBegin":336,
     "hEnd":367,"hTotal":300,"vActive":240,"vBegin":244,"vEnd":247,
     "vTotal":262},
    {"name":"good","pixelClockMHz":6.7,"hActive":320,"hBegin":336,
     "hEnd":367,"hTotal":426,"vActive":240,"vBegin":244,"vEnd":247,
     "vTotal":262,"interlaced":false}
  ]
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(warning, IsEmpty());
  ASSERT_EQ(loaded.customModelines.size(), 1u);
  EXPECT_EQ(loaded.customModelines[0].name, "good");
}

TEST(LoadGroovyConfig, AnEmptyOrMalformedCustomModelineArrayIsHarmless) {
  const TemporaryDirectory directory("config-empty-customs");
  std::string warning;
  auto loaded = loadGroovyConfig(
      directory.write("empty.json",
                      R"({"version":1,"target":"a","customModelines":[]})"),
      &warning);
  EXPECT_EQ(loaded.target, "a");
  EXPECT_THAT(loaded.customModelines, IsEmpty());

  // The key is present but the array is never closed.
  loaded = loadGroovyConfig(
      directory.write("unclosed.json",
                      R"({"version":1,"target":"b","customModelines":[)"),
      &warning);
  EXPECT_EQ(loaded.target, "b");
  EXPECT_THAT(loaded.customModelines, IsEmpty());
}

TEST(LoadGroovyConfig, NumbersTooLargeForADoubleAreIgnored) {
  const TemporaryDirectory directory("config-huge-number");
  const std::string enormous(400, '9');
  const auto path = directory.write(
      "config.json",
      R"({"version":1,"target":"real.local","frameDelay":)" + enormous + "}");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_EQ(loaded.target, "real.local");
  EXPECT_EQ(loaded.source.frameDelay, 0);
}

TEST(LoadGroovyConfig, ACustomModelineWithAnUnreadableNumberIsSkipped) {
  const TemporaryDirectory directory("config-huge-custom");
  const std::string enormous(400, '9');
  const auto path = directory.write("config.json", R"({
  "version": 1, "target": "real.local",
  "customModelines": [
    {"name":"huge","pixelClockMHz":)" + enormous +
                                                       R"(,"hActive":320}
  ]
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_EQ(loaded.target, "real.local");
  EXPECT_THAT(loaded.customModelines, IsEmpty());
}

TEST(LoadGroovyConfig, IgnoresArbitrarilyDeepNestingWhenReadingTopLevelKeys) {
  const TemporaryDirectory directory("config-deep");
  const auto path = directory.write("config.json", R"({
  "version": 1,
  "diagnostics": {"window": {"geometry": [10, [20, 30], {"target": "decoy"}]}},
  "history": [[1, 2], [3, {"target": "another-decoy"}]],
  "target": "real.local",
  "width": 512, "height": 384
})");
  std::string warning;
  const auto loaded = loadGroovyConfig(path, &warning);
  EXPECT_THAT(warning, IsEmpty());
  EXPECT_EQ(loaded.target, "real.local");
  EXPECT_EQ(loaded.source.width, 512);
  EXPECT_EQ(loaded.source.height, 384);
}

TEST(LoadGroovyConfig, ACustomModelineMissingAnySingleTimingIsSkipped) {
  const TemporaryDirectory directory("config-missing-fields");
  const char* fields[] = {"hActive", "hBegin", "hEnd",  "hTotal",
                          "vActive", "vBegin", "vEnd",  "vTotal"};
  for (const char* omitted : fields) {
    std::string object = R"({"name":"partial","pixelClockMHz":6.7)";
    const struct {
      const char* key;
      int value;
    } timings[] = {{"hActive", 320}, {"hBegin", 336}, {"hEnd", 367},
                   {"hTotal", 426},  {"vActive", 240}, {"vBegin", 244},
                   {"vEnd", 247},    {"vTotal", 262}};
    for (const auto& timing : timings) {
      if (std::string(timing.key) == omitted) continue;
      object += ",\"" + std::string(timing.key) + "\":" +
                std::to_string(timing.value);
    }
    object += "}";
    const auto path = directory.write(
        std::string("without-") + omitted + ".json",
        R"({"version":1,"target":"real.local","customModelines":[)" + object +
            "]}");
    std::string warning;
    const auto loaded = loadGroovyConfig(path, &warning);
    EXPECT_EQ(loaded.target, "real.local") << omitted;
    EXPECT_THAT(loaded.customModelines, IsEmpty())
        << "a modeline without " << omitted << " cannot be used";
  }
}

TEST(LoadGroovyConfig, AcceptsTheSilentAudioSinkSentinel) {
  const TemporaryDirectory directory("config-silent-sink");
  const auto path = directory.file("config.json");
  AppConfig config;
  config.source.audioSink = SilentAudioSink;
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(config, path, error)) << error;
  EXPECT_EQ(loadGroovyConfig(path).source.audioSink, SilentAudioSink);
}

TEST(LoadGroovyConfig, LeavesAGoodFileInPlaceAfterARejectedSave) {
  const TemporaryDirectory directory("config-save-reject");
  const auto path = directory.file("config.json");
  const auto good = populatedConfig();
  std::string error;
  ASSERT_TRUE(saveGroovyConfig(good, path, error)) << error;

  auto rejected = good;
  rejected.version = 9;
  EXPECT_FALSE(saveGroovyConfig(rejected, path, error));
  EXPECT_EQ(loadGroovyConfig(path).target, good.target);
}

}  // namespace
