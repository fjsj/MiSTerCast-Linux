#include "mistercast/interfaces.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#ifdef MISTERCAST_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#endif
namespace mistercast {
#ifdef MISTERCAST_HAVE_PULSE
struct PulseQuery { pa_mainloop* loop{};std::string sink,source;bool done{}; };
static void serverInfo(pa_context*,const pa_server_info*i,void*p){auto&q=*static_cast<PulseQuery*>(p);if(i&&i->default_sink_name)q.sink=i->default_sink_name;q.done=true;}
static void sinkInfo(pa_context*,const pa_sink_info*i,int end,void*p){auto&q=*static_cast<PulseQuery*>(p);if(!end&&i&&i->monitor_source_name)q.source=i->monitor_source_name;if(end)q.done=true;}
static bool waitOperation(PulseQuery&q,pa_operation*op){while(pa_operation_get_state(op)==PA_OPERATION_RUNNING){int rc;if(pa_mainloop_iterate(q.loop,1,&rc)<0){pa_operation_unref(op);return false;}}pa_operation_unref(op);return true;}
static std::string defaultMonitor(){PulseQuery q;q.loop=pa_mainloop_new();if(!q.loop)return{};auto*c=pa_context_new(pa_mainloop_get_api(q.loop),"MiSTerCast query");if(!c){pa_mainloop_free(q.loop);return{};}if(pa_context_connect(c,nullptr,PA_CONTEXT_NOFLAGS,nullptr)<0){pa_context_unref(c);pa_mainloop_free(q.loop);return{};}for(;;){auto s=pa_context_get_state(c);if(s==PA_CONTEXT_READY)break;if(!PA_CONTEXT_IS_GOOD(s)){pa_context_disconnect(c);pa_context_unref(c);pa_mainloop_free(q.loop);return{};}int rc;if(pa_mainloop_iterate(q.loop,1,&rc)<0)return{};}q.done=false;if(!waitOperation(q,pa_context_get_server_info(c,serverInfo,&q))||q.sink.empty())q.done=true;if(!q.sink.empty()){q.done=false;waitOperation(q,pa_context_get_sink_info_by_name(c,q.sink.c_str(),sinkInfo,&q));}pa_context_disconnect(c);pa_context_unref(c);pa_mainloop_free(q.loop);return q.source;}
#endif
class PulseCapture final:public IAudioCapture{
#ifdef MISTERCAST_HAVE_PULSE
 pa_simple*stream_{};
#endif
 std::atomic<bool>running_{false};ErrorCallback error_;uint32_t rate_{48000};std::mutex mutex_;
public:~PulseCapture()override{stop();}bool start(ErrorCallback cb)override{error_=std::move(cb);
#ifdef MISTERCAST_HAVE_PULSE
 auto source=defaultMonitor();if(source.empty()){if(error_)error_({"audio","default sink has no monitor source","Start PulseAudio/pipewire-pulse, or disable audio."});return false;}int err=0;for(uint32_t candidate:{48000u,44100u,22050u}){pa_sample_spec ss{PA_SAMPLE_S16LE,candidate,2};stream_=pa_simple_new(nullptr,"MiSTerCast",PA_STREAM_RECORD,source.c_str(),"System output",&ss,nullptr,nullptr,&err);if(stream_){rate_=candidate;break;}}if(!stream_){if(error_)error_({"audio",pa_strerror(err),"Set the recording source to the default sink monitor, or disable audio."});return false;}running_=true;return true;
#else
 if(error_)error_({"audio","PulseAudio development support was unavailable at build time","Install libpulse-dev and rebuild, or disable audio."});return false;
#endif
 }bool next(PcmBlock&b,std::chrono::milliseconds timeout)override{(void)timeout;
#ifdef MISTERCAST_HAVE_PULSE
 std::lock_guard<std::mutex> lock(mutex_);if(!running_||!stream_)return false;b.sampleRate=rate_;b.samples.resize(1600);int err=0;if(pa_simple_read(stream_,b.samples.data(),b.samples.size()*sizeof(int16_t),&err)<0){if(error_)error_({"audio",pa_strerror(err),"Check pipewire-pulse/PulseAudio and restart streaming."});return false;}b.timestampNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();return true;
#else
 return false;
#endif
 }void stop()noexcept override{running_=false;std::lock_guard<std::mutex> lock(mutex_);
#ifdef MISTERCAST_HAVE_PULSE
 if(stream_){pa_simple_free(stream_);stream_=nullptr;}
#endif
 }uint32_t sampleRate()const noexcept override{return rate_;}};
std::unique_ptr<IAudioCapture> makePulseAudioCapture(){return std::make_unique<PulseCapture>();}
}
