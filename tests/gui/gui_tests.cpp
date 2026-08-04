#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <xcb/xcb.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gui/main_window.hpp"
#include "mistercast/config.hpp"
#include "support/fake_mister.hpp"
#include "support/pulse_server.hpp"
#include "support/temp_directory.hpp"

using namespace mistercast;
using namespace mistercast::test;
using mistercast::gui::MainWindow;
using testing::HasSubstr;

namespace {

constexpr uint8_t kHealthy = 0x84;

// Runs the Qt event loop until the condition holds or the deadline passes. Every
// widget mutation still happens on this, the Qt thread.
bool pumpUntil(const std::function<bool()>& ready,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(15000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) return true;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents();
  }
  return ready();
}

void pumpFor(std::chrono::milliseconds duration) {
  pumpUntil([] { return false; }, duration);
}

// Real X11 windows for the chooser to find. Xvfb has no window manager and no
// other clients, so a test that needs a capturable window has to create one.
class ForeignWindows {
 public:
  explicit ForeignWindows(const std::vector<std::string>& titles) {
    connection_ = xcb_connect(nullptr, nullptr);
    if (!connection_ || xcb_connection_has_error(connection_)) return;
    auto* screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
    if (!screen) return;
    for (const auto& title : titles) {
      const auto window = xcb_generate_id(connection_);
      const uint32_t background[] = {screen->black_pixel};
      xcb_create_window(connection_, screen->root_depth, window, screen->root, 0,
                        0, 64, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual, XCB_CW_BACK_PIXEL, background);
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window,
                          XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, title.size(),
                          title.data());
      xcb_map_window(connection_, window);
      windows_.push_back(window);
    }
    free(xcb_get_input_focus_reply(
        connection_, xcb_get_input_focus(connection_), nullptr));
    ready_ = true;
  }

  ~ForeignWindows() {
    if (!connection_) return;
    for (const auto window : windows_) xcb_destroy_window(connection_, window);
    xcb_disconnect(connection_);
  }

  ForeignWindows(const ForeignWindows&) = delete;
  ForeignWindows& operator=(const ForeignWindows&) = delete;

  bool ready() const noexcept { return ready_; }
  uint32_t first() const noexcept { return windows_.empty() ? 0 : windows_.front(); }

 private:
  xcb_connection_t* connection_{};
  std::vector<xcb_window_t> windows_;
  bool ready_{false};
};

// Each test gets its own configuration directory, so the window never reads or
// writes the developer's real ~/.config/mistercast.
class Gui : public testing::Test {
 protected:
  void SetUp() override {
    directory = std::make_unique<TemporaryDirectory>("gui");
    configHome = std::make_unique<ScopedEnvironment>(
        "XDG_CONFIG_HOME", directory->path().string().c_str());
  }

  void TearDown() override {
    window.reset();
    receiver.reset();
    configHome.reset();
    directory.reset();
  }

  // Writes a configuration the window will load on construction.
  void writeConfig(const AppConfig& config) {
    const auto path = directory->path() / "mistercast/config.json";
    std::filesystem::create_directories(path.parent_path());
    std::string error;
    ASSERT_TRUE(saveGroovyConfig(config, path, error)) << error;
  }

  AppConfig streamableConfig() const {
    AppConfig config;
    config.target = "127.0.0.1";
    config.source.audio = false;
    config.source.preview = false;
    config.source.crop = CropMode::X1;
    config.modeline = Modeline::safeDefault();
    return config;
  }

  void build() { window = std::make_unique<MainWindow>(); }

  void bindReceiver(uint8_t statusBits = kHealthy) {
    receiver = std::make_unique<FakeMister>(statusBits);
    if (!receiver->bound())
      GTEST_SKIP() << "UDP port 32100 is already in use on this machine";
  }

  template <class Widget>
  Widget* find(const char* name) const {
    auto* widget = window->findChild<Widget*>(QString::fromLatin1(name));
    EXPECT_NE(widget, nullptr) << "no widget named " << name;
    return widget;
  }

  QString log() const {
    return find<QPlainTextEdit>("log")->toPlainText();
  }

  std::unique_ptr<TemporaryDirectory> directory;
  std::unique_ptr<ScopedEnvironment> configHome;
  std::unique_ptr<FakeMister> receiver;
  std::unique_ptr<MainWindow> window;
};

// ------------------------------------------------------------------ construction

