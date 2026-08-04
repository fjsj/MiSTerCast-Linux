#pragma once

#include <QMainWindow>
#include <QTimer>
#include <optional>
#include <vector>

#include "mistercast/config.hpp"
#include "mistercast/stream_session.hpp"
#include "mistercast/types.hpp"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace mistercast::gui {

// Every interactive widget carries an objectName matching its member name
// without the trailing underscore, so tests can reach it with
// findChild<QPushButton*>("streamButton") instead of needing accessors that
// exist only for testing. The window-chooser dialog names itself
// "windowChooser" and its list "windowList" while it is open.
class MainWindow final : public QMainWindow {
 public:
  MainWindow();
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void append(const QString& message);
  void append(const std::string& message);
  void clearWindowSelection();
  void refreshStartEnabled();
  void refreshConfigurationEnabled(SessionState state);
  void chooseWindow();
  void scheduleModelineApply();
  void applyModelineLive();
  void showState(SessionState state);
  Modeline modelineFromControls() const;
  void setModelineControls(const Modeline& modeline);
  void configFromControls();
  void controlsFromConfig();
  void saveSettings(bool announce = true);
  void loadSettings();
  void toggleStream();
  void updateStats();
  static QSpinBox* timingSpin();

  AppConfig config_{loadGroovyConfig(configPath())};
  StreamSession session_;
  std::vector<Modeline> presets_;
  std::optional<CaptureWindow> selectedWindow_;

  QPushButton *streamButton_{}, *saveButton_{}, *loadButton_{},
      *applyModelineButton_{}, *chooseWindowButton_{};
  QLineEdit* target_{};
  QComboBox *captureMode_{}, *monitor_{}, *audioSink_{}, *preset_{}, *crop_{},
      *alignment_{}, *rotation_{}, *sampling_{};
  QDoubleSpinBox* pixelClock_{};
  QSpinBox *hActive_{}, *hBegin_{}, *hEnd_{}, *hTotal_{};
  QSpinBox *vActive_{}, *vBegin_{}, *vEnd_{}, *vTotal_{};
  QSpinBox *width_{}, *height_{}, *xOffset_{}, *yOffset_{}, *frameDelay_{};
  QCheckBox *interlaced_{}, *progressiveInterlaceBuffer_{}, *audio_{},
      *preview_{};
  QLabel *windowSelection_{}, *previewImage_{}, *status_{}, *videoStatus_{},
      *transportStatus_{}, *audioStatus_{};
  QPlainTextEdit* log_{};
  std::vector<QWidget*> streamLockedControls_;
  QTimer statsTimer_, modelineApplyTimer_;
  uint64_t previousDropped_{}, previousAudioDropped_{}, previousUnderrun_{},
      previousSendErrors_{}, previousFieldRealignments_{};
};

}  // namespace mistercast::gui
