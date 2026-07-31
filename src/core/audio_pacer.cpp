#include "mistercast/audio_pacer.hpp"
#include <algorithm>
#include <cstring>

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

size_t AudioPacer::sourceValuesFor(size_t outputValues,size_t bufferedValues,size_t targetValues,size_t hysteresisValues)const noexcept{
 if(!outputValues)return 0;
 if(bufferedValues>targetValues+hysteresisValues)return outputValues+2;
 if(bufferedValues+hysteresisValues<targetValues&&outputValues>=4)return outputValues-2;
 return outputValues;
}

void AudioPacer::conformStereo(const int16_t*source,size_t sourceValues,std::vector<int16_t>&output,size_t outputValues){
 output.resize(outputValues);
 if(!outputValues)return;
 if(!sourceValues){std::fill(output.begin(),output.end(),0);return;}
 if(sourceValues==outputValues){std::memcpy(output.data(),source,outputValues*sizeof(int16_t));return;}
 const size_t outputFrames=outputValues/2,sourceFrames=sourceValues/2;
 const size_t pivot=std::min(outputFrames/2,sourceFrames-1);
 if(sourceValues==outputValues+2){
  std::memcpy(output.data(),source,pivot*2*sizeof(int16_t));
  std::memcpy(output.data()+pivot*2,source+(pivot+1)*2,(outputFrames-pivot)*2*sizeof(int16_t));
  return;
 }
 if(sourceValues+2==outputValues){
  std::memcpy(output.data(),source,(pivot+1)*2*sizeof(int16_t));
  std::memcpy(output.data()+(pivot+1)*2,source+pivot*2,2*sizeof(int16_t));
  std::memcpy(output.data()+(pivot+2)*2,source+(pivot+1)*2,(sourceFrames-pivot-1)*2*sizeof(int16_t));
  return;
 }
 const size_t copied=std::min(sourceValues,outputValues);
 std::memcpy(output.data(),source,copied*sizeof(int16_t));
 std::fill(output.begin()+copied,output.end(),0);
}
}
