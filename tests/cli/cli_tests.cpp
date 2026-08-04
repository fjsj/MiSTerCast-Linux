#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "support/fake_mister.hpp"
#include "support/nested_xvfb.hpp"
#include "support/subprocess.hpp"
#include "support/temp_directory.hpp"
#include "support/wait_for.hpp"

using namespace mistercast;
using namespace mistercast::test;
using testing::HasSubstr;

namespace {

constexpr uint8_t kHealthy = 0x84;

// Every case drives the installed entry point rather than a linked-in copy of
// the parser, so exit codes and diagnostics are checked exactly as a user or a
// packaging script would see them.
ProcessResult run(std::vector<std::string> arguments,
                  ProcessOptions options = {}) {
  arguments.insert(arguments.begin(), MISTERCAST_EXECUTABLE);
  return runProcess(arguments, std::move(options));
}

// Aggregate-initialising ProcessOptions in place would leave interruptAfter
// unmentioned, so build it here instead.
ProcessOptions withEnvironment(
    std::vector<std::pair<std::string, std::string>> variables,
    std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
  ProcessOptions options;
  options.environment = std::move(variables);
  options.timeout = timeout;
  return options;
}

bool displayAvailable() {
  const char* display = ::getenv("DISPLAY");
  return display && *display;
}

// ------------------------------------------------------------------- help

TEST(Cli, HelpIsAvailableUnderBothSpellings) {
  for (const char* flag : {"--help", "-h"}) {
    const auto result = run({flag});
    EXPECT_TRUE(result.exitedWith(0)) << flag << " exit " << result.exitCode;
    EXPECT_THAT(result.out, HasSubstr("MiSTerCast for Linux"));
    EXPECT_THAT(result.out, HasSubstr("mistercast stream"));
    EXPECT_THAT(result.out, HasSubstr("mistercast pattern"));
    EXPECT_THAT(result.out, HasSubstr("list-monitors"));
  }
}

TEST(Cli, AnUnknownCommandPrintsUsageAndFails) {
  const auto result = run({"broadcast"});
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.out, HasSubstr("Usage:"));
}

TEST(Cli, StreamAndPatternBothHaveTheirOwnHelp) {
  for (const auto& arguments : std::vector<std::vector<std::string>>{
           {"stream", "--help"}, {"stream", "-h"}, {"pattern", "--help"},
           {"pattern", "-h"}}) {
    const auto result = run(arguments);
    EXPECT_TRUE(result.exitedWith(0)) << arguments[0] << " " << arguments[1];
    EXPECT_THAT(result.out, HasSubstr("Stream options:"));
  }
}

// ------------------------------------------------------------- enumeration

TEST(Cli, ListsTheBundledModelinesWithoutNeedingADisplay) {
  const auto result = run({"list-modelines"}, withEnvironment({{"DISPLAY", ""}}));
  ASSERT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.out, HasSubstr("320x240 NTSC (60Hz)"));
  EXPECT_THAT(result.out, HasSubstr("320x240p"));
  EXPECT_THAT(result.out, HasSubstr("480i"));
  EXPECT_THAT(result.out, HasSubstr("Hz"));
  // One line per bundled preset.
  EXPECT_EQ(std::count(result.out.begin(), result.out.end(), '\n'),
            long(bundledModelines().size()));
}

TEST(Cli, ListMonitorsAndCheckExplainAMissingDisplay) {
  for (const char* command : {"list-monitors", "check"}) {
    const auto result = run({command}, withEnvironment({{"DISPLAY", ""}}));
    EXPECT_TRUE(result.exitedWith(1)) << command;
    EXPECT_THAT(result.err, HasSubstr("DISPLAY"));
  }
}

TEST(Cli, ListsMonitorsOnTheCurrentDisplay) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  const auto result = run({"list-monitors"});
  ASSERT_TRUE(result.exitedWith(0)) << result.err;
  // name<TAB>WIDTHxHEIGHT+X+Y, one line per monitor. Whether any is flagged
  // primary depends on the server, so only the shape is asserted.
  std::string error;
  const auto monitors = x11Monitors(error);
  ASSERT_FALSE(monitors.empty()) << error;
  EXPECT_EQ(std::count(result.out.begin(), result.out.end(), '\n'),
            long(monitors.size()));
  for (const auto& monitor : monitors) {
    EXPECT_THAT(result.out, HasSubstr(monitor.name + "\t"));
    EXPECT_THAT(result.out, HasSubstr(std::to_string(monitor.width) + "x" +
                                      std::to_string(monitor.height) + "+"));
  }
}

