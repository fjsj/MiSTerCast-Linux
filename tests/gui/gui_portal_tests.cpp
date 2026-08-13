#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <cstdio>
#include <cstdlib>

#include "gui_test_support.hpp"
#include "mistercast/interfaces.hpp"
#include "support/portal_session.hpp"

using namespace mistercast;
using namespace mistercast::test;
using mistercast::test::portalSessionSkipCode;

namespace {

// The GUI driving the Wayland portal, in the rig from packaging/docker/ where
// there is a real compositor and a real portal whose picker is disabled.
//
// This exists because the window is where the portal's awkwardness lands: its
// dialog owns source selection, and the handshake blocks the thread that draws
// the window. None of that is visible to the `gui` suite, which pins itself to
// X11 precisely so it stays deterministic -- and none of it should depend on a
// person watching a window to find out whether it worked.
//
// Kept apart from the `gui` suite rather than folded into it, because a test that
// reaches a real portal captures the screen of whoever runs it. That is fine here
// and nowhere else; see the note in AGENTS.md.

class GuiPortal : public Gui {
 protected:
  AppConfig portalConfig() {
    auto config = streamableConfig();
    config.source.captureBackend = CaptureBackend::Portal;
    config.source.preview = true;
    return config;
  }
};

TEST_F(GuiPortal, StartsStopsAndRestartsAPortalStream) {
  bindReceiver();
  if (IsSkipped()) return;
  writeConfig(portalConfig());
  build();

  ASSERT_EQ(find<QComboBox>("backend")->currentIndex(),
            int(CaptureBackend::Portal));
  EXPECT_FALSE(find<QComboBox>("monitor")->isEnabled())
      << "the portal owns source selection";

  auto* button = find<QPushButton>("streamButton");
  ASSERT_TRUE(button->isEnabled());
  button->click();
  // The handshake runs on this thread, so by the time click() returns the portal
  // has already answered; what matters is that the window did not stay in
  // Starting… and that the log says what happened.
  ASSERT_TRUE(pumpUntil([&] {
    const auto state = find<QLabel>("status")->text();
    return state == "Streaming" || state == "Error";
  })) << log().toStdString();
  ASSERT_EQ(find<QLabel>("status")->text(), "Streaming")
      << log().toStdString();
  EXPECT_EQ(button->text(), "Stop Stream");

  // Frames have to reach the receiver, not merely be reported as sent.
  EXPECT_TRUE(pumpUntil([&] { return receiver->blits() > 0; }))
      << "no video reached the receiver: " << log().toStdString();

  // The preview is the only place the captured pixels are visible in the window,
  // and it is fed from the same frames.
  EXPECT_TRUE(pumpUntil(
      [&] { return !find<QLabel>("previewImage")->pixmap().isNull(); }))
      << "the preview never received a frame";

  button->click();
  ASSERT_TRUE(pumpUntil(
      [&] { return find<QLabel>("status")->text() == "Idle"; }))
      << log().toStdString();
  EXPECT_EQ(button->text(), "Start Stream");
  EXPECT_TRUE(find<QComboBox>("backend")->isEnabled())
      << "the controls have to come back after a portal stream";

  // Stopping releases the portal session, so starting again has to negotiate a
  // fresh one. A leaked session or an unclosed PipeWire remote breaks this.
  const auto before = receiver->blits();
  button->click();
  ASSERT_TRUE(pumpUntil([&] {
    const auto state = find<QLabel>("status")->text();
    return state == "Streaming" || state == "Error";
  })) << log().toStdString();
  EXPECT_EQ(find<QLabel>("status")->text(), "Streaming") << log().toStdString();
  EXPECT_TRUE(pumpUntil([&] { return receiver->blits() > before; }))
      << "the second portal session sent nothing";
}

// The grant is what spares the user a dialog on every run, and the GUI is where
// that matters most. The rig's portal issues no restore token, so this asserts
// the part that holds either way: whatever the portal grants is written where the
// next run will look, and never left readable by anyone else.
TEST_F(GuiPortal, StoresAnyGrantThePortalIssuesUserOnly) {
  bindReceiver();
  if (IsSkipped()) return;
  writeConfig(portalConfig());
  build();
  find<QPushButton>("streamButton")->click();
  ASSERT_TRUE(pumpUntil(
      [&] { return find<QLabel>("status")->text() == "Streaming"; }))
      << log().toStdString();
  find<QPushButton>("streamButton")->click();
  ASSERT_TRUE(pumpUntil(
      [&] { return find<QLabel>("status")->text() == "Idle"; }));

  const auto token = directory->path() / "mistercast/portal-token";
  if (!std::filesystem::exists(token))
    GTEST_SKIP() << "this portal backend issues no restore token";
  EXPECT_THAT(loadPortalRestoreToken(token.string()),
              testing::Not(testing::IsEmpty()))
      << "a stored token has to be one the next run will accept";
  const auto permissions = std::filesystem::status(token).permissions();
  EXPECT_EQ(permissions & (std::filesystem::perms::group_all |
                           std::filesystem::perms::others_all),
            std::filesystem::perms::none);
}

}  // namespace

int main(int argc, char** argv) {
  ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
  ::testing::InitGoogleTest(&argc, argv);
  if (const int skip = portalSessionSkipCode("GUI portal tests")) return skip;
  QApplication application(argc, argv);
  return RUN_ALL_TESTS();
}
