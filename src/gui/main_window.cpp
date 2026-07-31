#include "mistercast/config.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/stream_session.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using namespace mistercast;

namespace {
QString stateName(SessionState state) {
  switch (state) {
    case SessionState::Idle: return "Idle";
    case SessionState::Starting: return "Starting";
    case SessionState::Streaming: return "Streaming";
    case SessionState::Stopping: return "Stopping";
    case SessionState::Error: return "Error";
  }
  return "Unknown";
}
}

class MainWindow final : public QMainWindow {
  AppConfig config_{loadConfig(configPath())};
  StreamSession session_;
  std::vector<Modeline> presets_;

  QPushButton *streamButton_{}, *saveButton_{}, *loadButton_{}, *applyModelineButton_{};
  QLineEdit *target_{};
  QComboBox *monitor_{}, *preset_{}, *crop_{}, *alignment_{}, *rotation_{};
  QDoubleSpinBox *pixelClock_{};
  QSpinBox *hActive_{}, *hBegin_{}, *hEnd_{}, *hTotal_{};
  QSpinBox *vActive_{}, *vBegin_{}, *vEnd_{}, *vTotal_{};
  QSpinBox *width_{}, *height_{}, *xOffset_{}, *yOffset_{}, *frameDelay_{};
  QCheckBox *interlaced_{}, *audio_{}, *preview_{};
  QLabel *previewImage_{}, *status_{};
  QPlainTextEdit *log_{};
  QTimer statsTimer_;
  uint64_t previousDropped_{}, previousAudioDropped_{}, previousUnderrun_{};

  void append(const QString& message) {
    log_->appendPlainText(message);
  }

  void append(const std::string& message) {
    append(QString::fromStdString(message));
  }

  void refreshStartEnabled() {
    const auto state = session_.state();
    const bool busy = state == SessionState::Starting || state == SessionState::Stopping;
    const bool valid = !target_->text().trimmed().isEmpty() && modelineFromControls().validate() == std::nullopt;
    streamButton_->setEnabled(!busy && (state == SessionState::Streaming || valid));
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
    hActive_->setValue(modeline.hActive); hBegin_->setValue(modeline.hBegin);
    hEnd_->setValue(modeline.hEnd); hTotal_->setValue(modeline.hTotal);
    vActive_->setValue(modeline.vActive); vBegin_->setValue(modeline.vBegin);
    vEnd_->setValue(modeline.vEnd); vTotal_->setValue(modeline.vTotal);
    interlaced_->setChecked(modeline.interlaced);
    refreshStartEnabled();
  }

  void configFromControls() {
    config_.target = target_->text().trimmed().toStdString();
    config_.source.monitor = monitor_->currentText().toStdString();
    config_.source.audio = audio_->isChecked();
    config_.source.preview = preview_->isChecked();
    config_.source.width = uint16_t(width_->value());
    config_.source.height = uint16_t(height_->value());
    config_.source.xOffset = int16_t(xOffset_->value());
    config_.source.yOffset = int16_t(yOffset_->value());
    config_.source.frameDelay = uint16_t(frameDelay_->value());
    config_.source.crop = CropMode(crop_->currentIndex());
    config_.source.alignment = Alignment(alignment_->currentIndex());
    config_.source.rotation = Rotation(rotation_->currentIndex());
    config_.modeline = modelineFromControls();
  }

  void controlsFromConfig() {
    target_->setText(QString::fromStdString(config_.target));
    auto monitorIndex = monitor_->findText(QString::fromStdString(config_.source.monitor));
    if (monitorIndex >= 0) monitor_->setCurrentIndex(monitorIndex);
    crop_->setCurrentIndex(int(config_.source.crop));
    alignment_->setCurrentIndex(int(config_.source.alignment));
    rotation_->setCurrentIndex(int(config_.source.rotation));
    width_->setValue(config_.source.width); height_->setValue(config_.source.height);
    xOffset_->setValue(config_.source.xOffset); yOffset_->setValue(config_.source.yOffset);
    frameDelay_->setValue(config_.source.frameDelay);
    audio_->setChecked(config_.source.audio); preview_->setChecked(config_.source.preview);
    auto presetIndex = preset_->findText(QString::fromStdString(config_.modeline.name));
    if (presetIndex >= 0) preset_->setCurrentIndex(presetIndex);
    setModelineControls(config_.modeline);
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
    const bool started = session_.start(config_, [this](SessionState state, const std::optional<SessionError>& problem) {
      QTimer::singleShot(0, this, [this, state, problem] {
        showState(state);
        if (problem) {
          append(QString::fromStdString(problem->component + ": " + problem->message));
          if (!problem->hint.empty()) append(QString::fromStdString("Hint: " + problem->hint));
        }
      });
    }, &error);
    if (!started) {
      showState(SessionState::Error);
      if (!error.empty()) append("Start failed: " + error);
      return;
    }
    showState(SessionState::Streaming);
    saveSettings(false);
    previousDropped_ = previousAudioDropped_ = previousUnderrun_ = 0;
    append("Streaming to " + config_.target + ".");
  }

