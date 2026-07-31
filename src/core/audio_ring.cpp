#include "mistercast/audio_ring.hpp"
#include <algorithm>
namespace mistercast {
AudioRing::AudioRing(size_t n):data_(std::max<size_t>(1,n)){}
size_t AudioRing::push(const int16_t* p,size_t n){std::lock_guard<std::mutex>l(mutex_);size_t dropped=0;for(size_t i=0;i<n;++i){if(size_==data_.size()){read_=(read_+1)%data_.size();--size_;++dropped;}data_[write_]=p[i];write_=(write_+1)%data_.size();++size_;}return dropped;}
size_t AudioRing::pop(int16_t*p,size_t n){std::lock_guard<std::mutex>l(mutex_);size_t real=std::min(n,size_);for(size_t i=0;i<real;++i){p[i]=data_[read_];read_=(read_+1)%data_.size();}std::fill(p+real,p+n,0);size_-=real;return real;}
void AudioRing::reset(){std::lock_guard<std::mutex>l(mutex_);read_=write_=size_=0;}
size_t AudioRing::size()const{std::lock_guard<std::mutex>l(mutex_);return size_;}
}
