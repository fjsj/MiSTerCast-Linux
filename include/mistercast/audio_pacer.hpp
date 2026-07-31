#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mistercast {
class AudioPacer {
public:
 explicit AudioPacer(uint32_t sampleRate=48000)noexcept:sampleRate_(sampleRate){}
 void reset(uint32_t sampleRate)noexcept;
 size_t valuesDue(uint64_t elapsedNs,size_t maxValues)noexcept;
 size_t sourceValuesFor(size_t outputValues,size_t bufferedValues,size_t targetValues,size_t hysteresisValues)const noexcept;
 static void conformStereo(const int16_t*source,size_t sourceValues,std::vector<int16_t>&output,size_t outputValues);
private:
 uint32_t sampleRate_;
 uint64_t fractionalFrameNs_{},pendingFrames_{};
};
}
