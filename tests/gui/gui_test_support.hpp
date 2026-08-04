#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QPlainTextEdit>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "gui/main_window.hpp"
#include "mistercast/config.hpp"
#include "support/fake_groovy_endpoint.hpp"
#include "support/fake_mister.hpp"
#include "support/groovy_wire.hpp"
#include "support/scoped_environment.hpp"
#include "support/temp_directory.hpp"

namespace mistercast::test {

using mistercast::gui::MainWindow;

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
    silent.reset();
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

  // GTEST_SKIP() returns from the function it appears in, so a skip raised here
  // only aborts this helper. Every call site follows it with
  // `if (IsSkipped()) return;` — without that the body runs on against an
  // unbound receiver and fails instead of skipping.
  void bindReceiver(uint8_t statusBits = kHealthy) {
    silent.reset();  // free the port if this test was holding it
    receiver = std::make_unique<FakeMister>(statusBits);
    if (!receiver->bound())
      GTEST_SKIP() << "UDP port 32100 is already in use on this machine";
  }

  // Owns UDP 32100 without ever answering, for the tests whose premise is that
  // nothing is listening. See the SessionTest fixture for why unbound is not
  // enough.
  void holdPortSilently() {
    silent = std::make_unique<FakeGroovyEndpoint>(
        [](FakeGroovyEndpoint&, const FakeGroovyEndpoint::Packet&) {}, 32100);
    if (!silent->valid())
      GTEST_SKIP() << "UDP port 32100 is already in use on this machine";
  }

  // objectNames are the test's only handle on a widget and nothing checks them
  // at compile time, so a member renamed without its objectName has to fail the
  // test that looks for it. Returning null would instead crash the whole suite
  // on the next dereference; googletest reports the throw as a failure.
  template <class Widget>
  Widget* find(const char* name) const {
    auto* widget = window->findChild<Widget*>(QString::fromLatin1(name));
    if (!widget)
      throw std::runtime_error(std::string("no widget named ") + name);
    return widget;
  }

  QString log() const {
    return find<QPlainTextEdit>("log")->toPlainText();
  }

  std::unique_ptr<TemporaryDirectory> directory;
  std::unique_ptr<ScopedEnvironment> configHome;
  std::unique_ptr<FakeGroovyEndpoint> silent;
  std::unique_ptr<FakeMister> receiver;
  std::unique_ptr<MainWindow> window;
};

}  // namespace mistercast::test
