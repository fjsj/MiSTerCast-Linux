#include "mistercast/pattern.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#include "mistercast/groovy_transport.hpp"

namespace mistercast {
namespace {
constexpr uint32_t kToneRate = 48000;
constexpr uint32_t kToneFrequency = 440;
constexpr int16_t kToneAmplitude = 4096;
constexpr uint64_t kPhaseScale = uint64_t{1} << 32;
constexpr double kTau = 6.283185307179586476925286766559;

bool nextValue(const std::vector<std::string>& arguments, size_t& index,
               const std::string& option, std::string& value,
               std::string& error) {
  if (++index >= arguments.size()) {
    error = "missing value for " + option;
    return false;
  }
  value = arguments[index];
  return true;
}

uint32_t noiseWord(uint32_t frame, uint32_t x, uint32_t y) noexcept {
  uint32_t value = 0x9e3779b9u ^ (frame * 0x85ebca6bu) ^
                   (x * 0xc2b2ae35u) ^ (y * 0x27d4eb2fu);
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  return value ^ (value >> 16);
}

void printStats(std::ostream& output, const GroovyTransportStats& stats) {
  output << "sync " << stats.requestedSyncLine << "/" << stats.fpgaVCount
         << ", frames " << stats.acknowledgedFrame << "/" << stats.fpgaFrame
         << ", ACK " << stats.acknowledgedFrames << "/" << stats.missedAcks
         << ", FPGA " << (stats.vramSynced ? "synced" : "unsynced")
         << (stats.vgaFrameskip ? "/fallback" : "") << ", queue "
         << (stats.vramQueuePresent ? "ready" : "empty") << ", unhealthy "
         << stats.fpgaFallbackSamples << "/" << stats.vramUnsyncedSamples
         << "/" << stats.vramQueueEmptySamples << ", UDP peak "
         << stats.observedUdpQueueHighWater << " B, late "
         << stats.lateBatchReleases << " (max "
         << stats.maxBatchReleaseLatenessNs / 1000 << " us)\n";
}
}  // namespace

bool parsePatternOptions(const std::vector<std::string>& arguments,
                         PatternOptions& options, std::string& error) {
  options = {};
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& option = arguments[i];
    std::string value;
    if (option == "--target") {
      if (!nextValue(arguments, i, option, options.target, error)) return false;
    } else if (option == "--tone") {
      options.tone = true;
    } else if (option == "--modeline") {
      if (!nextValue(arguments, i, option, value, error)) return false;
      if (!parseModeline(value, options.modeline, error)) return false;
    } else if (option == "--content") {
      if (!nextValue(arguments, i, option, value, error)) return false;
      if (value == "bars")
        options.content = PatternContent::Bars;
      else if (value == "noise")
        options.content = PatternContent::Noise;
      else {
        error = "pattern content must be bars or noise";
        return false;
      }
    } else if (option == "--progressive-interlace-buffer") {
      options.progressiveInterlaceBuffer = true;
    } else if (option == "--interlaced-field-buffer") {
      options.progressiveInterlaceBuffer = false;
    } else if (option == "--frame-delay") {
      if (!nextValue(arguments, i, option, value, error)) return false;
      try {
        const int delay = std::stoi(value);
        if (delay < 0 || delay > 10) throw std::out_of_range("delay");
        options.frameDelay = uint16_t(delay);
      } catch (...) {
        error = "frame delay must be 0..10";
        return false;
      }
    } else {
      error = "unknown pattern option: " + option;
      return false;
    }
  }
  if (options.target.empty()) {
    error = "a target is required; use --target HOST";
    return false;
  }
  if (auto validation = options.modeline.validate()) {
    error = *validation;
    return false;
  }
  return true;
}

