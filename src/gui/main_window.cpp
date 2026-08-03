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
#include <QMainWindow>
#include <QMessageBox>
#include <QListWidget>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/stream_session.hpp"

using namespace mistercast;

namespace {
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
}  // namespace

class MainWindow final : public QMainWindow {
  AppConfig config_{loadConfig(configPath())};
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

  void append(const QString& message) { log_->appendPlainText(message); }

  void append(const std::string& message) {
    append(QString::fromStdString(message));
  }

  void refreshStartEnabled() {
    const auto state = session_.state();
    const bool busy =
        state == SessionState::Starting || state == SessionState::Stopping;
    const bool valid = !target_->text().trimmed().isEmpty() &&
                       modelineFromControls().validate() == std::nullopt &&
                       (captureMode_->currentIndex() == 0 || selectedWindow_);
    streamButton_->setEnabled(!busy &&
                              (state == SessionState::Streaming || valid));
  }

  void refreshConfigurationEnabled(SessionState state) {
    const bool enabled =
        state == SessionState::Idle || state == SessionState::Error;
    for (auto* control : streamLockedControls_) control->setEnabled(enabled);
    const bool windowMode = captureMode_->currentIndex() == 1;
    monitor_->setEnabled(enabled && !windowMode);
    chooseWindowButton_->setEnabled(enabled && windowMode);
    if (enabled) audioSink_->setEnabled(audio_->isChecked());
    // Interlace buffering tracks the interlaced flag whether or not a stream is
    // running, because timings are now switched live.
    progressiveInterlaceBuffer_->setEnabled(interlaced_->isChecked());
  }