TEST(Cli, CheckReportsCaptureHealthAndTheConfigurationPath) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  const TemporaryDirectory directory("cli-check");
  const auto result =
      run({"check"}, withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}));
  ASSERT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.out, HasSubstr("X11 capture: OK"));
  EXPECT_THAT(result.out,
              HasSubstr((directory.path() / "mistercast/config.json").string()));
}

// ------------------------------------------------------- stream argument errors

TEST(Cli, MarksThePrimaryMonitorWhenTheServerReportsOne) {
  // A server without RandR offers its whole screen as one primary monitor, which
  // is the only way to see the "(primary)" marker on a headless runner.
  NestedXvfb nested({"-screen", "0", "112x84x24", "-extension", "RANDR"});
  if (!nested.ready()) GTEST_SKIP() << "a nested Xvfb could not be started";

  const auto result =
      run({"list-monitors"}, withEnvironment({{"DISPLAY", nested.display()}}));
  ASSERT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.out, HasSubstr("X11-screen-0\t112x84+0+0 (primary)"));

  const TemporaryDirectory directory("cli-primary-check");
  const auto checked =
      run({"check"}, withEnvironment({{"DISPLAY", nested.display()},
                                      {"XDG_CONFIG_HOME",
                                       directory.path().string()}}));
  EXPECT_TRUE(checked.exitedWith(0)) << checked.err;
  EXPECT_THAT(checked.out, HasSubstr("X11 capture: OK"));
}

TEST(Cli, StreamRequiresATarget) {
  const TemporaryDirectory directory("cli-no-target");
  const auto result =
      run({"stream"}, withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}));
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("A target is required"));
}

TEST(Cli, StreamRejectsAnUnknownOption) {
  const auto result = run({"stream", "--target", "127.0.0.1", "--bitrate"});
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("Unknown option: --bitrate"));
}

TEST(Cli, StreamReportsAMissingValueForEveryOptionThatTakesOne) {
  for (const char* option : {"--target", "--monitor", "--modeline", "--crop",
                             "--alignment", "--rotation", "--sampling",
                             "--size", "--offset", "--frame-delay"}) {
    const auto result = run({"stream", option});
    EXPECT_TRUE(result.exitedWith(2)) << option;
    EXPECT_TRUE(result.mentions(option)) << option << ": " << result.err;
  }
}

TEST(Cli, StreamValidatesEveryEnumeratedOption) {
  const struct {
    const char* option;
    const char* value;
    const char* diagnostic;
  } cases[] = {{"--crop", "16:9", "Invalid crop mode"},
               {"--alignment", "diagonal", "Invalid alignment"},
               {"--rotation", "270", "Invalid rotation"},
               {"--sampling", "area", "Sampling must be point"}};
  for (const auto& test : cases) {
    const auto result =
        run({"stream", "--target", "127.0.0.1", test.option, test.value});
    EXPECT_TRUE(result.exitedWith(2)) << test.option;
    EXPECT_THAT(result.err, HasSubstr(test.diagnostic));
  }
}

TEST(Cli, StreamValidatesSizeAndOffsetSyntax) {
  for (const char* size : {"640", "wide", "", "x480"}) {
    const auto result = run({"stream", "--target", "127.0.0.1", "--size", size});
    EXPECT_TRUE(result.exitedWith(2)) << "size " << size;
    EXPECT_THAT(result.err, HasSubstr("Size must be WxH"));
  }
  for (const char* offset : {"10", "left,top", "", ",4"}) {
    const auto result =
        run({"stream", "--target", "127.0.0.1", "--offset", offset});
    EXPECT_TRUE(result.exitedWith(2)) << "offset " << offset;
    EXPECT_THAT(result.err, HasSubstr("Offset must be X,Y"));
  }
}

TEST(Cli, StreamValidatesTheFrameDelayRange) {
  for (const char* delay : {"11", "-1", "auto", "99999999999999999999"}) {
    const auto result =
        run({"stream", "--target", "127.0.0.1", "--frame-delay", delay});
    EXPECT_TRUE(result.exitedWith(2)) << delay;
    EXPECT_THAT(result.err, HasSubstr("Frame delay must be 0..10"));
  }
}

