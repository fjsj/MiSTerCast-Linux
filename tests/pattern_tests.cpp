#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include "fake_groovy_endpoint.hpp"
#include "mistercast/pattern.hpp"

using namespace mistercast;
using namespace mistercast::test;

namespace {
int failed = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
      ++failed;                                                               \
    }                                                                         \
  } while (0)

void checkParsingAndGeneration() {
  PatternOptions options;
  std::string error;
  CHECK(!parsePatternOptions({}, options, error));
  CHECK(!parsePatternOptions({"--target", "host", "--content", "solid"},
                             options, error));
  CHECK(!parsePatternOptions({"--target", "host", "--frame-delay", "11"},
                             options, error));
  CHECK(parsePatternOptions(
      {"--target", "host", "--tone", "--content", "noise",
       "--progressive-interlace-buffer", "--frame-delay", "4",
       "--modeline", "1 16 18 20 24 8 9 10 12 1"},
      options, error));
  CHECK(options.target == "host" && options.tone &&
        options.content == PatternContent::Noise &&
        options.progressiveInterlaceBuffer && options.frameDelay == 4 &&
        options.modeline.interlaced);

  options.modeline = {"pattern", 1, 16, 18, 20, 24, 8, 9, 10, 12, false};
  options.progressiveInterlaceBuffer = false;
  options.content = PatternContent::Bars;
  std::vector<uint8_t> even, odd;
  generatePattern(options, 2, 0, even);
  generatePattern(options, 3, 1, odd);
  CHECK(even.size() == size_t(16 * 8 * 3));
  CHECK(even[0] == 255 && even[1] == 255 && even[2] == 255);
  CHECK(odd[0] == 0 && odd[1] == 0 && odd[2] == 0);
  const size_t secondBar = size_t(2) * 3;
  CHECK(even[secondBar] == 0 && even[secondBar + 1] == 255 &&
        even[secondBar + 2] == 255);
  CHECK(even[even.size() - 3] == 255 && even[even.size() - 2] == 0);
  CHECK(odd[odd.size() - 3] == 0 && odd[odd.size() - 2] == 255);

  options.modeline.interlaced = true;
  options.content = PatternContent::Noise;
  options.progressiveInterlaceBuffer = true;
  std::vector<uint8_t> full, repeated, changed, fieldZero, fieldOne;
  generatePattern(options, 7, 0, full);
  generatePattern(options, 7, 0, repeated);
  generatePattern(options, 8, 0, changed);
  CHECK(full == repeated && full != changed);
  size_t equalNeighbours = 0;
  for (size_t i = 3; i < full.size(); i += 3)
    if (std::memcmp(full.data() + i - 3, full.data() + i, 3) == 0)
      ++equalNeighbours;
  CHECK(equalNeighbours < full.size() / 96);
  options.progressiveInterlaceBuffer = false;
  generatePattern(options, 7, 0, fieldZero);
  generatePattern(options, 7, 1, fieldOne);
  CHECK(fieldZero.size() == full.size() / 2 &&
        fieldOne.size() == full.size() / 2);
  const size_t rowBytes = size_t(options.modeline.hActive) * 3;
  for (size_t y = 0; y < options.modeline.vActive / 2; ++y) {
    CHECK(std::memcmp(fieldZero.data() + y * rowBytes,
                      full.data() + (y * 2 + 1) * rowBytes, rowBytes) == 0);
    CHECK(std::memcmp(fieldOne.data() + y * rowBytes,
                      full.data() + y * 2 * rowBytes, rowBytes) == 0);
  }
}

void checkToneContinuity() {
  PatternTone split;
  std::vector<int16_t> part, combined;
  for (uint64_t elapsed : {10000000ull, 7000000ull, 13000000ull}) {
    std::vector<int16_t> generated;
    split.generate(elapsed, generated);
    part.insert(part.end(), generated.begin(), generated.end());
  }
  PatternTone once;
  once.generate(30000000, combined);
  CHECK(part == combined && combined.size() == 2880);
  for (size_t i = 0; i < combined.size(); i += 2)
    CHECK(combined[i] == combined[i + 1]);
}

void checkRunnerProtocol() {
  std::atomic<bool> stop{false};
  std::vector<char> events;
  std::vector<uint8_t> fields;
  bool sawInit = false, sawMode = false, sawClose = false;
  size_t payloadRemaining = 0;
  char payloadEvent = 0;
  FakeGroovyEndpoint endpoint(
      [&](auto& peer, const auto& packet) {
        if (payloadRemaining) {
          payloadRemaining -= std::min(payloadRemaining, packet.size());
          if (!payloadRemaining) {
            events.push_back(payloadEvent);
            if (payloadEvent == 'V' && fields.size() >= 4) stop = true;
          }
          return;
        }
        if (packet.empty()) return;
        if (packet[0] == 2) {
          sawInit = true;
          peer.replyAck({0, 0, 0, 0, 0xc4});
        } else if (packet[0] == 3) {
          sawMode = true;
        } else if (packet[0] == 4) {
          payloadRemaining = packetU16(packet, 1);
          payloadEvent = 'A';
        } else if (packet[0] == 7) {
          fields.push_back(packet.at(5));
          const auto compressed = packet.size() == 12 ? packetU32(packet, 8) : 0;
          payloadRemaining = compressed ? compressed : size_t(8 * 8 * 3 / 2);
          payloadEvent = 'V';
          peer.replyAck({packetU32(packet, 1), packetU16(packet, 6),
                         packetU32(packet, 1), packetU16(packet, 6),
                         uint8_t(0xc4 | ((fields.size() & 1) ? 0x20 : 0))});
        } else if (packet[0] == 1) {
          sawClose = true;
        }
      },
      32100);
  if (!endpoint.valid()) return;

  PatternOptions options;
  options.target = "localhost";
  options.tone = true;
  options.modeline = {"runner", 1, 8, 10, 12, 100, 8, 10, 12, 100, true};
  std::ostringstream status;
  std::string error;
  CHECK(runGeneratedPattern(options, stop, status, error));
  endpoint.stop();
  CHECK(sawInit && sawMode && sawClose && fields.size() >= 4);
  CHECK(fields[0] != fields[1] && fields[1] != fields[2]);
  CHECK(std::find(events.begin(), events.end(), 'A') != events.end());
  size_t videos = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i] != 'V') continue;
    if (videos++) CHECK(i > 0 && events[i - 1] == 'A');
  }
  CHECK(videos >= 4);
}
}  // namespace

int main() {
  checkParsingAndGeneration();
  checkToneContinuity();
  checkRunnerProtocol();
  return failed ? 1 : 0;
}