TEST_F(Gui, OpensIdleWithEveryControlPresent) {
  build();
  EXPECT_EQ(find<QLabel>("status")->text(), "Idle");
  EXPECT_EQ(find<QPushButton>("streamButton")->text(), "Start Stream");
  EXPECT_THAT(log().toStdString(), HasSubstr("MiSTerCast ready."));
  EXPECT_GT(find<QComboBox>("preset")->count(),
            int(bundledModelines().size()) - 1);
  EXPECT_EQ(find<QComboBox>("captureMode")->count(), 2);
  EXPECT_EQ(find<QComboBox>("crop")->count(), 8);
  EXPECT_EQ(find<QComboBox>("alignment")->count(), 9);
  EXPECT_EQ(find<QComboBox>("rotation")->count(), 4);
  EXPECT_EQ(find<QComboBox>("sampling")->count(), 3);
  // Default output plus the silent sink are always offered.
  EXPECT_GE(find<QComboBox>("audioSink")->count(), 2);
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected");
  EXPECT_EQ(find<QLabel>("previewImage")->text(), "Preview Disabled");
}

TEST_F(Gui, LoadsEverySavedSettingIntoTheControls) {
  auto config = streamableConfig();
  config.target = "mister.example";
  config.source.crop = CropMode::X3;
  config.source.alignment = Alignment::BottomLeft;
  config.source.rotation = Rotation::Flip180;
  config.source.sampling = SamplingMode::LineBlend;
  config.source.width = 512;
  config.source.height = 384;
  config.source.xOffset = -20;
  config.source.yOffset = 30;
  config.source.frameDelay = 5;
  config.source.audio = true;
  config.source.preview = true;
  config.modeline = bundledModelines().at(3);  // 640x480i
  writeConfig(config);
  build();

  EXPECT_EQ(find<QLineEdit>("target")->text(), "mister.example");
  EXPECT_EQ(find<QComboBox>("crop")->currentIndex(), int(CropMode::X3));
  EXPECT_EQ(find<QComboBox>("alignment")->currentIndex(),
            int(Alignment::BottomLeft));
  EXPECT_EQ(find<QComboBox>("rotation")->currentIndex(),
            int(Rotation::Flip180));
  EXPECT_EQ(find<QComboBox>("sampling")->currentIndex(),
            int(SamplingMode::LineBlend));
  EXPECT_EQ(find<QSpinBox>("width")->value(), 512);
  EXPECT_EQ(find<QSpinBox>("height")->value(), 384);
  EXPECT_EQ(find<QSpinBox>("xOffset")->value(), -20);
  EXPECT_EQ(find<QSpinBox>("yOffset")->value(), 30);
  EXPECT_EQ(find<QSpinBox>("frameDelay")->value(), 5);
  EXPECT_TRUE(find<QCheckBox>("audio")->isChecked());
  EXPECT_TRUE(find<QCheckBox>("preview")->isChecked());
  EXPECT_EQ(find<QSpinBox>("hActive")->value(), 640);
  EXPECT_EQ(find<QSpinBox>("vTotal")->value(), 525);
  EXPECT_TRUE(find<QCheckBox>("interlaced")->isChecked());
  EXPECT_EQ(find<QComboBox>("preset")->currentText(),
            QString::fromStdString(config.modeline.name));
}

TEST_F(Gui, OffersASavedAudioSinkThatIsNoLongerPresent) {
  auto config = streamableConfig();
  config.source.audioSink = "alsa_output.no-longer-here";
  writeConfig(config);
  build();
  auto* sink = find<QComboBox>("audioSink");
  EXPECT_EQ(sink->currentData().toString(), "alsa_output.no-longer-here")
      << "an unknown saved sink is kept selectable rather than silently reset";
}

TEST_F(Gui, RestoresWindowCaptureModeWithoutRestoringTheWindow) {
  auto config = streamableConfig();
  config.source.capturePreference = CapturePreference::Window;
  writeConfig(config);
  build();
  EXPECT_EQ(find<QComboBox>("captureMode")->currentIndex(), 1);
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected")
      << "X11 window IDs are transient and must never be restored from disk";
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled())
      << "window mode without a selection cannot start";
}

TEST_F(Gui, ReportsACorruptConfigurationInTheLog) {
  const auto path = directory->path() / "mistercast/config.json";
  std::filesystem::create_directories(path.parent_path());
  TemporaryDirectory::read(directory->write("mistercast/config.json", "broken"));
  build();
  find<QPushButton>("loadButton")->click();
  EXPECT_THAT(log().toStdString(), HasSubstr("safe defaults"));
}

// -------------------------------------------------------- start button gating

TEST_F(Gui, TheStartButtonNeedsATarget) {
  build();
  auto* start = find<QPushButton>("streamButton");
  auto* target = find<QLineEdit>("target");
  target->setText("");
  EXPECT_FALSE(start->isEnabled());
  target->setText("   ");
  EXPECT_FALSE(start->isEnabled()) << "whitespace is not an address";
  target->setText("127.0.0.1");
  EXPECT_TRUE(start->isEnabled());
}

