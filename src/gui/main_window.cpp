#include "main_window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QListWidget>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>
#include <csignal>

#include "mistercast/groovy_protocol.hpp"
#include "mistercast/interfaces.hpp"

using namespace mistercast;

namespace {
volatile std::sig_atomic_t interrupted = 0;

void signalHandler(int) { interrupted = 1; }

QString stateName(SessionState state) {
  switch (state) {
    case SessionState::Idle:
      return "Idle";
    case SessionState::Starting:
      return "Starting";
    case SessionState::Streaming:
      return "Streaming";
    case SessionState::Stopping:
      return "Stopping";
    case SessionState::Error:
      return "Error";
  }
  return "Unknown";
}

// Names every widget the tests address. Returning the widget keeps the
// construction below reading as one expression per control.
template <class Widget>
Widget* named(Widget* widget, const char* name) {
  widget->setObjectName(QString::fromLatin1(name));
  return widget;
}
}  // namespace

namespace mistercast::gui {

void MainWindow::append(const QString& message) {
  log_->appendPlainText(message);
}

void MainWindow::append(const std::string& message) {
  append(QString::fromStdString(message));
}

void MainWindow::clearWindowSelection() {
  selectedWindow_.reset();
  windowSelection_->setText(QStringLiteral("No window selected"));
  windowSelection_->setToolTip({});
}

void MainWindow::refreshStartEnabled() {
  const auto state = session_.state();
  const bool busy =
      state == SessionState::Starting || state == SessionState::Stopping;
  const bool valid = !target_->text().trimmed().isEmpty() &&
                     validateGroovyModeline(modelineFromControls()) ==
                         std::nullopt &&
                     (captureMode_->currentIndex() == 0 || selectedWindow_);
  streamButton_->setEnabled(!busy &&
                            (state == SessionState::Streaming || valid));
}

void MainWindow::refreshConfigurationEnabled(SessionState state) {
  const bool enabled =
      state == SessionState::Idle || state == SessionState::Error;
  for (auto* control : streamLockedControls_) control->setEnabled(enabled);
  // The monitor chooser and the window chooser share one row; only the pair
  // that matches the capture mode is shown.
  const bool windowMode = captureMode_->currentIndex() == 1;
  chooserLabelStack_->setCurrentWidget(
      windowMode ? static_cast<QWidget*>(chooseWindowButton_) : monitorLabel_);
  chooserFieldStack_->setCurrentWidget(
      windowMode ? static_cast<QWidget*>(windowSelection_) : monitor_);
  monitor_->setEnabled(enabled && !windowMode);
  chooseWindowButton_->setEnabled(enabled && windowMode);
  if (enabled) audioSink_->setEnabled(audio_->isChecked());
  // Size describes the custom crop rectangle and every other crop mode
  // computes it itself; offset nudges the crop position in every mode
  // (calculateCrop adds it after alignment unconditionally), so it stays
  // editable whatever the crop.
  const bool customCrop = crop_->currentIndex() == int(CropMode::Custom);
  for (auto* spin : {width_, height_}) spin->setEnabled(enabled && customCrop);
  // Interlace buffering tracks the interlaced flag whether or not a stream is
  // running, because timings are now switched live.
  progressiveInterlaceBuffer_->setEnabled(interlaced_->isChecked());
}

void MainWindow::chooseWindow() {
  std::string error;
  const auto windows = x11CaptureWindows(error);
  if (!error.empty()) {
    append("Windows: " + error);
    return;
  }
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("windowChooser"));
  dialog.setWindowTitle("Choose a window to share");
  dialog.resize(560, 420);
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(
      "Select one visible X11 window. Minimized windows are unavailable."));
  auto* list = named(new QListWidget, "windowList");
  for (size_t index = 0; index < windows.size(); ++index) {
    const auto& window = windows[index];
    auto* item = new QListWidgetItem(
        QString("%1  —  %2×%3")
            .arg(QString::fromStdString(window.title))
            .arg(window.width)
            .arg(window.height),
        list);
    item->setData(Qt::UserRole, qulonglong(index));
    if (selectedWindow_ && window.id == selectedWindow_->id)
      list->setCurrentItem(item);
  }
  layout->addWidget(list);
  auto* buttons = named(new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel),
                        "windowChooserButtons");
  buttons->button(QDialogButtonBox::Ok)->setEnabled(list->currentItem());
  connect(list, &QListWidget::currentItemChanged, &dialog,
          [buttons](QListWidgetItem* current) {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(current);
          });
  connect(list, &QListWidget::itemDoubleClicked, &dialog,
          [&dialog](QListWidgetItem*) { dialog.accept(); });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted || !list->currentItem()) return;
  const auto* item = list->currentItem();
  selectedWindow_ = windows.at(item->data(Qt::UserRole).toULongLong());
  windowSelection_->setText(QString::fromStdString(selectedWindow_->title));
  windowSelection_->setToolTip(item->text());
  captureMode_->setCurrentIndex(1);
  refreshStartEnabled();
}

// Rebuilds the preset chooser from the bundled and custom modelines while
// keeping the current selection, without re-applying whatever lands at index 0.
void MainWindow::refreshPresetChoices() {
  presets_ = bundledModelines();
  presets_.insert(presets_.end(), config_.customModelines.begin(),
                  config_.customModelines.end());
  const QSignalBlocker blocker(preset_);
  const auto current = preset_->currentText();
  preset_->clear();
  for (const auto& preset : presets_)
    preset_->addItem(QString::fromStdString(preset.name));
  const auto index = preset_->findText(current);
  if (index >= 0) preset_->setCurrentIndex(index);
}

