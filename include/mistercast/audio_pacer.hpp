#pragma once
#include <cstddef>
#include <cstdint>

namespace mistercast {
class AudioPacer {
public:
 explicit AudioPacer(uint32_t sampleRate=48000)noexcept:sampleRate_(sampleRate){}
 void reset(uint32_t sampleRate)noexcept;
 size_t valuesDue(uint64_t elapsedNs,size_t maxValues)noexcept;
private:
 uint32_t sampleRate_;
 uint64_t fractionalFrameNs_{},pendingFrames_{};
};
}