TEST_F(Gui, TheStartButtonNeedsTimingsTheProtocolAccepts) {
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  auto* start = find<QPushButton>("streamButton");
  ASSERT_TRUE(start->isEnabled());

  // hEnd beyond hTotal is not an orderable modeline.
  find<QSpinBox>("hTotal")->setValue(10);
  EXPECT_FALSE(start->isEnabled());
  find<QSpinBox>("hTotal")->setValue(426);
  EXPECT_TRUE(start->isEnabled());

  // An active area larger than the core's framebuffer is also refused.
  find<QSpinBox>("hActive")->setValue(1024);
  find<QSpinBox>("hBegin")->setValue(1048);
  find<QSpinBox>("hEnd")->setValue(1184);
  find<QSpinBox>("hTotal")->setValue(1344);
  find<QSpinBox>("vActive")->setValue(768);
  find<QSpinBox>("vBegin")->setValue(771);
  find<QSpinBox>("vEnd")->setValue(777);
  find<QSpinBox>("vTotal")->setValue(806);
  EXPECT_FALSE(start->isEnabled());
}

TEST_F(Gui, SwitchingToWindowModeSwapsWhichSourceControlIsUsable) {
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  auto* mode = find<QComboBox>("captureMode");
  auto* monitor = find<QComboBox>("monitor");
  auto* choose = find<QPushButton>("chooseWindowButton");

  EXPECT_TRUE(monitor->isEnabled());
  EXPECT_FALSE(choose->isEnabled());
  EXPECT_TRUE(find<QPushButton>("streamButton")->isEnabled());

  mode->setCurrentIndex(1);
  EXPECT_FALSE(monitor->isEnabled());
  EXPECT_TRUE(choose->isEnabled());
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled());

  mode->setCurrentIndex(0);
  EXPECT_TRUE(monitor->isEnabled());
  EXPECT_FALSE(choose->isEnabled());
  EXPECT_TRUE(find<QPushButton>("streamButton")->isEnabled());
}

// ----------------------------------------------------------------- plain controls

TEST_F(Gui, ApplyingAPresetLoadsItsTimings) {
  build();
  auto* preset = find<QComboBox>("preset");
  const auto presets = bundledModelines();
  const auto wanted = presets.at(3);  // 640x480i
  preset->setCurrentIndex(3);
  find<QPushButton>("applyModelineButton")->click();

  EXPECT_DOUBLE_EQ(find<QDoubleSpinBox>("pixelClock")->value(),
                   wanted.pixelClockMHz);
  EXPECT_EQ(find<QSpinBox>("hActive")->value(), wanted.hActive);
  EXPECT_EQ(find<QSpinBox>("hBegin")->value(), wanted.hBegin);
  EXPECT_EQ(find<QSpinBox>("hEnd")->value(), wanted.hEnd);
  EXPECT_EQ(find<QSpinBox>("hTotal")->value(), wanted.hTotal);
  EXPECT_EQ(find<QSpinBox>("vActive")->value(), wanted.vActive);
  EXPECT_EQ(find<QSpinBox>("vTotal")->value(), wanted.vTotal);
  EXPECT_EQ(find<QCheckBox>("interlaced")->isChecked(), wanted.interlaced);
  EXPECT_TRUE(find<QCheckBox>("progressiveInterlaceBuffer")->isEnabled())
      << "the interlace buffering choice only applies to interlaced modes";
}

TEST_F(Gui, InterlaceBufferingFollowsTheInterlacedFlag) {
  build();
  auto* interlaced = find<QCheckBox>("interlaced");
  auto* buffering = find<QCheckBox>("progressiveInterlaceBuffer");
  interlaced->setChecked(false);
  EXPECT_FALSE(buffering->isEnabled());
  interlaced->setChecked(true);
  EXPECT_TRUE(buffering->isEnabled());
}

TEST_F(Gui, TheAudioCheckboxGatesTheOutputChooser) {
  build();
  auto* audio = find<QCheckBox>("audio");
  auto* sink = find<QComboBox>("audioSink");
  audio->setChecked(false);
  EXPECT_FALSE(sink->isEnabled());
  audio->setChecked(true);
  EXPECT_TRUE(sink->isEnabled());
}

TEST_F(Gui, ThePreviewCheckboxUpdatesThePlaceholder) {
  build();
  auto* preview = find<QCheckBox>("preview");
  auto* image = find<QLabel>("previewImage");
  // Preview is on by default, so it has to be cleared before enabling it can
  // change anything.
  preview->setChecked(false);
  EXPECT_EQ(image->text(), "Preview Disabled");
  preview->setChecked(true);
  EXPECT_EQ(image->text(), "Waiting for preview…");
  preview->setChecked(false);
  EXPECT_EQ(image->text(), "Preview Disabled");
  EXPECT_TRUE(image->pixmap().isNull());
}