TEST(Cli, StreamValidatesTheModeline) {
  auto result = run({"stream", "--target", "127.0.0.1", "--modeline", "bad"});
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("modeline needs"));

  // Orderable timings the core has no framebuffer for parse successfully; the
  // protocol limit is reported when the session refuses to start.
  result = run({"stream", "--target", "127.0.0.1", "--no-audio", "--modeline",
                "65 1024 1048 1184 1344 768 771 777 806 0"});
  EXPECT_TRUE(result.exitedWith(1));
  EXPECT_THAT(result.err, HasSubstr("exceeds Groovy_MiSTer frame buffer"));
}

// ------------------------------------------------------------------ persistence

TEST(Cli, SaveWritesTheOverridesAndKeepsACustomModelineAsAPreset) {
  const TemporaryDirectory directory("cli-save");
  const auto configFile = directory.path() / "mistercast/config.json";
  // The stream itself cannot connect, but --save is applied before dialling.
  const auto result = run({"stream", "--target", "mister.example", "--no-audio",
                           "--crop", "2x", "--rotation", "cw90", "--sampling",
                           "line-blend", "--alignment", "top-left", "--size",
                           "640x480", "--offset", "-5,7", "--frame-delay", "3",
                           "--progressive-interlace-buffer", "--modeline",
                           "6.7 320 336 367 426 240 244 247 262 0", "--save"},
                          withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}, std::chrono::seconds(20)));
  EXPECT_TRUE(result.exitedWith(1)) << "the unreachable target ends the run";
  ASSERT_TRUE(std::filesystem::exists(configFile)) << result.err;

  const auto saved = loadGroovyConfig(configFile);
  EXPECT_EQ(saved.target, "mister.example");
  EXPECT_FALSE(saved.source.audio);
  EXPECT_EQ(saved.source.crop, CropMode::X2);
  EXPECT_EQ(saved.source.rotation, Rotation::CW90);
  EXPECT_EQ(saved.source.sampling, SamplingMode::LineBlend);
  EXPECT_EQ(saved.source.alignment, Alignment::TopLeft);
  EXPECT_EQ(saved.source.width, 640);
  EXPECT_EQ(saved.source.height, 480);
  EXPECT_EQ(saved.source.xOffset, -5);
  EXPECT_EQ(saved.source.yOffset, 7);
  EXPECT_EQ(saved.source.frameDelay, 3);
  EXPECT_TRUE(saved.source.progressiveInterlaceBuffer);
  ASSERT_EQ(saved.customModelines.size(), 1u)
      << "a modeline given on the command line is kept as a custom preset";
  EXPECT_EQ(saved.customModelines[0].name, "Custom");
}

TEST(Cli, OverridesDoNotPersistWithoutSave) {
  const TemporaryDirectory directory("cli-no-save");
  const auto configFile = directory.path() / "mistercast/config.json";
  const auto result =
      run({"stream", "--target", "mister.example", "--no-audio", "--crop", "3x"},
          withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}, std::chrono::seconds(20)));
  EXPECT_TRUE(result.exitedWith(1));
  EXPECT_FALSE(std::filesystem::exists(configFile));
}

TEST(Cli, ReportsAConfigurationThatCannotBeSaved) {
  const TemporaryDirectory directory("cli-save-fails");
  // A regular file where the configuration directory has to go.
  directory.write("mistercast", "not a directory");
  const auto result =
      run({"stream", "--target", "mister.example", "--no-audio", "--save"},
          withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}));
  EXPECT_TRUE(result.exitedWith(1));
  EXPECT_THAT(result.err, HasSubstr("Cannot save settings"));
}

TEST(Cli, StreamStartsFromASavedConfiguration) {
  const TemporaryDirectory directory("cli-load");
  AppConfig config;
  config.target = "mister.example";
  config.source.audio = false;
  config.source.crop = CropMode::X1;
  std::string error;
  const auto configFile = directory.path() / "mistercast/config.json";
  std::filesystem::create_directories(configFile.parent_path());
  ASSERT_TRUE(saveGroovyConfig(config, configFile, error)) << error;

  // No --target on the command line: the saved one has to be used, and the run
  // therefore fails at the network rather than at argument validation.
  const auto result =
      run({"stream"}, withEnvironment({{"XDG_CONFIG_HOME", directory.path().string()}}, std::chrono::seconds(20)));
  EXPECT_TRUE(result.exitedWith(1));
  EXPECT_THAT(result.err, HasSubstr("Start failed"));
}

// --------------------------------------------------------------- live streaming