  void updateStats() {
    if (session_.state() != SessionState::Streaming) return;
    const auto stats = session_.stats();
    const double audioMs = config_.source.audio && session_.state() == SessionState::Streaming
      ? stats.audioBufferedSamples * 500.0 / 48000.0 : 0.0;
    status_->setText(QString("Streaming  |  %1 fps  |  capture %2 fps  |  dropped %3  |  sync %4/%5 (%6 us)  |  VRAM %7  |  audio %8 ms / %9%  |  MiSTer audio %10")
      .arg(stats.streamFps, 0, 'f', 1).arg(stats.captureFps, 0, 'f', 1)
      .arg(stats.droppedFrames).arg(stats.syncLine).arg(stats.fpgaVCount).arg(stats.rasterCorrectionUs)
      .arg(stats.vramSynced ? (stats.vgaFrameskip ? "fallback" : "synced") : "unsynced")
      .arg(audioMs, 0, 'f', 0).arg(stats.audioPeak*100, 0, 'f', 0).arg(stats.misterAudioEnabled ? "on" : "off"));
    if (stats.droppedFrames > previousDropped_ + 30) {
      append(QString("Performance: %1 video frames dropped.").arg(stats.droppedFrames));
      previousDropped_ = stats.droppedFrames;
    }
    if (stats.audioDroppedSamples > previousAudioDropped_) {
      append(QString("Performance: audio overrun dropped %1 samples.").arg(stats.audioDroppedSamples));
      previousAudioDropped_ = stats.audioDroppedSamples;
    }
    if (stats.audioUnderrunSamples > previousUnderrun_ + 4800) {
      append(QString("Performance: audio underrun inserted %1 silent samples.").arg(stats.audioUnderrunSamples));
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
    auto* help = new QPushButton("?"); help->setFixedWidth(34);
    controls->addWidget(streamButton_, 2); controls->addWidget(saveButton_, 2); controls->addWidget(loadButton_, 2);
    controls->addStretch(); controls->addWidget(new QLabel("Target IP")); controls->addWidget(target_, 2); controls->addWidget(help);
    root->addWidget(controlsBox);

    auto* modelineRow = new QHBoxLayout;
    auto* presetBox = new QGroupBox("Modeline Presets");
    auto* presetLayout = new QVBoxLayout(presetBox);
    preset_ = new QComboBox; applyModelineButton_ = new QPushButton("Apply Modeline");
    presetLayout->addWidget(preset_); presetLayout->addWidget(applyModelineButton_); presetLayout->addStretch();
    modelineRow->addWidget(presetBox, 1);

    auto* timingsBox = new QGroupBox("Modeline");
    auto* timings = new QGridLayout(timingsBox);
    pixelClock_ = new QDoubleSpinBox; pixelClock_->setRange(0.1, 400); pixelClock_->setDecimals(3);
    hActive_ = timingSpin(); hBegin_ = timingSpin(); hEnd_ = timingSpin(); hTotal_ = timingSpin();
    vActive_ = timingSpin(); vBegin_ = timingSpin(); vEnd_ = timingSpin(); vTotal_ = timingSpin(); interlaced_ = new QCheckBox;
    const char* topLabels[] = {"Pclock", "Hactive", "Hbegin", "Hend", "Htotal"};
    QWidget* topFields[] = {pixelClock_, hActive_, hBegin_, hEnd_, hTotal_};
    const char* bottomLabels[] = {"Vactive", "Vbegin", "Vend", "Vtotal", "Interlace"};
    QWidget* bottomFields[] = {vActive_, vBegin_, vEnd_, vTotal_, interlaced_};
    for (int column = 0; column < 5; ++column) {
      timings->addWidget(new QLabel(topLabels[column]), 0, column); timings->addWidget(topFields[column], 1, column);
      timings->addWidget(new QLabel(bottomLabels[column]), 2, column); timings->addWidget(bottomFields[column], 3, column);
    }
    modelineRow->addWidget(timingsBox, 4);
    root->addLayout(modelineRow);

    auto* sourceBox = new QGroupBox("Capture Source");
    auto* sourceLayout = new QHBoxLayout(sourceBox);
    auto* sourceControls = new QWidget;
    auto* sourceGrid = new QGridLayout(sourceControls);
    monitor_ = new QComboBox; crop_ = new QComboBox; alignment_ = new QComboBox; rotation_ = new QComboBox;
    crop_->addItems({"Custom Size", "1X Crop", "2X Crop", "3X Crop", "4X Crop", "5X Crop", "Full 4:3 Crop", "Full 5:4 Crop"});
    alignment_->addItems({"Centered", "Top Left", "Top", "Top Right", "Right", "Bottom Right", "Bottom", "Bottom Left", "Left"});
    rotation_->addItems({"No Rotate", "90° CW", "90° CCW", "180°"});
    width_ = timingSpin(); height_ = timingSpin();
    xOffset_ = new QSpinBox; yOffset_ = new QSpinBox; xOffset_->setRange(-8192, 8192); yOffset_->setRange(-8192, 8192);
    frameDelay_ = new QSpinBox; frameDelay_->setRange(0, 10); frameDelay_->setSpecialValueText("Automatic");
    audio_ = new QCheckBox("Enable Audio"); preview_ = new QCheckBox("Enable Preview");
    sourceGrid->addWidget(new QLabel("Monitor"), 0, 0); sourceGrid->addWidget(monitor_, 0, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Crop"), 1, 0); sourceGrid->addWidget(crop_, 1, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Alignment"), 2, 0); sourceGrid->addWidget(alignment_, 2, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Rotation"), 3, 0); sourceGrid->addWidget(rotation_, 3, 1, 1, 2);
    sourceGrid->addWidget(new QLabel("Size"), 4, 0); sourceGrid->addWidget(width_, 4, 1); sourceGrid->addWidget(height_, 4, 2);
    sourceGrid->addWidget(new QLabel("Offset"), 5, 0); sourceGrid->addWidget(xOffset_, 5, 1); sourceGrid->addWidget(yOffset_, 5, 2);
    sourceGrid->addWidget(new QLabel("Frame delay"), 6, 0); sourceGrid->addWidget(frameDelay_, 6, 1, 1, 2);
    sourceGrid->addWidget(audio_, 7, 0, 1, 3); sourceGrid->addWidget(preview_, 8, 0, 1, 3); sourceGrid->setColumnStretch(1, 1); sourceGrid->setColumnStretch(2, 1);
    sourceLayout->addWidget(sourceControls, 1);
    previewImage_ = new QLabel("Preview Disabled");
    previewImage_->setAlignment(Qt::AlignCenter); previewImage_->setMinimumSize(450, 260);
    previewImage_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewImage_->setStyleSheet("QLabel { background: #777; color: black; border: 1px solid #999; }");
    sourceLayout->addWidget(previewImage_, 3);
    root->addWidget(sourceBox, 4);

    auto* logsBox = new QGroupBox("Logs");
    auto* logsLayout = new QVBoxLayout(logsBox);
    status_ = new QLabel("Idle"); log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->document()->setMaximumBlockCount(300);
    logsLayout->addWidget(status_); logsLayout->addWidget(log_); root->addWidget(logsBox, 2);
    setCentralWidget(central); resize(840, 790);

    std::string monitorError; auto capture = makeX11Capture();
    for (auto& monitor : capture->monitors(monitorError)) monitor_->addItem(QString::fromStdString(monitor.name));
    if (!monitorError.empty()) append(monitorError);
    presets_ = bundledModelines(); presets_.insert(presets_.end(), config_.customModelines.begin(), config_.customModelines.end());
    for (const auto& preset : presets_) preset_->addItem(QString::fromStdString(preset.name));
    audio_->setChecked(config_.source.audio); preview_->setChecked(config_.source.preview); controlsFromConfig();

    session_.setPreviewCallback([this](const Frame& frame) {
      QImage source(frame.bgra.data(), int(frame.width), int(frame.height), int(frame.stride), QImage::Format_RGB32);
      const auto preview = source.scaled(640, 360, Qt::KeepAspectRatio, Qt::FastTransformation).copy();
      QTimer::singleShot(0, this, [this, preview] {
        if (preview_->isChecked()) previewImage_->setPixmap(QPixmap::fromImage(preview).scaled(previewImage_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
      });
    });

    connect(streamButton_, &QPushButton::clicked, this, [this] { toggleStream(); });
    connect(saveButton_, &QPushButton::clicked, this, [this] { saveSettings(); });
    connect(loadButton_, &QPushButton::clicked, this, [this] { loadSettings(); });
    connect(applyModelineButton_, &QPushButton::clicked, this, [this] { if (preset_->currentIndex() >= 0) setModelineControls(presets_.at(size_t(preset_->currentIndex()))); });
    connect(help, &QPushButton::clicked, this, [this] { QMessageBox::information(this, "MiSTerCast", "Linux/X11 MiSTerCast\n\nEnter the MiSTer address, select a modeline and monitor, then press Start Stream.\nGroovy_MiSTer uses UDP port 32100."); });
    connect(target_, &QLineEdit::textChanged, this, [this] { refreshStartEnabled(); });
    connect(preview_, &QCheckBox::toggled, this, [this](bool enabled) { if (!enabled) { previewImage_->setPixmap({}); previewImage_->setText("Preview Disabled"); } else previewImage_->setText("Waiting for preview…"); });
    for (auto* spin : {hActive_, hBegin_, hEnd_, hTotal_, vActive_, vBegin_, vEnd_, vTotal_}) connect(spin, &QSpinBox::valueChanged, this, [this] { refreshStartEnabled(); });
    connect(pixelClock_, &QDoubleSpinBox::valueChanged, this, [this] { refreshStartEnabled(); });
    statsTimer_.setInterval(1000); connect(&statsTimer_, &QTimer::timeout, this, [this] { updateStats(); }); statsTimer_.start();
    append(QStringLiteral("MiSTerCast ready.")); showState(SessionState::Idle);
  }

  ~MainWindow() override { session_.stop(); }
};

int launchGui(int argc, char** argv) {
  QApplication app(argc, argv);
  MainWindow window;
  window.show();
  return app.exec();
}