TEST_F(Gui, TheHelpButtonExplainsTheProtocolPort) {
  build();
  QString shown;
  // The message box runs its own event loop, so it has to be closed from inside.
  QTimer::singleShot(0, [&] {
    for (auto* dialog : QApplication::topLevelWidgets())
      if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
        shown = box->text();
        box->accept();
      }
  });
  find<QPushButton>("helpButton")->click();
  EXPECT_THAT(shown.toStdString(), HasSubstr("32100"));
}

TEST_F(Gui, SavingAndLoadingRoundTripsThroughTheConfigurationFile) {
  build();
  find<QLineEdit>("target")->setText(" mister.example ");
  find<QComboBox>("crop")->setCurrentIndex(int(CropMode::X4));
  find<QComboBox>("rotation")->setCurrentIndex(int(Rotation::CCW90));
  find<QComboBox>("sampling")->setCurrentIndex(int(SamplingMode::Bilinear));
  find<QSpinBox>("width")->setValue(720);
  find<QSpinBox>("height")->setValue(576);
  find<QSpinBox>("frameDelay")->setValue(6);
  find<QPushButton>("saveButton")->click();

  EXPECT_THAT(log().toStdString(), HasSubstr("Settings saved to"));
  const auto saved =
      loadGroovyConfig(directory->path() / "mistercast/config.json");
  EXPECT_EQ(saved.target, "mister.example") << "the address is trimmed";
  EXPECT_EQ(saved.source.crop, CropMode::X4);
  EXPECT_EQ(saved.source.rotation, Rotation::CCW90);
  EXPECT_EQ(saved.source.sampling, SamplingMode::Bilinear);
  EXPECT_EQ(saved.source.width, 720);
  EXPECT_EQ(saved.source.frameDelay, 6);

  // Editing then reloading must bring the file's values back.
  find<QSpinBox>("width")->setValue(320);
  find<QPushButton>("loadButton")->click();
  EXPECT_EQ(find<QSpinBox>("width")->value(), 720);
  EXPECT_THAT(log().toStdString(), HasSubstr("Settings loaded."));
}

TEST_F(Gui, ReportsAConfigurationThatCannotBeSaved) {
  build();
  // A regular file where the configuration directory has to go.
  directory->write("mistercast", "not a directory");
  find<QLineEdit>("target")->setText("mister.example");
  find<QPushButton>("saveButton")->click();
  EXPECT_THAT(log().toStdString(), HasSubstr("Settings:"));
}

// -------------------------------------------------------------- window chooser

TEST_F(Gui, TheWindowChooserCanBeDismissedWithoutSelectingAnything) {
  build();
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  bool opened = false;
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        opened = true;
        EXPECT_NE(dialog->findChild<QListWidget*>("windowList"), nullptr);
        dialog->reject();
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  EXPECT_TRUE(opened);
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected");
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled());
}

TEST_F(Gui, SelectingAWindowNamesItAndUnlocksStarting) {
  const ForeignWindows candidates({"gui-chooser-alpha", "gui-chooser-beta"});
  if (!candidates.ready()) GTEST_SKIP() << "no X11 display available";
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  find<QComboBox>("captureMode")->setCurrentIndex(1);

  int listed = 0;
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        auto* list = dialog->findChild<QListWidget*>("windowList");
        ASSERT_NE(list, nullptr);
        auto* buttons =
            dialog->findChild<QDialogButtonBox*>("windowChooserButtons");
        ASSERT_NE(buttons, nullptr);
        listed = list->count();
        // Nothing is current yet, so accepting must not be possible.
        EXPECT_FALSE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        for (int row = 0; row < list->count(); ++row)
          if (list->item(row)->text().startsWith("gui-chooser-alpha")) {
            list->setCurrentRow(row);
            // The size is offered alongside the title so two windows of the
            // same application can be told apart.
            EXPECT_TRUE(list->item(row)->text().contains("64×48"));
          }
        EXPECT_TRUE(buttons->button(QDialogButtonBox::Ok)->isEnabled());
        dialog->accept();
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  ASSERT_GE(listed, 2) << "both created windows must be offered";
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "gui-chooser-alpha");
  EXPECT_THAT(find<QLabel>("windowSelection")->toolTip().toStdString(),
              HasSubstr("64×48"));
  EXPECT_EQ(find<QComboBox>("captureMode")->currentIndex(), 1);
  EXPECT_TRUE(find<QPushButton>("streamButton")->isEnabled());

  // Re-opening preselects the window already chosen, and a double-click accepts.
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        auto* list = dialog->findChild<QListWidget*>("windowList");
        ASSERT_NE(list, nullptr);
        ASSERT_NE(list->currentItem(), nullptr) << "the choice is remembered";
        EXPECT_TRUE(list->currentItem()->text().startsWith("gui-chooser-alpha"));
        emit list->itemDoubleClicked(list->currentItem());
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "gui-chooser-alpha");
}