// Custom presets are persisted the moment they are edited, independently of
// the ask-on-close flow for the other settings: the file's other values stay
// exactly as last saved.
void MainWindow::persistCustomModelines() {
  auto saved = loadGroovyConfig(configPath());
  saved.customModelines = config_.customModelines;
  savedConfig_.customModelines = config_.customModelines;
  std::string error;
  if (!saveGroovyConfig(saved, configPath(), error))
    append("Presets: " + error);
}

void MainWindow::managePresets() {
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("presetEditor"));
  dialog.setWindowTitle("Edit Modeline Presets");
  auto* layout = new QVBoxLayout(&dialog);
  // The session-long timing fields are shown here and returned to their
  // hidden home under the central widget before the dialog destroys its
  // children. Edits switch the modeline live while streaming, editor open.
  layout->addWidget(timingsBox_);
  timingsBox_->show();
  layout->addWidget(
      new QLabel("Save the timings above as a preset. Custom presets are "
                 "stored in your configuration."));
  auto* list = named(new QListWidget, "customPresetList");
  for (const auto& modeline : config_.customModelines)
    list->addItem(QString::fromStdString(modeline.name));
  layout->addWidget(list);
  auto* nameRow = new QHBoxLayout;
  auto* name = named(new QLineEdit, "presetName");
  name->setPlaceholderText("New preset name");
  auto* add = named(new QPushButton("Save as Preset"), "addPresetButton");
  add->setEnabled(false);
  nameRow->addWidget(name, 1);
  nameRow->addWidget(add);
  layout->addLayout(nameRow);
  auto* remove = named(new QPushButton("Remove Selected"), "removePresetButton");
  remove->setEnabled(false);
  auto* feedback = named(new QLabel, "presetEditorStatus");
  auto* bottomRow = new QHBoxLayout;
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  bottomRow->addWidget(remove);
  bottomRow->addWidget(feedback, 1);
  bottomRow->addWidget(buttons);
  layout->addLayout(bottomRow);

  connect(name, &QLineEdit::textChanged, add, [add](const QString& text) {
    add->setEnabled(!text.trimmed().isEmpty());
  });
  connect(list, &QListWidget::currentItemChanged, remove,
          [remove](QListWidgetItem* current) { remove->setEnabled(current); });
  connect(add, &QPushButton::clicked, &dialog, [this, name, list, feedback] {
    auto modeline = modelineFromControls();
    modeline.name = name->text().trimmed().toStdString();
    if (auto problem = validateGroovyModeline(modeline)) {
      feedback->setText(QString::fromStdString(*problem));
      return;
    }
    for (const auto& bundled : bundledModelines())
      if (bundled.name == modeline.name) {
        feedback->setText("That name belongs to a bundled preset.");
        return;
      }
    config_.customModelines.erase(
        std::remove_if(config_.customModelines.begin(),
                       config_.customModelines.end(),
                       [&](const Modeline& existing) {
                         return existing.name == modeline.name;
                       }),
        config_.customModelines.end());
    config_.customModelines.push_back(modeline);
    persistCustomModelines();
    refreshPresetChoices();
    list->clear();
    for (const auto& custom : config_.customModelines)
      list->addItem(QString::fromStdString(custom.name));
    preset_->setCurrentIndex(
        preset_->findText(QString::fromStdString(modeline.name)));
    feedback->setText(QString("Preset \"%1\" saved.")
                          .arg(QString::fromStdString(modeline.name)));
    name->clear();
  });
  connect(remove, &QPushButton::clicked, &dialog, [this, list, feedback] {
    auto* item = list->currentItem();
    if (!item) return;
    const auto removedName = item->text().toStdString();
    config_.customModelines.erase(
        std::remove_if(config_.customModelines.begin(),
                       config_.customModelines.end(),
                       [&](const Modeline& existing) {
                         return existing.name == removedName;
                       }),
        config_.customModelines.end());
    persistCustomModelines();
    refreshPresetChoices();
    delete item;
    feedback->setText(QString("Preset \"%1\" removed.")
                          .arg(QString::fromStdString(removedName)));
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  dialog.exec();
  timingsBox_->hide();
  timingsBox_->setParent(centralWidget());
}

// Editing timings fires a change per field, so the live switch is debounced
// until the user stops typing rather than sending a CMD_SWITCHRES per digit.
void MainWindow::scheduleModelineApply() {
  if (session_.state() == SessionState::Streaming)
    modelineApplyTimer_.start();
}

void MainWindow::applyModelineLive() {
  if (session_.state() != SessionState::Streaming) return;
  const auto modeline = modelineFromControls();
  if (validateGroovyModeline(modeline)) return;
  const bool progressive = progressiveInterlaceBuffer_->isChecked();
  std::string error;
  if (!session_.updateModeline(modeline, progressive, &error)) {
    append("Modeline: " + error);
    return;
  }
  config_.modeline = modeline;
  config_.source.progressiveInterlaceBuffer = progressive;
  append(QString("Modeline switched live to %1x%2%3.")
             .arg(modeline.hActive)
             .arg(modeline.vActive)
             .arg(modeline.interlaced ? "i" : "p"));
}

void MainWindow::showState(SessionState state) {
  switch (state) {
    case SessionState::Starting:
      streamButton_->setText("Starting…");
      break;
    case SessionState::Streaming:
      streamButton_->setText("Stop Stream");
      break;
    case SessionState::Stopping:
      streamButton_->setText("Stopping…");
      break;
    case SessionState::Idle:
    case SessionState::Error:
      streamButton_->setText("Start Stream");
      break;
  }
  status_->setText(stateName(state));
  if (state != SessionState::Streaming) {
    videoStatus_->clear();
    transportStatus_->clear();
    audioStatus_->clear();
  }
  refreshConfigurationEnabled(state);
  refreshStartEnabled();
}

Modeline MainWindow::modelineFromControls() const {
  Modeline result;
  result.name = preset_->currentText().toStdString();
  result.pixelClockMHz = pixelClock_->value();
  result.hActive = uint16_t(hActive_->value());
  result.hBegin = uint16_t(hBegin_->value());
  result.hEnd = uint16_t(hEnd_->value());
  result.hTotal = uint16_t(hTotal_->value());
  result.vActive = uint16_t(vActive_->value());
  result.vBegin = uint16_t(vBegin_->value());
  result.vEnd = uint16_t(vEnd_->value());
  result.vTotal = uint16_t(vTotal_->value());
  result.interlaced = interlaced_->isChecked();
  return result;
}

void MainWindow::setModelineControls(const Modeline& modeline) {
  pixelClock_->setValue(modeline.pixelClockMHz);
  hActive_->setValue(modeline.hActive);
  hBegin_->setValue(modeline.hBegin);
  hEnd_->setValue(modeline.hEnd);
  hTotal_->setValue(modeline.hTotal);
  vActive_->setValue(modeline.vActive);
  vBegin_->setValue(modeline.vBegin);
  vEnd_->setValue(modeline.vEnd);
  vTotal_->setValue(modeline.vTotal);
  interlaced_->setChecked(modeline.interlaced);
  progressiveInterlaceBuffer_->setEnabled(modeline.interlaced);
  refreshStartEnabled();
}

void MainWindow::configFromControls() {
  config_.target = target_->text().trimmed().toStdString();
  config_.source.monitor = monitor_->currentText().toStdString();
  config_.source.capturePreference =
      captureMode_->currentIndex() == 1 ? CapturePreference::Window
                                        : CapturePreference::Monitor;
  config_.source.audioSink =
      audioSink_->currentData().toString().toStdString();
  config_.source.audio = audio_->isChecked();
  config_.source.preview = preview_->isChecked();
  config_.source.width = uint16_t(width_->value());
  config_.source.height = uint16_t(height_->value());
  config_.source.xOffset = int16_t(xOffset_->value());
  config_.source.yOffset = int16_t(yOffset_->value());
  config_.source.frameDelay = uint16_t(frameDelay_->value());
  config_.source.progressiveInterlaceBuffer =
      progressiveInterlaceBuffer_->isChecked();
  config_.source.crop = CropMode(crop_->currentIndex());
  config_.source.alignment = Alignment(alignment_->currentIndex());
  config_.source.rotation = Rotation(rotation_->currentIndex());
  config_.source.sampling = SamplingMode(sampling_->currentIndex());
  config_.modeline = modelineFromControls();
}

void MainWindow::controlsFromConfig() {
  target_->setText(QString::fromStdString(config_.target));
  auto monitorIndex =
      monitor_->findText(QString::fromStdString(config_.source.monitor));
  if (monitorIndex >= 0) monitor_->setCurrentIndex(monitorIndex);
  captureMode_->setCurrentIndex(config_.source.capturePreference ==
                                        CapturePreference::Window
                                    ? 1
                                    : 0);
  clearWindowSelection();
  auto audioSinkIndex =
      audioSink_->findData(QString::fromStdString(config_.source.audioSink));
  if (audioSinkIndex < 0 && !config_.source.audioSink.empty()) {
    audioSink_->addItem(QString::fromStdString(config_.source.audioSink),
                        QString::fromStdString(config_.source.audioSink));
    audioSinkIndex = audioSink_->count() - 1;
  }
  audioSink_->setCurrentIndex(std::max(audioSinkIndex, 0));
  crop_->setCurrentIndex(int(config_.source.crop));
  alignment_->setCurrentIndex(int(config_.source.alignment));
  rotation_->setCurrentIndex(int(config_.source.rotation));
  sampling_->setCurrentIndex(int(config_.source.sampling));
  width_->setValue(config_.source.width);
  height_->setValue(config_.source.height);
  xOffset_->setValue(config_.source.xOffset);
  yOffset_->setValue(config_.source.yOffset);
  frameDelay_->setValue(config_.source.frameDelay);
  progressiveInterlaceBuffer_->setChecked(
      config_.source.progressiveInterlaceBuffer);
  audio_->setChecked(config_.source.audio);
  preview_->setChecked(config_.source.preview);
  auto presetIndex =
      preset_->findText(QString::fromStdString(config_.modeline.name));
  if (presetIndex >= 0) preset_->setCurrentIndex(presetIndex);
  setModelineControls(config_.modeline);
  refreshConfigurationEnabled(session_.state());
}

void MainWindow::saveSettings() {
  configFromControls();
  std::string error;
  if (!saveGroovyConfig(config_, configPath(), error)) {
    append("Settings: " + error);
    return;
  }
  savedConfig_ = config_;
  append("Settings saved to " + configPath().string());
}

void MainWindow::loadSettings() {
  std::string warning;
  config_ = loadGroovyConfig(configPath(), &warning);
  refreshPresetChoices();
  controlsFromConfig();
  // The baseline is the loaded file as the controls normalize it, so closing
  // without further edits never asks about differences the load itself made.
  configFromControls();
  savedConfig_ = config_;
  append(warning.empty() ? "Settings loaded." : warning);
}

void MainWindow::toggleStream() {
  if (session_.state() == SessionState::Streaming) {
    showState(SessionState::Stopping);
    session_.stop();
    showState(SessionState::Idle);
    append(QStringLiteral("Stream stopped."));
    return;
  }

  configFromControls();
  if (auto validation = validateGroovyConfig(config_)) {
    append("Configuration: " + *validation);
    return;
  }

  showState(SessionState::Starting);
  std::string error;
  const CaptureSource source =
      captureMode_->currentIndex() == 1
          ? CaptureSource{WindowCaptureSource{selectedWindow_->id}}
          : CaptureSource{MonitorCaptureSource{
                monitor_->currentText().toStdString()}};
  const bool started = session_.start(
      config_, source,
      [this](SessionState state, const std::optional<SessionError>& problem) {
        QTimer::singleShot(0, this, [this, state, problem] {
          showState(state);
          if (problem) {
            append(QString::fromStdString(problem->component + ": " +
                                          problem->message));
            if (!problem->hint.empty())
              append(QString::fromStdString("Hint: " + problem->hint));
          }
        });
      },
      &error);
  if (!started) {
    showState(SessionState::Error);
    if (!error.empty()) append("Start failed: " + error);
    return;
  }
  showState(SessionState::Streaming);
  previousDropped_ = previousAudioDropped_ = previousUnderrun_ = 0;
  previousSendErrors_ = previousFieldRealignments_ = 0;
  append("Streaming to " + config_.target + ".");
}

void MainWindow::updateStats() {
  if (session_.state() != SessionState::Streaming) return;
  const auto stats = session_.stats();
  const double audioMs =
      stats.audioSampleRate
          ? stats.audioBufferedSamples * 500.0 / stats.audioSampleRate
          : 0.0;
  const double refresh = modelineFromControls().refreshHz();
  const double streamPercent = refresh > 0 ? stats.streamFps * 100 / refresh : 0;
  const double capturePercent =
      refresh > 0 ? stats.captureFps * 100 / refresh : 0;
  const double droppedPercent = stats.capturedFrames
                                    ? stats.droppedFrames * 100.0 /
                                          stats.capturedFrames
                                    : 0;
  const double queuePercent = stats.transport.socketSendBufferBytes
                                  ? stats.transport.observedUdpQueueHighWater *
                                        100.0 /
                                        stats.transport.socketSendBufferBytes
                                  : 0;
  const double reservePercent = vTotal_->value()
                                    ? stats.transport.deliveryReserveLines *
                                          100.0 / vTotal_->value()
                                    : 0;
  QString fieldStatus;
  if (stats.transport.interlacedFieldBuffer)
    fieldStatus = QString(" · field %1/FPGA %2 %3 · reserve %4%")
                      .arg(stats.transport.outgoingField)
                      .arg(stats.transport.fpgaField)
                      .arg(stats.transport.fieldPhaseValid ? "locked"
                                                           : "acquiring")
                      .arg(reservePercent, 0, 'f', 1);
  status_->setText("Streaming");
  videoStatus_->setText(
      QString("Video %1% · capture %2% · dropped %3% · transform %4 µs")
          .arg(streamPercent, 0, 'f', 0)
          .arg(capturePercent, 0, 'f', 0)
          .arg(droppedPercent, 0, 'f', 1)
          .arg(stats.transformTimeUs));
  videoStatus_->setToolTip(
      QString("Video: %1 fps (%2 frames)\nCapture: %3 fps (%4 frames)\n"
              "Dropped: %5 frames\nTransform: %6 µs EWMA, %7 µs maximum")
          .arg(stats.streamFps, 0, 'f', 2)
          .arg(stats.sentFrames)
          .arg(stats.captureFps, 0, 'f', 2)
          .arg(stats.capturedFrames)
          .arg(stats.droppedFrames)
          .arg(stats.transformTimeUs)
          .arg(stats.transformMaxUs));
  transportStatus_->setText(
      QString("Sync %1/%2 · FPGA %3 · queue %4 · UDP peak %5% · late %6%7")
          .arg(stats.transport.requestedSyncLine)
          .arg(stats.transport.fpgaVCount)
          .arg(stats.transport.vramSynced
                   ? (stats.transport.vgaFrameskip ? "fallback" : "synced")
                   : "unsynced")
          .arg(stats.transport.vramQueuePresent ? "ready" : "empty")
          .arg(queuePercent, 0, 'f', 1)
          .arg(stats.transport.lateBatchReleases)
          .arg(fieldStatus));
  transportStatus_->setToolTip(
      QString("Raster correction: %1 µs\nCompression/submission/wire: "
              "%2/%3/%4 µs\nUDP queue peak: %5 of %6 bytes\n"
              "Path MTU: %7 bytes\nPaced payloads/datagrams: %8/%9\n"
              "Maximum batch lateness: %10 µs\nAdaptive reserve/latest: "
              "%11/%12 lines\nHealthy ACKs/steps/resets: %13/%14/%15")
          .arg(stats.transport.rasterCorrectionUs)
          .arg(stats.transport.compressionTimeUs)
          .arg(stats.transport.submissionTimeUs)
          .arg(stats.transport.estimatedWireTimeUs)
          .arg(stats.transport.observedUdpQueueHighWater)
          .arg(stats.transport.socketSendBufferBytes)
          .arg(stats.transport.pathMtu)
          .arg(stats.transport.pacedVideoPayloads)
          .arg(stats.transport.pacedDatagrams)
          .arg(stats.transport.maxBatchReleaseLatenessNs / 1000)
          .arg(stats.transport.deliveryReserveLines)
          .arg(stats.transport.adaptiveLatestSafeLine)
          .arg(stats.transport.adaptiveHealthyAcks)
          .arg(stats.transport.adaptiveReductions)
          .arg(stats.transport.adaptiveResets));
  const bool audioHealthy =
      stats.audioDroppedSamples == 0 && stats.audioUnderrunSamples == 0;
  audioStatus_->setText(
      QString("Audio %1 ms · level %2% · MiSTer %3 · %4")
          .arg(audioMs, 0, 'f', 0)
          .arg(stats.audioPeak * 100, 0, 'f', 0)
          .arg(stats.misterAudioEnabled ? "on" : "off")
          .arg(audioHealthy ? "healthy" : "pressure detected"));
  audioStatus_->setToolTip(
      QString("Buffered: %1 samples at %2 Hz\nOverrun drops: %3 samples\n"
              "Underrun silence: %4 samples")
          .arg(stats.audioBufferedSamples)
          .arg(stats.audioSampleRate)
          .arg(stats.audioDroppedSamples)
          .arg(stats.audioUnderrunSamples));
  if (stats.droppedFrames > previousDropped_ + 30) {
    append(QString("Performance: %1 video frames dropped.")
               .arg(stats.droppedFrames));
    previousDropped_ = stats.droppedFrames;
  }
  if (stats.audioDroppedSamples > previousAudioDropped_) {
    append(QString("Performance: audio overrun dropped %1 samples.")
               .arg(stats.audioDroppedSamples));
    previousAudioDropped_ = stats.audioDroppedSamples;
  }
  if (stats.transport.sendErrors > previousSendErrors_) {
    append(QString("Network: %1 datagrams could not be sent (stream "
                   "continues).")
               .arg(stats.transport.sendErrors));
    previousSendErrors_ = stats.transport.sendErrors;
  }
  if (stats.transport.fieldRealignments > previousFieldRealignments_) {
    append(QString("Interlace: FPGA feedback corrected field phase (%1 "
                   "total).")
               .arg(stats.transport.fieldRealignments));
    previousFieldRealignments_ = stats.transport.fieldRealignments;
  }
  if (stats.audioUnderrunSamples > previousUnderrun_ + 4800) {
    append(QString("Performance: audio underrun inserted %1 silent samples.")
               .arg(stats.audioUnderrunSamples));
    previousUnderrun_ = stats.audioUnderrunSamples;
  }
}

QSpinBox* MainWindow::timingSpin() {
  auto* spin = new QSpinBox;
  spin->setRange(1, 8192);
  return spin;
}

MainWindow::MainWindow() {
  setWindowTitle("MiSTerCast 1.0 — Linux/X11");
  auto* central = new QWidget;
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(10, 8, 10, 8);
  root->setSpacing(6);

  auto* controlsBox = new QGroupBox("Controls");
  auto* controls = new QHBoxLayout(controlsBox);
  streamButton_ = named(new QPushButton("Start Stream"), "streamButton");
  saveButton_ = named(new QPushButton("Save Settings"), "saveButton");
  loadButton_ = named(new QPushButton("Load Settings"), "loadButton");
  target_ = named(new QLineEdit(QString::fromStdString(config_.target)),
                  "target");
  target_->setPlaceholderText("MiSTer IPv4 or hostname");
  auto* help = named(new QPushButton("?"), "helpButton");
  help->setFixedWidth(34);
  controls->addWidget(streamButton_, 2);
  controls->addWidget(saveButton_, 2);
  controls->addWidget(loadButton_, 2);
  controls->addStretch();
  controls->addWidget(new QLabel("Target IP"));
  controls->addWidget(target_, 2);
  controls->addWidget(help);
  root->addWidget(controlsBox);

  auto* presetBox = new QGroupBox("Modeline Presets");
  auto* presetLayout = new QHBoxLayout(presetBox);
  preset_ = named(new QComboBox, "preset");
  preset_->setToolTip(
      "Selecting a preset applies its timings immediately, switching the "
      "modeline live while a stream is running. Edit Presets… opens the "
      "timing fields.");
  managePresetsButton_ =
      named(new QPushButton("Edit Presets…"), "managePresetsButton");
  presetLayout->addWidget(preset_, 1);
  presetLayout->addWidget(managePresetsButton_);
  root->addWidget(presetBox);

  // The timing fields live inside the preset editor dialog to keep the main
  // window small, but the widgets themselves last the whole session: values
  // set on them (presets, loads, tests) keep driving validation and live
  // switching while the editor is closed.
  timingsBox_ = named(new QGroupBox("Modeline"), "timingsBox");
  auto* timings = new QGridLayout(timingsBox_);
  pixelClock_ = named(new QDoubleSpinBox, "pixelClock");
  pixelClock_->setRange(0.1, 400);
  pixelClock_->setDecimals(3);
  hActive_ = named(timingSpin(), "hActive");
  hBegin_ = named(timingSpin(), "hBegin");
  hEnd_ = named(timingSpin(), "hEnd");
  hTotal_ = named(timingSpin(), "hTotal");
  vActive_ = named(timingSpin(), "vActive");
  vBegin_ = named(timingSpin(), "vBegin");
  vEnd_ = named(timingSpin(), "vEnd");
  vTotal_ = named(timingSpin(), "vTotal");
  interlaced_ = named(new QCheckBox, "interlaced");
  const char* topLabels[] = {"Pclock", "Hactive", "Hbegin", "Hend", "Htotal"};
  QWidget* topFields[] = {pixelClock_, hActive_, hBegin_, hEnd_, hTotal_};
  const char* bottomLabels[] = {"Vactive", "Vbegin", "Vend", "Vtotal",
                                "Interlace"};
  QWidget* bottomFields[] = {vActive_, vBegin_, vEnd_, vTotal_, interlaced_};
  for (int column = 0; column < 5; ++column) {
    timings->addWidget(new QLabel(topLabels[column]), 0, column);
    timings->addWidget(topFields[column], 1, column);
    timings->addWidget(new QLabel(bottomLabels[column]), 2, column);
    timings->addWidget(bottomFields[column], 3, column);
  }
  timingsBox_->setParent(central);
  timingsBox_->hide();

  auto* sourceBox = new QGroupBox("Capture Source");
  auto* sourceLayout = new QHBoxLayout(sourceBox);
  auto* sourceControls = new QWidget;
  auto* sourceGrid = new QGridLayout(sourceControls);
  captureMode_ = named(new QComboBox, "captureMode");
  captureMode_->addItems({"Entire monitor", "Single window"});
  monitor_ = named(new QComboBox, "monitor");
  chooseWindowButton_ =
      named(new QPushButton("Choose Window…"), "chooseWindowButton");
  windowSelection_ =
      named(new QLabel("No window selected"), "windowSelection");
  windowSelection_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  audioSink_ = named(new QComboBox, "audioSink");
  crop_ = named(new QComboBox, "crop");
  alignment_ = named(new QComboBox, "alignment");
  rotation_ = named(new QComboBox, "rotation");
  sampling_ = named(new QComboBox, "sampling");
  crop_->addItems({"Custom Size", "1X Crop", "2X Crop", "3X Crop", "4X Crop",
                   "5X Crop", "Full 4:3 Crop", "Full 5:4 Crop"});
  alignment_->addItems({"Centered", "Top Left", "Top", "Top Right", "Right",
                        "Bottom Right", "Bottom", "Bottom Left", "Left"});
  rotation_->addItems({"No Rotate", "90° CW", "90° CCW", "180°"});
  sampling_->addItems({"Point", "Bilinear", "Line Blend"});
  sampling_->setToolTip(
      "Point is pixel-exact. Bilinear smooths adjacent pixels. Line Blend "
      "reduces vertical CRT shimmer with an area-weighted line filter.");
  width_ = named(timingSpin(), "width");
  height_ = named(timingSpin(), "height");
  xOffset_ = named(new QSpinBox, "xOffset");
  yOffset_ = named(new QSpinBox, "yOffset");
  xOffset_->setRange(-8192, 8192);
  yOffset_->setRange(-8192, 8192);
  frameDelay_ = named(new QSpinBox, "frameDelay");
  frameDelay_->setRange(0, 10);
  frameDelay_->setSpecialValueText("Automatic");
  progressiveInterlaceBuffer_ = named(
      new QCheckBox("Stable interlace (progressive framebuffer)"),
      "progressiveInterlaceBuffer");
  progressiveInterlaceBuffer_->setToolTip(
      "Send every interlaced update as one full-height framebuffer. This "
      "prevents field-buffer index changes, but doubles video work and "
      "payload and may add latency.");
  audio_ = named(new QCheckBox("Enable Audio"), "audio");
  preview_ = named(new QCheckBox("Enable Preview"), "preview");
  monitorLabel_ = named(new QLabel("Monitor"), "monitorLabel");
  sourceGrid->addWidget(new QLabel("Source"), 0, 0);
  sourceGrid->addWidget(captureMode_, 0, 1, 1, 2);
  // The monitor chooser and the window chooser occupy the same row; the
  // capture mode decides which stack page shows. Stacking one widget per grid
  // cell keeps the row spacing regular, which two widgets overlapping in one
  // cell did not.
  chooserLabelStack_ = new QStackedWidget;
  chooserLabelStack_->addWidget(monitorLabel_);
  chooserLabelStack_->addWidget(chooseWindowButton_);
  chooserFieldStack_ = new QStackedWidget;
  chooserFieldStack_->addWidget(monitor_);
  chooserFieldStack_->addWidget(windowSelection_);
  sourceGrid->addWidget(chooserLabelStack_, 1, 0);
  sourceGrid->addWidget(chooserFieldStack_, 1, 1, 1, 2);
  sourceGrid->addWidget(new QLabel("Audio output"), 2, 0);
  sourceGrid->addWidget(audioSink_, 2, 1, 1, 2);
  // Crop sits directly above the size and offset it controls.
  sourceGrid->addWidget(new QLabel("Crop"), 3, 0);
  sourceGrid->addWidget(crop_, 3, 1, 1, 2);
  sourceGrid->addWidget(new QLabel("Size"), 4, 0);
  sourceGrid->addWidget(width_, 4, 1);
  sourceGrid->addWidget(height_, 4, 2);
  sourceGrid->addWidget(new QLabel("Offset"), 5, 0);
  sourceGrid->addWidget(xOffset_, 5, 1);
  sourceGrid->addWidget(yOffset_, 5, 2);
  sourceGrid->addWidget(new QLabel("Alignment"), 6, 0);
  sourceGrid->addWidget(alignment_, 6, 1, 1, 2);
  sourceGrid->addWidget(new QLabel("Rotation"), 7, 0);
  sourceGrid->addWidget(rotation_, 7, 1, 1, 2);
  sourceGrid->addWidget(new QLabel("Sampling"), 8, 0);
  sourceGrid->addWidget(sampling_, 8, 1, 1, 2);
  sourceGrid->addWidget(new QLabel("Frame delay"), 9, 0);
  sourceGrid->addWidget(frameDelay_, 9, 1, 1, 2);
  sourceGrid->addWidget(progressiveInterlaceBuffer_, 10, 0, 1, 3);
  sourceGrid->addWidget(audio_, 11, 0, 1, 3);
  sourceGrid->addWidget(preview_, 12, 0, 1, 3);
  sourceGrid->setColumnStretch(1, 1);
  sourceGrid->setColumnStretch(2, 1);
  sourceLayout->addWidget(sourceControls, 1);
  previewImage_ = named(new QLabel("Preview Disabled"), "previewImage");
  previewImage_->setAlignment(Qt::AlignCenter);
  // Small enough that narrowing the window squeezes the preview, not the
  // capture fields, whose minimum width is pinned above.
  previewImage_->setMinimumSize(320, 180);
  previewImage_->setSizePolicy(QSizePolicy::Expanding,
                               QSizePolicy::Expanding);
  previewImage_->setStyleSheet(
      "QLabel { background: #777; color: black; border: 1px solid #999; }");
  sourceLayout->addWidget(previewImage_, 3);
  root->addWidget(sourceBox, 4);

  auto* logsBox = new QGroupBox("Logs");
  auto* logsLayout = new QVBoxLayout(logsBox);
  status_ = named(new QLabel("Idle"), "status");
  status_->setStyleSheet("font-weight: bold");
  videoStatus_ = named(new QLabel, "videoStatus");
  transportStatus_ = named(new QLabel, "transportStatus");
  audioStatus_ = named(new QLabel, "audioStatus");
  for (auto* diagnostic : {videoStatus_, transportStatus_, audioStatus_}) {
    diagnostic->setWordWrap(true);
    diagnostic->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    diagnostic->setTextInteractionFlags(Qt::TextSelectableByMouse);
  }
  log_ = named(new QPlainTextEdit, "log");
  log_->setReadOnly(true);
  log_->document()->setMaximumBlockCount(300);
  logsLayout->addWidget(status_);
  logsLayout->addWidget(videoStatus_);
  logsLayout->addWidget(transportStatus_);
  logsLayout->addWidget(audioStatus_);
  logsLayout->addWidget(log_);
  root->addWidget(logsBox, 2);
  setCentralWidget(central);
  resize(840, 680);

  std::string monitorError;
  for (auto& monitor : x11Monitors(monitorError))
    monitor_->addItem(QString::fromStdString(monitor.name));
  if (!monitorError.empty()) append(monitorError);
  audioSink_->addItem("Default output (PC audio remains on)", QString());
  audioSink_->addItem("MiSTerCast silent output (CRT only)",
                      QString::fromLatin1(SilentAudioSink));
  audioSink_->setToolTip(
      "Silent output temporarily routes active and new playback into a "
      "virtual sink. Original routing is restored when streaming stops.");
  // A crashed or killed instance leaves its silent-output sink loaded in the
  // sound server; remove those leftovers and never list one as a choice.
  std::string staleError;
  if (const auto removed = cleanupStaleSilentSinks(staleError))
    append(QString("Removed %1 stale MiSTerCast silent output%2 left by a "
                   "previous run.")
               .arg(removed)
               .arg(removed == 1 ? "" : "s"));
  std::string audioError;
  for (const auto& sink : pulseAudioSinks(audioError)) {
    if (sink.name.rfind(SilentSinkPrefix, 0) == 0) continue;
    const auto label = QString::fromStdString(sink.description) +
                       (sink.isDefault ? " (default)" : "");
    audioSink_->addItem(label, QString::fromStdString(sink.name));
  }
  if (!audioError.empty()) append("Audio outputs: " + audioError);
  refreshPresetChoices();
  audio_->setChecked(config_.source.audio);
  preview_->setChecked(config_.source.preview);
  controlsFromConfig();
  // The baseline for the save prompt on close is the configuration as loaded,
  // normalized through the controls so a load-then-close never asks.
  configFromControls();
  savedConfig_ = config_;

  // The fields never compress below their preferred width: when the window
  // narrows, the preview shrinks instead of the controls collapsing. Pinned
  // only now, after the monitor and audio combos have their real content, and
  // capped so one unusually long sink description cannot force a huge window.
  sourceControls->setMinimumWidth(
      std::min(sourceControls->sizeHint().width(), 460));

  // Timings stay editable while streaming: they are switched live, as the
  // Windows GUI did, which locked only the capture source and audio.
  streamLockedControls_ = {loadButton_, target_, captureMode_, monitor_,
                           chooseWindowButton_, audioSink_,
                           crop_,       alignment_, rotation_, sampling_,
                           width_,
                           height_,     xOffset_,   yOffset_,  frameDelay_,
                           audio_,      preview_};

  session_.setPreviewCallback([this](const Frame& frame) {
    QImage source(frame.bgra.data(), int(frame.width), int(frame.height),
                  int(frame.stride), QImage::Format_RGB32);
    const auto preview =
        source.scaled(640, 360, Qt::KeepAspectRatio, Qt::FastTransformation)
            .copy();
    QTimer::singleShot(0, this, [this, preview] {
      if (preview_->isChecked())
        previewImage_->setPixmap(QPixmap::fromImage(preview).scaled(
            previewImage_->size(), Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    });
  });

  connect(streamButton_, &QPushButton::clicked, this,
          [this] { toggleStream(); });
  connect(saveButton_, &QPushButton::clicked, this,
          [this] { saveSettings(); });
  connect(loadButton_, &QPushButton::clicked, this,
          [this] { loadSettings(); });
  // Choosing a preset applies it directly; while streaming, the resulting
  // field edits are debounced into a live modeline switch like manual edits.
  connect(preset_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (index >= 0 && size_t(index) < presets_.size())
      setModelineControls(presets_.at(size_t(index)));
  });
  connect(managePresetsButton_, &QPushButton::clicked, this,
          [this] { managePresets(); });
  connect(crop_, &QComboBox::currentIndexChanged, this,
          [this] { refreshConfigurationEnabled(session_.state()); });
  connect(help, &QPushButton::clicked, this, [this] {
    QMessageBox::information(
        this, "MiSTerCast",
        "Linux/X11 MiSTerCast\n\nEnter the MiSTer address, select a modeline "
        "and monitor, then press Start Stream.\nGroovy_MiSTer uses UDP port "
        "32100.");
  });
  connect(target_, &QLineEdit::textChanged, this,
          [this] { refreshStartEnabled(); });
  connect(captureMode_, &QComboBox::currentIndexChanged, this, [this](int mode) {
    if (mode == 0) {
      clearWindowSelection();
    }
    refreshConfigurationEnabled(session_.state());
    refreshStartEnabled();
  });
  connect(chooseWindowButton_, &QPushButton::clicked, this,
          [this] { chooseWindow(); });
  connect(preview_, &QCheckBox::toggled, this, [this](bool enabled) {
    if (!enabled) {
      previewImage_->setPixmap({});
      previewImage_->setText("Preview Disabled");
    } else
      previewImage_->setText("Waiting for preview…");
  });
  connect(audio_, &QCheckBox::toggled, audioSink_, &QComboBox::setEnabled);
  audioSink_->setEnabled(audio_->isChecked());
  for (auto* spin :
       {hActive_, hBegin_, hEnd_, hTotal_, vActive_, vBegin_, vEnd_, vTotal_})
    connect(spin, &QSpinBox::valueChanged, this, [this] {
      refreshStartEnabled();
      scheduleModelineApply();
    });
  connect(pixelClock_, &QDoubleSpinBox::valueChanged, this, [this] {
    refreshStartEnabled();
    scheduleModelineApply();
  });
  connect(interlaced_, &QCheckBox::toggled, this, [this](bool interlacedMode) {
    progressiveInterlaceBuffer_->setEnabled(interlacedMode);
    refreshStartEnabled();
    scheduleModelineApply();
  });
  connect(progressiveInterlaceBuffer_, &QCheckBox::toggled, this,
          [this] { scheduleModelineApply(); });
  modelineApplyTimer_.setSingleShot(true);
  modelineApplyTimer_.setInterval(750);
  connect(&modelineApplyTimer_, &QTimer::timeout, this,
          [this] { applyModelineLive(); });
  statsTimer_.setInterval(1000);
  connect(&statsTimer_, &QTimer::timeout, this, [this] { updateStats(); });
  statsTimer_.start();
  append(QStringLiteral("MiSTerCast ready."));
  if (!compressionAvailable())
    append(QStringLiteral(
        "Warning: built without liblz4; frames are sent uncompressed at "
        "roughly 3-5x the bandwidth."));
  showState(SessionState::Idle);
}

MainWindow::~MainWindow() { session_.stop(); }

void MainWindow::closeEvent(QCloseEvent* event) {
  // Settings are only written when the user asks: here, or via Save Settings.
  // A SIGINT/SIGTERM close must terminate promptly, so it never prompts.
  configFromControls();
  if (!interrupted &&
      serializeGroovyConfig(config_) != serializeGroovyConfig(savedConfig_)) {
    QMessageBox prompt(QMessageBox::Question, "MiSTerCast",
                       "Save the changed settings before closing?",
                       QMessageBox::Save | QMessageBox::Discard |
                           QMessageBox::Cancel,
                       this);
    prompt.setObjectName(QStringLiteral("closePrompt"));
    prompt.setDefaultButton(QMessageBox::Save);
    const auto choice = prompt.exec();
    if (choice == QMessageBox::Cancel) {
      event->ignore();
      return;
    }
    if (choice == QMessageBox::Save) {
      std::string error;
      if (!saveGroovyConfig(config_, configPath(), error)) {
        QMessageBox::warning(
            this, "MiSTerCast",
            QString::fromStdString("Settings could not be saved: " + error));
        event->ignore();
        return;
      }
    }
  }
  session_.stop();
  statsTimer_.stop();
  modelineApplyTimer_.stop();
  QMainWindow::closeEvent(event);
}

}  // namespace mistercast::gui

int launchGui(int argc, char** argv) {
  QApplication app(argc, argv);
  interrupted = 0;
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  mistercast::gui::MainWindow window;
  QTimer signalTimer;
  signalTimer.setInterval(50);
  QObject::connect(&signalTimer, &QTimer::timeout, &window, [&window] {
    if (interrupted) window.close();
  });
  signalTimer.start();
  window.show();
  return app.exec();
}