void generatePattern(const PatternOptions& options, uint32_t frame,
                     uint8_t field, std::vector<uint8_t>& bgr) {
  const auto& mode = options.modeline;
  const bool fieldBuffer =
      mode.interlaced && !options.progressiveInterlaceBuffer;
  const uint32_t outputHeight = mode.vActive / (fieldBuffer ? 2 : 1);
  bgr.resize(size_t(mode.hActive) * outputHeight * 3);
  static constexpr uint8_t bars[8][3] = {
      {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
      {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {0, 0, 0}};
  const uint32_t markerSize =
      std::max<uint32_t>(1, std::min(mode.hActive, mode.vActive) / 8);
  uint8_t* destination = bgr.data();
  for (uint32_t y = 0; y < outputHeight; ++y) {
    const uint32_t fullY = fieldBuffer ? y * 2 + !(field & 1) : y;
    for (uint32_t x = 0; x < mode.hActive; ++x, destination += 3) {
      if (options.content == PatternContent::Noise) {
        const uint32_t random = noiseWord(frame, x, fullY);
        destination[0] = uint8_t(random);
        destination[1] = uint8_t(random >> 8);
        destination[2] = uint8_t(random >> 16);
        continue;
      }
      const auto* color = bars[std::min<uint32_t>(7, x * 8 / mode.hActive)];
      std::memcpy(destination, color, 3);
      if (x < markerSize && fullY < markerSize) {
        const uint8_t flash = (frame & 1) ? 0 : 255;
        destination[0] = destination[1] = destination[2] = flash;
      } else if (x >= mode.hActive - markerSize &&
                 fullY >= mode.vActive - markerSize) {
        destination[0] = (field & 1) ? 0 : 255;
        destination[1] = (field & 1) ? 255 : 0;
        destination[2] = 0;
      }
    }
  }
}

PatternTone::PatternTone(uint32_t sampleRate) noexcept
    : sampleRate_(sampleRate), pacer_(sampleRate) {}

void PatternTone::reset() noexcept {
  pacer_.reset(sampleRate_);
  phase_ = 0;
}

void PatternTone::generate(uint64_t elapsedNs,
                           std::vector<int16_t>& stereo) {
  const size_t values = pacer_.valuesDue(elapsedNs, 32000);
  stereo.resize(values);
  const uint64_t phaseStep = kPhaseScale * kToneFrequency / sampleRate_;
  for (size_t i = 0; i < values; i += 2) {
    const auto sample = int16_t(std::lround(
        std::sin(kTau * double(phase_) / double(kPhaseScale)) *
        kToneAmplitude));
    stereo[i] = stereo[i + 1] = sample;
    phase_ = (phase_ + phaseStep) % kPhaseScale;
  }
}

bool runGeneratedPattern(const PatternOptions& options,
                         const std::atomic<bool>& stop, std::ostream& status,
                         std::string& error) {
  GroovyTransport transport;
  if (!transport.open(options.target, kToneRate, error) ||
      !transport.switchMode(options.modeline,
                            options.progressiveInterlaceBuffer, error))
    return false;
  transport.setSyncOptions(true, options.frameDelay);
  status << "Pattern streaming; press Ctrl-C to stop.\n";
  std::vector<uint8_t> pixels;
  std::vector<int16_t> toneSamples;
  PatternTone tone(kToneRate);
  auto audioClock = std::chrono::steady_clock::now();
  auto nextStats = audioClock + std::chrono::seconds(5);
  uint32_t frame = 0;
  uint8_t field = 0;
  while (!stop) {
    ++frame;
    transport.alignFrame(frame, field);
    generatePattern(options, frame, field, pixels);
    const auto now = std::chrono::steady_clock::now();
    if (options.tone) {
      if (transport.misterAudioEnabled()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - audioClock)
                                 .count();
        tone.generate(uint64_t(std::max<int64_t>(0, elapsed)), toneSamples);
        if (!toneSamples.empty() &&
            !transport.sendAudio(toneSamples.data(), toneSamples.size(),
                                 error))
          return false;
      } else {
        tone.reset();
      }
      audioClock = now;
    }
    if (!transport.sendFrame(frame, field, pixels, error)) return false;
    transport.waitSync();
    if (std::chrono::steady_clock::now() >= nextStats) {
      printStats(status, transport.stats());
      nextStats += std::chrono::seconds(5);
    }
  }
  transport.close();
  return true;
}
}  // namespace mistercast