TEST_F(Gui, SwitchingBackToMonitorModeForgetsTheChosenWindow) {
  const ForeignWindows candidates({"gui-forget-me"});
  if (!candidates.ready()) GTEST_SKIP() << "no X11 display available";
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        auto* list = dialog->findChild<QListWidget*>("windowList");
        if (list && list->count() > 0) {
          list->setCurrentRow(0);
          dialog->accept();
        } else {
          dialog->reject();
        }
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  ASSERT_NE(find<QLabel>("windowSelection")->text(), "No window selected");

  find<QComboBox>("captureMode")->setCurrentIndex(0);
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected");
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled())
      << "the window has to be chosen again";
}

TEST_F(Gui, StreamsASingleWindowWhenWindowModeIsSelected) {
  const ForeignWindows candidates({"gui-window-stream"});
  if (!candidates.ready()) GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        auto* list = dialog->findChild<QListWidget*>("windowList");
        if (list && list->count() > 0) {
          list->setCurrentRow(0);
          dialog->accept();
        } else {
          dialog->reject();
        }
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  ASSERT_NE(find<QLabel>("windowSelection")->text(), "No window selected");

  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  EXPECT_TRUE(pumpUntil([&] { return receiver->blits() > 1; }));
  find<QPushButton>("streamButton")->click();
  EXPECT_EQ(find<QLabel>("status")->text(), "Idle");

  // The saved preference records window mode, but never the transient ID.
  const auto saved =
      loadGroovyConfig(directory->path() / "mistercast/config.json");
  EXPECT_EQ(saved.source.capturePreference, CapturePreference::Window);
}

// -------------------------------------------------------------- stream lifecycle

TEST_F(Gui, CannotBeAskedToStartAConfigurationTheProtocolRejects) {
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  // Timings the core has no framebuffer for. The button gate, not a later error,
  // is what has to stop this: the widget ranges cannot express any other kind of
  // invalid configuration, so this is the only way to reach an unstreamable one.
  find<QSpinBox>("hActive")->setValue(1024);
  find<QSpinBox>("hBegin")->setValue(1048);
  find<QSpinBox>("hEnd")->setValue(1184);
  find<QSpinBox>("hTotal")->setValue(1344);
  find<QSpinBox>("vActive")->setValue(768);
  find<QSpinBox>("vBegin")->setValue(771);
  find<QSpinBox>("vEnd")->setValue(777);
  find<QSpinBox>("vTotal")->setValue(806);
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled());
  find<QPushButton>("streamButton")->click();
  EXPECT_EQ(find<QLabel>("status")->text(), "Idle");
}

TEST_F(Gui, ReportsATargetThatIsNotListening) {
  writeConfig(streamableConfig());
  build();
  // No receiver is bound, so the transport cannot negotiate.
  find<QPushButton>("streamButton")->click();
  EXPECT_TRUE(pumpUntil(
      [&] { return find<QLabel>("status")->text() == "Error"; }));
  EXPECT_EQ(find<QPushButton>("streamButton")->text(), "Start Stream");
  EXPECT_THAT(log().toStdString(), HasSubstr("Start failed"));
  // Configuration controls must be usable again after a failure.
  EXPECT_TRUE(find<QComboBox>("captureMode")->isEnabled());
  EXPECT_TRUE(find<QLineEdit>("target")->isEnabled());
}

TEST_F(Gui, StreamsToAListeningTargetAndLocksTheSourceControls) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();

  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  EXPECT_EQ(find<QPushButton>("streamButton")->text(), "Stop Stream");
  EXPECT_THAT(log().toStdString(), HasSubstr("Streaming to 127.0.0.1."));

  // Source and audio controls lock; the timings deliberately stay editable.
  EXPECT_FALSE(find<QComboBox>("captureMode")->isEnabled());
  EXPECT_FALSE(find<QComboBox>("monitor")->isEnabled());
  EXPECT_FALSE(find<QCheckBox>("audio")->isEnabled());
  EXPECT_FALSE(find<QLineEdit>("target")->isEnabled());
  EXPECT_FALSE(find<QPushButton>("loadButton")->isEnabled());
  EXPECT_TRUE(find<QSpinBox>("hActive")->isEnabled());
  EXPECT_TRUE(find<QDoubleSpinBox>("pixelClock")->isEnabled());

  ASSERT_TRUE(pumpUntil([&] { return receiver->blits() > 2; }));
  // The periodic counters land in the status labels, not the log.
  ASSERT_TRUE(pumpUntil([&] {
    return !find<QLabel>("videoStatus")->text().isEmpty();
  }));
  EXPECT_THAT(find<QLabel>("videoStatus")->text().toStdString(),
              HasSubstr("capture"));
  EXPECT_THAT(find<QLabel>("transportStatus")->text().toStdString(),
              HasSubstr("Sync"));
  EXPECT_THAT(find<QLabel>("audioStatus")->text().toStdString(),
              HasSubstr("MiSTer off"));
  EXPECT_THAT(find<QLabel>("videoStatus")->toolTip().toStdString(),
              HasSubstr("Transform"));

  find<QPushButton>("streamButton")->click();
  EXPECT_EQ(find<QLabel>("status")->text(), "Idle");
  EXPECT_EQ(find<QPushButton>("streamButton")->text(), "Start Stream");
  EXPECT_THAT(log().toStdString(), HasSubstr("Stream stopped."));
  EXPECT_TRUE(find<QComboBox>("captureMode")->isEnabled());
  EXPECT_TRUE(find<QLabel>("videoStatus")->text().isEmpty())
      << "stale counters must be cleared when the stream ends";
  EXPECT_TRUE(pumpUntil([&] { return receiver->closes() >= 1; }));
}