TEST(Cli, StreamsToAListeningTargetAndShutsDownCleanlyOnInterrupt) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  FakeMister mister(kHealthy);
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  const TemporaryDirectory directory("cli-stream");
  ProcessOptions options;
  options.environment = {{"XDG_CONFIG_HOME", directory.path().string()}};
  options.interruptAfter = std::chrono::seconds(2);
  options.timeout = std::chrono::seconds(30);
  const auto result = run({"stream", "--target", "127.0.0.1", "--no-audio",
                           "--crop", "1x", "--frame-delay", "0"},
                          options);
  EXPECT_TRUE(result.interruptDelivered) << "the stream ended before SIGINT";
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.err, HasSubstr("Streaming; press Ctrl-C to stop."));
  EXPECT_GT(mister.blits(), 0u);
  EXPECT_EQ(mister.audioPackets(), 0u);
  EXPECT_TRUE(waitFor([&] { return mister.closes() >= 1; }));
}

TEST(Cli, StreamPrintsPeriodicCounters) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  FakeMister mister(kHealthy);
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  const TemporaryDirectory directory("cli-stats");
  ProcessOptions options;
  options.environment = {{"XDG_CONFIG_HOME", directory.path().string()}};
  // Counters are emitted every five seconds.
  options.interruptAfter = std::chrono::milliseconds(6500);
  options.timeout = std::chrono::seconds(40);
  const auto result =
      run({"stream", "--target", "127.0.0.1", "--no-audio", "--crop", "1x",
           "--modeline", "12.336 640 662 720 784 480 488 494 525 1"},
          options);
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.err, HasSubstr("fps"));
  EXPECT_THAT(result.err, HasSubstr("dropped"));
  EXPECT_THAT(result.err, HasSubstr("sync line"));
  EXPECT_THAT(result.err, HasSubstr("audio buffered"));
  // An interlaced field buffer adds the field-phase and adaptive counters.
  EXPECT_THAT(result.err, HasSubstr("field"));
  EXPECT_THAT(result.err, HasSubstr("reserve/latest"));
}

TEST(Cli, StreamReportsAnUnhealthyReceiverInItsCounters) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  // VRAM synced, VGA frameskip fallback, core audio on, queue empty.
  FakeMister mister(0x4c);
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  const TemporaryDirectory directory("cli-unhealthy");
  ProcessOptions options;
  options.environment = {{"XDG_CONFIG_HOME", directory.path().string()}};
  options.interruptAfter = std::chrono::milliseconds(6500);
  options.timeout = std::chrono::seconds(40);
  const auto result = run({"stream", "--target", "127.0.0.1", "--no-audio",
                           "--crop", "1x", "--progressive-interlace-buffer"},
                          options);
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.err, HasSubstr("/fallback"));
  EXPECT_THAT(result.err, HasSubstr("queue empty"));
  EXPECT_THAT(result.err, HasSubstr("MiSTer audio on"));
  EXPECT_THAT(result.err, testing::Not(HasSubstr("reserve/latest")))
      << "a progressive framebuffer has no field phase to report";
}

TEST(Cli, StreamExitsNonZeroWhenTheStreamFailsMidRun) {
  if (!displayAvailable()) GTEST_SKIP() << "no X11 display available";
  auto mister = std::make_unique<FakeMister>(kHealthy);
  if (!mister->bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  const TemporaryDirectory directory("cli-mid-failure");
  ProcessOptions options;
  options.environment = {{"XDG_CONFIG_HOME", directory.path().string()}};
  options.timeout = std::chrono::seconds(40);
  // No interrupt: the run has to end on its own once the target disappears.
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    mister.reset();
  });
  const auto result = run({"stream", "--target", "127.0.0.1", "--no-audio",
                           "--monitor", "", "--audio", "--no-audio",
                           "--interlaced-field-buffer", "--crop", "1x"},
                          options);
  stopper.join();
  EXPECT_TRUE(result.exitedWith(1))
      << "a stream that dies must not report success: " << result.err;
}

// ----------------------------------------------------------------- pattern mode

TEST(Cli, PatternHelpIsOnlyRecognisedOnItsOwn) {
  // `pattern --help` prints usage, but `--help` alongside other options is an
  // ordinary unknown option rather than a request for help.
  const auto result =
      run({"pattern", "--tone", "--help"}, withEnvironment({{"DISPLAY", ""}}));
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("unknown pattern option: --help"));
}