  void chooseWindow() {
    std::string error;
    auto capture = makeX11Capture();
    const auto windows = capture->windows(error);
    if (!error.empty()) {
      append("Windows: " + error);
      return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Choose a window to share");
    dialog.resize(560, 420);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(
        "Select one visible X11 window. Minimized windows are unavailable."));
    auto* list = new QListWidget;
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
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
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

  // Editing timings fires a change per field, so the live switch is debounced
  // until the user stops typing rather than sending a CMD_SWITCHRES per digit.
  void scheduleModelineApply() {
    if (session_.state() == SessionState::Streaming)
      modelineApplyTimer_.start();
  }

  void applyModelineLive() {
    if (session_.state() != SessionState::Streaming) return;
    const auto modeline = modelineFromControls();
    if (modeline.validate()) return;
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

  void showState(SessionState state) {
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

  Modeline modelineFromControls() const {
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

  void setModelineControls(const Modeline& modeline) {
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

  void configFromControls() {
    config_.target = target_->text().trimmed().toStdString();
    config_.source.monitor = monitor_->currentText().toStdString();
    config_.source.captureMode = captureMode_->currentIndex() == 1
                                     ? CaptureMode::Window
                                     : CaptureMode::Monitor;
    config_.source.window = selectedWindow_;
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

  void controlsFromConfig() {
    target_->setText(QString::fromStdString(config_.target));
    auto monitorIndex =
        monitor_->findText(QString::fromStdString(config_.source.monitor));
    if (monitorIndex >= 0) monitor_->setCurrentIndex(monitorIndex);
    captureMode_->setCurrentIndex(config_.source.captureMode ==
                                          CaptureMode::Window
                                      ? 1
                                      : 0);
    selectedWindow_ = config_.source.window;
    windowSelection_->setText(selectedWindow_
                                  ? QString::fromStdString(selectedWindow_->title)
                                  : QStringLiteral("No window selected"));
    windowSelection_->setToolTip({});
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

  void saveSettings(bool announce = true) {
    configFromControls();
    std::string error;
    if (!saveConfig(config_, configPath(), error)) {
      append("Settings: " + error);
      return;
    }
    if (announce) append("Settings saved to " + configPath().string());
  }

  void loadSettings() {
    std::string warning;
    config_ = loadConfig(configPath(), &warning);
    controlsFromConfig();
    append(warning.empty() ? "Settings loaded." : warning);
  }

  void toggleStream() {
    if (session_.state() == SessionState::Streaming) {
      showState(SessionState::Stopping);
      session_.stop();
      showState(SessionState::Idle);
      append(QStringLiteral("Stream stopped."));
      return;
    }

    configFromControls();
    if (auto validation = config_.validate()) {
      append("Configuration: " + *validation);
      return;
    }

    showState(SessionState::Starting);
    std::string error;
    const bool started = session_.start(
        config_,
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
    saveSettings(false);
    previousDropped_ = previousAudioDropped_ = previousUnderrun_ = 0;
    previousSendErrors_ = previousFieldRealignments_ = 0;
    append("Streaming to " + config_.target + ".");
  }

  void updateStats() {
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
    QString fieldStatus;
    if (stats.transport.interlacedFieldBuffer)
      fieldStatus = QString(" · field %1/FPGA %2 %3")
                        .arg(stats.transport.outgoingField)
                        .arg(stats.transport.fpgaField)
                        .arg(stats.transport.fieldPhaseValid ? "locked"
                                                             : "acquiring");
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
                "Paced payloads/datagrams: %7/%8\nMaximum batch lateness: %9 µs")
            .arg(stats.transport.rasterCorrectionUs)
            .arg(stats.transport.compressionTimeUs)
            .arg(stats.transport.submissionTimeUs)
            .arg(stats.transport.estimatedWireTimeUs)
            .arg(stats.transport.observedUdpQueueHighWater)
            .arg(stats.transport.socketSendBufferBytes)
            .arg(stats.transport.pacedVideoPayloads)
            .arg(stats.transport.pacedDatagrams)
            .arg(stats.transport.maxBatchReleaseLatenessNs / 1000));
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

  static QSpinBox* timingSpin() {
    auto* spin = new QSpinBox;
    spin->setRange(1, 8192);
    return spin;
  }

 public:
  MainWindow() {
    setWindowTitle("MiSTerCast 1.0 — Linux/X11");
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(6);

    auto* controlsBox = new QGroupBox("Controls");
    auto* controls = new QHBoxLayout(controlsBox);
    streamButton_ = new QPushButton("Start Stream");
    saveButton_ = new QPushButton("Save Settings");
    loadButton_ = new QPushButton("Load Settings");
    target_ = new QLineEdit(QString::fromStdString(config_.target));
    target_->setPlaceholderText("MiSTer IPv4 or hostname");
    auto* help = new QPushButton("?");
    help->setFixedWidth(34);
    controls->addWidget(streamButton_, 2);
    controls->addWidget(saveButton_, 2);
    controls->addWidget(loadButton_, 2);
    controls->addStretch();
    controls->addWidget(new QLabel("Target IP"));
    controls->addWidget(target_, 2);
    controls->addWidget(help);
    root->addWidget(controlsBox);

    auto* modelineRow = new QHBoxLayout;
    auto* presetBox = new QGroupBox("Modeline Presets");
    auto* presetLayout = new QVBoxLayout(presetBox);
    preset_ = new QComboBox;
    applyModelineButton_ = new QPushButton("Apply Modeline");
    presetLayout->addWidget(preset_);
    presetLayout->addWidget(applyModelineButton_);
    presetLayout->addStretch();
    modelineRow->addWidget(presetBox, 1);

    auto* timingsBox = new QGroupBox("Modeline");
    auto* timings = new QGridLayout(timingsBox);
    pixelClock_ = new QDoubleSpinBox;
    pixelClock_->setRange(0.1, 400);
    pixelClock_->setDecimals(3);
    hActive_ = timingSpin();
    hBegin_ = timingSpin();
    hEnd_ = timingSpin();
    hTotal_ = timingSpin();
    vActive_ = timingSpin();
    vBegin_ = timingSpin();
    vEnd_ = timingSpin();
    vTotal_ = timingSpin();
    interlaced_ = new QCheckBox;
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
    modelineRow->addWidget(timingsBox, 4);
    root->addLayout(modelineRow);

    auto* sourceBox = new QGroupBox("Capture Source");
    auto* sourceLayout = new QHBoxLayout(sourceBox);
    auto* sourceControls = new QWidget;
    auto* sourceGrid = new QGridLayout(sourceControls);
    captureMode_ = new QComboBox;
    captureMode_->addItems({"Entire monitor", "Single window"});
    monitor_ = new QComboBox;
    chooseWindowButton_ = new QPushButton("Choose Window…");
    windowSelection_ = new QLabel("No window selected");
    windowSelection_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    audioSink_ = new QComboBox;
    crop_ = new QComboBox;
    alignment_ = new QComboBox;
    rotation_ = new QComboBox;
    sampling_ = new QComboBox;
    crop_->addItems({"Custom Size", "1X Crop", "2X Crop", "3X Crop", "4X Crop",
                     "5X Crop", "Full 4:3 Crop", "Full 5:4 Crop"});
    alignment_->addItems({"Centered", "Top Left", "Top", "Top Right", "Right",
                          "Bottom Right", "Bottom", "Bottom Left", "Left"});
    rotation_->addItems({"No Rotate", "90° CW", "90° CCW", "180°"});
    sampling_->addItems({"Point", "Bilinear", "Line Blend"});
    sampling_->setToolTip(
        "Point is pixel-exact. Bilinear smooths adjacent pixels. Line Blend "
        "reduces vertical CRT shimmer with an area-weighted line filter.");
    width_ = timingSpin();
    height_ = timingSpin();
    xOffset_ = new QSpinBox;
    yOffset_ = new QSpinBox;
    xOffset_->setRange(-8192, 8192);
    yOffset_->setRange(-8192, 8192);
    frameDelay_ = new QSpinBox;
    frameDelay_->setRange(0, 10);
    frameDelay_->setSpecialValueText("Automatic");
    progressiveInterlaceBuffer_ =
        new QCheckBox("Stable interlace (progressive framebuffer)");
    progressiveInterlaceBuffer_->setToolTip(
        "Send every interlaced update as one full-height framebuffer. This "
        "prevents field-buffer index changes, but doubles video work and "
        "payload and may add latency.");
    audio_ = new QCheckBox("Enable Audio");
    preview_ = new QCheckBox("Enable Preview");
    sourceGrid->addWidget(new QLabel("Source"), 0, 0);
    sourceGrid->addWidget(captureMode_, 0, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Monitor"), 1, 0);
    sourceGrid->addWidget(monitor_, 1, 1, 1, 2);
    sourceGrid->addWidget(chooseWindowButton_, 2, 0);
    sourceGrid->addWidget(windowSelection_, 2, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Audio output"), 3, 0);
    sourceGrid->addWidget(audioSink_, 3, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Crop"), 4, 0);
    sourceGrid->addWidget(crop_, 4, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Alignment"), 5, 0);
    sourceGrid->addWidget(alignment_, 5, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Rotation"), 6, 0);
    sourceGrid->addWidget(rotation_, 6, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Sampling"), 7, 0);
    sourceGrid->addWidget(sampling_, 7, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Size"), 8, 0);
    sourceGrid->addWidget(width_, 8, 1);
    sourceGrid->addWidget(height_, 8, 2);
    sourceGrid->addWidget(new QLabel("Offset"), 9, 0);
    sourceGrid->addWidget(xOffset_, 9, 1);
    sourceGrid->addWidget(yOffset_, 9, 2);
    sourceGrid->addWidget(new QLabel("Frame delay"), 10, 0);
    sourceGrid->addWidget(frameDelay_, 10, 1, 1, 2);
    sourceGrid->addWidget(progressiveInterlaceBuffer_, 11, 0, 1, 3);
    sourceGrid->addWidget(audio_, 12, 0, 1, 3);
    sourceGrid->addWidget(preview_, 13, 0, 1, 3);
    sourceGrid->setColumnStretch(1, 1);
    sourceGrid->setColumnStretch(2, 1);
    sourceLayout->addWidget(sourceControls, 1);
    previewImage_ = new QLabel("Preview Disabled");
    previewImage_->setAlignment(Qt::AlignCenter);
    previewImage_->setMinimumSize(450, 260);
    previewImage_->setSizePolicy(QSizePolicy::Expanding,
                                 QSizePolicy::Expanding);
    previewImage_->setStyleSheet(
        "QLabel { background: #777; color: black; border: 1px solid #999; }");
    sourceLayout->addWidget(previewImage_, 3);
    root->addWidget(sourceBox, 4);

    auto* logsBox = new QGroupBox("Logs");
    auto* logsLayout = new QVBoxLayout(logsBox);
    status_ = new QLabel("Idle");
    status_->setStyleSheet("font-weight: bold");
    videoStatus_ = new QLabel;
    transportStatus_ = new QLabel;
    audioStatus_ = new QLabel;
    for (auto* diagnostic : {videoStatus_, transportStatus_, audioStatus_}) {
      diagnostic->setWordWrap(true);
      diagnostic->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
      diagnostic->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->document()->setMaximumBlockCount(300);
    logsLayout->addWidget(status_);
    logsLayout->addWidget(videoStatus_);
    logsLayout->addWidget(transportStatus_);
    logsLayout->addWidget(audioStatus_);
    logsLayout->addWidget(log_);
    root->addWidget(logsBox, 2);
    setCentralWidget(central);
    resize(840, 790);

    std::string monitorError;
    auto capture = makeX11Capture();
    for (auto& monitor : capture->monitors(monitorError))
      monitor_->addItem(QString::fromStdString(monitor.name));
    if (!monitorError.empty()) append(monitorError);
    audioSink_->addItem("Default output (PC audio remains on)", QString());
    audioSink_->addItem("MiSTerCast silent output (CRT only)",
                        QString::fromLatin1(SilentAudioSink));
    audioSink_->setToolTip(
        "Silent output temporarily routes active and new playback into a "
        "virtual sink. Original routing is restored when streaming stops.");
    std::string audioError;
    for (const auto& sink : pulseAudioSinks(audioError)) {
      const auto label = QString::fromStdString(sink.description) +
                         (sink.isDefault ? " (default)" : "");
      audioSink_->addItem(label, QString::fromStdString(sink.name));
    }
    if (!audioError.empty()) append("Audio outputs: " + audioError);
    presets_ = bundledModelines();
    presets_.insert(presets_.end(), config_.customModelines.begin(),
                    config_.customModelines.end());
    for (const auto& preset : presets_)
      preset_->addItem(QString::fromStdString(preset.name));
    audio_->setChecked(config_.source.audio);
    preview_->setChecked(config_.source.preview);
    controlsFromConfig();

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
    connect(applyModelineButton_, &QPushButton::clicked, this, [this] {
      if (preset_->currentIndex() >= 0)
        setModelineControls(presets_.at(size_t(preset_->currentIndex())));
    });
    connect(help, &QPushButton::clicked, this, [this] {
      QMessageBox::information(
          this, "MiSTerCast",
          "Linux/X11 MiSTerCast\n\nEnter the MiSTer address, select a modeline "
          "and monitor, then press Start Stream.\nGroovy_MiSTer uses UDP port "
          "32100.");
    });
    connect(target_, &QLineEdit::textChanged, this,
            [this] { refreshStartEnabled(); });
    connect(captureMode_, &QComboBox::currentIndexChanged, this, [this] {
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
    connect(interlaced_, &QCheckBox::toggled, this, [this](bool interlaced) {
      progressiveInterlaceBuffer_->setEnabled(interlaced);
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

  ~MainWindow() override { session_.stop(); }

 protected:
  void closeEvent(QCloseEvent* event) override {
    session_.stop();
    statsTimer_.stop();
    modelineApplyTimer_.stop();
    QMainWindow::closeEvent(event);
  }
};

int launchGui(int argc, char** argv) {
  QApplication app(argc, argv);
  MainWindow window;
  window.show();
  return app.exec();
}