TEST_F(Gui, StartingAStreamPersistsTheSettingsWithoutSayingSo) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QLineEdit>("target")->setText("127.0.0.1");
  find<QSpinBox>("frameDelay")->setValue(2);
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  EXPECT_THAT(log().toStdString(),
              testing::Not(HasSubstr("Settings saved to")));
  find<QPushButton>("streamButton")->click();

  const auto saved =
      loadGroovyConfig(directory->path() / "mistercast/config.json");
  EXPECT_EQ(saved.source.frameDelay, 2);
}

TEST_F(Gui, EditingTimingsWhileStreamingSwitchesThemLiveAfterADebounce) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] { return receiver->switchModes() >= 1; }));

  // One CMD_SWITCHRES per digit would be wrong: the change is debounced.
  find<QSpinBox>("vActive")->setValue(200);
  find<QSpinBox>("vBegin")->setValue(210);
  find<QSpinBox>("vEnd")->setValue(213);
  find<QSpinBox>("vTotal")->setValue(240);
  EXPECT_EQ(receiver->switchModes(), 1u) << "not switched yet";

  ASSERT_TRUE(pumpUntil([&] { return receiver->switchModes() >= 2; }));
  EXPECT_THAT(log().toStdString(), HasSubstr("Modeline switched live to"));
  EXPECT_THAT(receiver->activeHeights(), testing::Contains(200));
  EXPECT_EQ(find<QLabel>("status")->text(), "Streaming");
  find<QPushButton>("streamButton")->click();
}

TEST_F(Gui, EditingTimingsWhileIdleSwitchesNothing) {
  bindReceiver();
  build();
  find<QSpinBox>("vActive")->setValue(200);
  pumpFor(std::chrono::milliseconds(900));
  EXPECT_EQ(receiver->switchModes(), 0u);
  EXPECT_THAT(log().toStdString(),
              testing::Not(HasSubstr("Modeline switched live")));
}

TEST_F(Gui, ReportsFieldPhaseAndAdaptiveReserveForAnInterlacedMode) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  auto config = streamableConfig();
  config.modeline = {"480i", 12.336, 640, 662, 720, 784, 480, 488, 494, 525,
                     true};
  writeConfig(config);
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] {
    return !find<QLabel>("transportStatus")->text().isEmpty();
  }));
  const auto transport = find<QLabel>("transportStatus")->text().toStdString();
  find<QPushButton>("streamButton")->click();

  // An alternating field buffer adds the field/FPGA phase and the adaptive
  // delivery reserve to the transport line.
  EXPECT_THAT(transport, HasSubstr("field"));
  EXPECT_THAT(transport, HasSubstr("FPGA"));
  EXPECT_THAT(transport, HasSubstr("reserve"));
  EXPECT_THAT(transport, testing::AnyOf(HasSubstr("locked"),
                                        HasSubstr("acquiring")));
}

TEST_F(Gui, LogsInterlaceRealignmentsReportedByTheReceiver) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  // A receiver that always reports field one while the sender alternates: the
  // FPGA phase keeps contradicting the local one, which is exactly the
  // condition the interlace diagnostic exists for.
  bindReceiver(0xa4);  // synced, queue ready, FPGA field 1
  auto config = streamableConfig();
  config.modeline = {"480i", 12.336, 640, 662, 720, 784, 480, 488, 494, 525,
                     true};
  writeConfig(config);
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");

  const bool logged = pumpUntil(
      [&] {
        return log().toStdString().find("corrected field phase") !=
               std::string::npos;
      },
      std::chrono::seconds(25));
  find<QPushButton>("streamButton")->click();
  EXPECT_TRUE(logged) << log().toStdString();
}

