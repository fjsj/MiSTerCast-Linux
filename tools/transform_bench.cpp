#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

#include "mistercast/transform.hpp"

using namespace mistercast;

int main() {
  const Modeline output = Modeline::safeDefault();
  const SamplingMode modes[] = {SamplingMode::Point, SamplingMode::Bilinear,
                                SamplingMode::LineBlend};
  const std::pair<uint32_t, uint32_t> sizes[] = {
      {960, 720}, {1440, 1080}, {2880, 2160}};
  uint64_t totalChecksum = 0;
  for (const auto [width, height] : sizes) {
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 4;
    frame.bgra.resize(size_t(frame.stride) * height);
    uint32_t state = 0x6d2b79f5u;
    for (auto& byte : frame.bgra) {
      state = state * 1664525u + 1013904223u;
      byte = uint8_t(state >> 24);
    }
    SourceOptions source;
    source.crop = CropMode::Custom;
    source.width = uint16_t(width);
    source.height = uint16_t(height);
    const CropRect crop{0, 0, width, height};
    for (const auto mode : modes) {
      source.sampling = mode;
      std::vector<uint8_t> outputPixels;
      std::string error;
      for (int warmup = 0; warmup < 3; ++warmup)
        if (!transformRgb24(frame, crop, source, output, 0, outputPixels,
                            error)) {
          std::cerr << error << '\n';
          return 1;
        }
      std::vector<uint64_t> samples;
      for (int iteration = 0; iteration < 11; ++iteration) {
        const auto begin = std::chrono::steady_clock::now();
        if (!transformRgb24(frame, crop, source, output, 0, outputPixels,
                            error)) {
          std::cerr << error << '\n';
          return 1;
        }
        samples.push_back(uint64_t(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                       begin).count()));
      }
      std::sort(samples.begin(), samples.end());
      const auto checksum = std::accumulate(
          outputPixels.begin(), outputPixels.end(), uint64_t{},
          [](uint64_t sum, uint8_t value) { return (sum * 131) ^ value; });
      totalChecksum ^= checksum;
      std::cout << width << 'x' << height << " -> 320x240 "
                << toString(mode) << ": median "
                << samples[samples.size() / 2] << " us, checksum " << checksum
                << '\n';
    }
  }
  std::cout << "combined checksum " << totalChecksum << '\n';
}