TEST(Cli, ASinglePatternOptionThatIsNotHelpIsParsedNormally) {
  // Exactly one argument after `pattern`, so the help shortcut is considered and
  // declined; the option itself is then parsed and found to be incomplete.
  const auto result =
      run({"pattern", "--tone"}, withEnvironment({{"DISPLAY", ""}}));
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("a target is required"));
  EXPECT_THAT(result.out, testing::Not(HasSubstr("Usage:")));
}

TEST(Cli, PatternRejectsBadOptionsWithoutTouchingTheDisplay) {
  const struct {
    std::vector<std::string> arguments;
    const char* diagnostic;
  } cases[] = {
      {{"pattern"}, "a target is required"},
      {{"pattern", "--target", "h", "--content", "solid"},
       "content must be bars or noise"},
      {{"pattern", "--target", "h", "--frame-delay", "11"},
       "frame delay must be 0..10"},
      {{"pattern", "--target", "h", "--modeline", "bad"}, "modeline needs"},
      {{"pattern", "--target", "h", "--rotation", "none"},
       "unknown pattern option: --rotation"}};
  for (const auto& test : cases) {
    const auto result = run(test.arguments, withEnvironment({{"DISPLAY", ""}}));
    EXPECT_TRUE(result.exitedWith(2)) << test.diagnostic;
    EXPECT_THAT(result.err, HasSubstr(test.diagnostic));
  }
}

TEST(Cli, PatternReportsATargetThatCannotBeResolved) {
  const auto result = run({"pattern", "--target", "mistercast.invalid."},
                          withEnvironment({{"DISPLAY", ""}}, std::chrono::seconds(20)));
  EXPECT_TRUE(result.exitedWith(1));
  EXPECT_THAT(result.err, HasSubstr("Pattern failed"));
}

TEST(Cli, PatternStreamsWithoutADisplayAndStopsOnInterrupt) {
  FakeMister mister(0xc4);  // healthy, core audio on
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  ProcessOptions options;
  // Pattern mode is capture-independent: it must work with no X11 at all.
  options.environment = {{"DISPLAY", ""}};
  options.interruptAfter = std::chrono::seconds(2);
  options.timeout = std::chrono::seconds(30);
  const auto result =
      run({"pattern", "--target", "127.0.0.1", "--content", "noise", "--tone",
           "--modeline", "1 16 18 20 24 8 9 10 12 1", "--frame-delay", "0"},
          options);
  EXPECT_TRUE(result.interruptDelivered) << "the pattern ended before SIGINT";
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_THAT(result.err, HasSubstr("Pattern streaming"));
  EXPECT_GT(mister.blits(), 0u);
  EXPECT_GT(mister.audioPackets(), 0u);
  EXPECT_EQ(mister.initRateCode(), 3);
  EXPECT_TRUE(waitFor([&] { return mister.closes() >= 1; }));
}

TEST(Cli, PatternNeverReadsOrWritesSavedSettings) {
  FakeMister mister(kHealthy);
  if (!mister.bound())
    GTEST_SKIP() << "UDP port 32100 is already in use on this machine";

  const TemporaryDirectory directory("cli-pattern-config");
  ProcessOptions options;
  options.environment = {{"DISPLAY", ""},
                         {"XDG_CONFIG_HOME", directory.path().string()}};
  options.interruptAfter = std::chrono::seconds(1);
  options.timeout = std::chrono::seconds(20);
  const auto result = run({"pattern", "--target", "127.0.0.1"}, options);
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
  EXPECT_FALSE(
      std::filesystem::exists(directory.path() / "mistercast/config.json"))
      << "pattern mode is session-only and must not persist anything";
}

// --------------------------------------------------------------- no-argument run

TEST(Cli, RunningWithNoArgumentsAddressesTheGui) {
  ProcessOptions options;
  options.environment = {{"QT_QPA_PLATFORM", "offscreen"}, {"DISPLAY", ""}};
  options.timeout = std::chrono::seconds(20);
#ifdef MISTERCAST_HAVE_QT
  // With Qt present the window opens and only an interrupt ends it.
  options.interruptAfter = std::chrono::seconds(1);
  const auto result = run({}, options);
  EXPECT_TRUE(result.interruptDelivered)
      << "the GUI must still have been running when SIGINT was sent";
  EXPECT_TRUE(result.exitedWith(0)) << result.err;
#else
  const auto result = run({}, options);
  EXPECT_TRUE(result.exitedWith(2));
  EXPECT_THAT(result.err, HasSubstr("no Qt 6 GUI"));
#endif
}

}  // namespace