TEST_F(Gui, ReportsAudioLevelAndCoreAudioStateWhileStreaming) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  // A private sound server, so enabling audio in the GUI cannot capture or
  // disturb whatever the desktop is playing.
  TemporaryDirectory audioDirectory("gui-pulse");
  PrivatePulseServer audioServer(audioDirectory.path());
  if (!audioServer.ready())
    GTEST_SKIP() << "a private pulseaudio server could not be started";
  const ScopedEnvironment pulse("PULSE_SERVER", audioServer.address().c_str());

  bindReceiver(0xc4);  // healthy, core audio on
  auto config = streamableConfig();
  config.source.audio = true;
  writeConfig(config);
  build();
  ASSERT_TRUE(find<QCheckBox>("audio")->isChecked());

  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming")
      << log().toStdString();
  ASSERT_TRUE(pumpUntil([&] {
    return !find<QLabel>("audioStatus")->text().isEmpty();
  }));
  const auto audio = find<QLabel>("audioStatus")->text().toStdString();
  const auto tooltip = find<QLabel>("audioStatus")->toolTip().toStdString();
  EXPECT_TRUE(pumpUntil([&] { return receiver->audioPackets() > 0; }));
  find<QPushButton>("streamButton")->click();

  EXPECT_THAT(audio, HasSubstr("MiSTer on"));
  EXPECT_THAT(audio, HasSubstr("level"));
  EXPECT_THAT(audio, testing::AnyOf(HasSubstr("healthy"),
                                    HasSubstr("pressure detected")));
  EXPECT_THAT(tooltip, HasSubstr("48000 Hz"));
}

TEST_F(Gui, ReportsThatTheAudioOutputsCannotBeListed) {
  // A sound server address that nothing answers on: the window has to open with
  // the built-in choices and say so, not fail to construct.
  const ScopedEnvironment pulse("PULSE_SERVER",
                                (directory->path() / "absent").c_str());
  build();
  EXPECT_THAT(log().toStdString(), HasSubstr("Audio outputs:"));
  EXPECT_GE(find<QComboBox>("audioSink")->count(), 2)
      << "the default and silent outputs are always offered";
  EXPECT_EQ(find<QLabel>("status")->text(), "Idle");
}

TEST_F(Gui, AcceptingAnEmptyChooserSelectsNothing) {
  // No windows are created here, so on a bare X server the list has nothing in
  // it and accepting must not invent a selection. A list with rows behaves
  // differently, because Qt makes the first row current for keyboard use.
  build();
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  bool wasEmpty = false;
  QTimer::singleShot(0, [&] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QDialog*>(top);
          dialog && dialog->objectName() == "windowChooser") {
        auto* list = dialog->findChild<QListWidget*>("windowList");
        wasEmpty = list && list->count() == 0;
        dialog->accept();
      }
  });
  find<QPushButton>("chooseWindowButton")->click();
  if (!wasEmpty)
    GTEST_SKIP() << "this X server exposes capturable windows of its own";
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected");
  EXPECT_FALSE(find<QPushButton>("streamButton")->isEnabled());
}

TEST_F(Gui, ApplyingAPresetWithNothingSelectedIsIgnored) {
  build();
  const auto before = find<QSpinBox>("hActive")->value();
  find<QComboBox>("preset")->setCurrentIndex(-1);
  find<QPushButton>("applyModelineButton")->click();
  EXPECT_EQ(find<QSpinBox>("hActive")->value(), before);
}

TEST_F(Gui, SwitchingLiveToAnInterlacedModeIsReportedAsInterlaced) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] { return receiver->switchModes() >= 1; }));

  const auto interlaced = bundledModelines().at(3);  // 640x480i
  find<QDoubleSpinBox>("pixelClock")->setValue(interlaced.pixelClockMHz);
  find<QSpinBox>("hActive")->setValue(interlaced.hActive);
  find<QSpinBox>("hBegin")->setValue(interlaced.hBegin);
  find<QSpinBox>("hEnd")->setValue(interlaced.hEnd);
  find<QSpinBox>("hTotal")->setValue(interlaced.hTotal);
  find<QSpinBox>("vActive")->setValue(interlaced.vActive);
  find<QSpinBox>("vBegin")->setValue(interlaced.vBegin);
  find<QSpinBox>("vEnd")->setValue(interlaced.vEnd);
  find<QSpinBox>("vTotal")->setValue(interlaced.vTotal);
  find<QCheckBox>("interlaced")->setChecked(true);
  ASSERT_TRUE(pumpUntil([&] { return receiver->switchModes() >= 2; }));
  EXPECT_THAT(log().toStdString(), HasSubstr("switched live to 640x480i"));
  EXPECT_EQ(receiver->lastInterlaceMode(), 1);
  find<QPushButton>("streamButton")->click();
}

