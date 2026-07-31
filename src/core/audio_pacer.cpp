#include "mistercast/audio_pacer.hpp"
#include <algorithm>

namespace mistercast {
void AudioPacer::reset(uint32_t sampleRate)noexcept{
 sampleRate_=sampleRate;fractionalFrameNs_=pendingFrames_=0;
}

size_t AudioPacer::valuesDue(uint64_t elapsedNs,size_t maxValues)noexcept{
 constexpr uint64_t nsPerSecond=1000000000;
 const uint64_t seconds=elapsedNs/nsPerSecond;
 const uint64_t remainderNs=elapsedNs%nsPerSecond;
 const uint64_t frameNumerator=fractionalFrameNs_+remainderNs*sampleRate_;
 pendingFrames_+=seconds*sampleRate_+frameNumerator/nsPerSecond;
 fractionalFrameNs_=frameNumerator%nsPerSecond;
 const uint64_t maxFrames=maxValues/2;
 const uint64_t frames=std::min(pendingFrames_,maxFrames);
 pendingFrames_-=frames;
 return size_t(frames*2);
}
}
