#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "mistercast/audio_pacer.hpp"
#include "mistercast/types.hpp"

namespace mistercast {
class GroovyTransport;

// How often a long-running stream prints its performance counters. Shared by the
// pattern generator and the capture CLI so the cadence is stated once.
inline constexpr auto kStatsInterval = std::chrono::seconds(5);

enum class PatternContent : uint8_t { Bars, Noise };

struct PatternOptions {
  std::string target;
  Modeline modeline{Modeline::safeDefault()};
  PatternContent content{PatternContent::Bars};
  bool tone{}, progressiveInterlaceBuffer{};
  uint16_t frameDelay{};
  // Overridable so a test can cross the reporting boundary without waiting out
  // the default cadence.
  std::chrono::steady_clock::duration statsInterval{kStatsInterval};
};

bool parsePatternOptions(const std::vector<std::string>& arguments,
                         PatternOptions& options, std::string& error);
void generatePattern(const PatternOptions& options, uint32_t frame,
                     uint8_t field, std::vector<uint8_t>& bgr);

class PatternTone {
 public:
  explicit PatternTone(uint32_t sampleRate = 48000) noexcept;
  void reset() noexcept;
  void generate(uint64_t elapsedNs, std::vector<int16_t>& stereo);

 private:
  uint32_t sampleRate_;
  AudioPacer pacer_;
  uint64_t phase_{};
};

bool runGeneratedPattern(const PatternOptions& options,
                         const std::atomic<bool>& stop, std::ostream& status,
                         std::string& error);
// Uses an already-open transport; the caller retains lifecycle ownership.
bool streamGeneratedPattern(const PatternOptions& options,
                            GroovyTransport& transport,
                            const std::atomic<bool>& stop,
                            std::ostream& status, std::string& error);
}  // namespace mistercast