TEST_F(Gui, TurningPreviewOffMidStreamStopsUpdatingTheImage) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  auto config = streamableConfig();
  config.source.preview = true;
  writeConfig(config);
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  auto* image = find<QLabel>("previewImage");
  ASSERT_TRUE(pumpUntil([&] { return !image->pixmap().isNull(); },
                        std::chrono::seconds(20)));

  // Unchecking while frames are still arriving: the posted updates have to be
  // dropped on the Qt thread rather than painted anyway.
  find<QCheckBox>("preview")->setChecked(false);
  EXPECT_EQ(image->text(), "Preview Disabled");
  pumpFor(std::chrono::milliseconds(600));
  EXPECT_TRUE(image->pixmap().isNull());
  find<QPushButton>("streamButton")->click();
}

TEST_F(Gui, ReportsThatTheWindowListCannotBeReadWithoutADisplay) {
  build();
  find<QComboBox>("captureMode")->setCurrentIndex(1);
  {
    // The chooser opens its own X11 connection, so removing DISPLAY makes the
    // enumeration fail without disturbing the already-running Qt application.
    const ScopedEnvironment display("DISPLAY", nullptr);
    find<QPushButton>("chooseWindowButton")->click();
  }
  EXPECT_THAT(log().toStdString(), HasSubstr("Windows:"));
  EXPECT_EQ(find<QLabel>("windowSelection")->text(), "No window selected");
}

TEST_F(Gui, InvalidTimingsEnteredWhileStreamingAreNotSwitchedLive) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] { return receiver->switchModes() >= 1; }));

  // Timings that cannot be ordered: the debounce still fires, but nothing may be
  // sent and the stream must carry on with the mode it already has.
  find<QSpinBox>("hTotal")->setValue(10);
  EXPECT_TRUE(find<QPushButton>("streamButton")->isEnabled())
      << "stopping must stay possible however bad the timings in the fields are";
  pumpFor(std::chrono::milliseconds(1100));
  EXPECT_EQ(receiver->switchModes(), 1u);
  EXPECT_THAT(log().toStdString(),
              testing::Not(HasSubstr("Modeline switched live")));
  EXPECT_EQ(find<QLabel>("status")->text(), "Streaming");
  find<QPushButton>("streamButton")->click();
}

TEST_F(Gui, ReportsAnUnsyncedReceiverWithAnEmptyQueue) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver(0x08);  // VGA frameskip only: not synced, no queue, audio off
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] {
    return !find<QLabel>("transportStatus")->text().isEmpty();
  }));
  const auto transport = find<QLabel>("transportStatus")->text().toStdString();
  find<QPushButton>("streamButton")->click();
  EXPECT_THAT(transport, HasSubstr("unsynced"));
  EXPECT_THAT(transport, HasSubstr("queue empty"));
}

TEST_F(Gui, ShowsTheSessionErrorWhenTheTargetDisappearsMidStream) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] { return receiver->blits() > 2; }));

  // The session reports the failure from a worker thread; the window has to
  // surface it on the Qt thread with its hint, and unlock the controls again.
  receiver.reset();
  ASSERT_TRUE(pumpUntil(
      [&] { return find<QLabel>("status")->text() == "Error"; },
      std::chrono::seconds(30)));
  const auto messages = log().toStdString();
  EXPECT_THAT(messages, HasSubstr("Hint:"));
  EXPECT_EQ(find<QPushButton>("streamButton")->text(), "Start Stream");
  EXPECT_TRUE(find<QComboBox>("captureMode")->isEnabled());
  EXPECT_TRUE(find<QLabel>("videoStatus")->text().isEmpty())
      << "counters are cleared when the stream is no longer running";
}

TEST_F(Gui, ClosingTheWindowStopsAnActiveStream) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  writeConfig(streamableConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  ASSERT_TRUE(pumpUntil([&] { return receiver->blits() > 1; }));

  window->close();
  EXPECT_TRUE(pumpUntil([&] { return receiver->closes() >= 1; }));
}

TEST_F(Gui, PreviewFramesReachTheLabelWhilePreviewIsEnabled) {
  if (!::getenv("DISPLAY") || !*::getenv("DISPLAY"))
    GTEST_SKIP() << "no X11 display available";
  bindReceiver();
  auto config = streamableConfig();
  config.source.preview = true;
  writeConfig(config);
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming");
  auto* image = find<QLabel>("previewImage");
  const bool arrived =
      pumpUntil([&] { return !image->pixmap().isNull(); },
                std::chrono::seconds(20));
  find<QPushButton>("streamButton")->click();
  EXPECT_TRUE(arrived) << "a scaled preview must be posted to the Qt thread";
}

}  // namespace

int main(int argc, char** argv) {
  ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
  ::testing::InitGoogleTest(&argc, argv);
  QApplication application(argc, argv);
  return RUN_ALL_TESTS();
}
