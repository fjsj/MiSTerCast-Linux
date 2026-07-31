#include "mistercast/interfaces.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#ifdef MISTERCAST_HAVE_PULSE
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <unistd.h>
#endif

namespace mistercast {
#ifdef MISTERCAST_HAVE_PULSE
namespace {
class PulseConnection {
public:
  ~PulseConnection() { close(); }

  bool connect(std::string& error) {
    loop_ = pa_mainloop_new();
    if (!loop_) { error = "cannot create PulseAudio main loop"; return false; }
    context_ = pa_context_new(pa_mainloop_get_api(loop_), "MiSTerCast routing");
    if (!context_) { error = "cannot create PulseAudio context"; close(); return false; }
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      error = pa_strerror(pa_context_errno(context_)); close(); return false;
    }
    for (;;) {
      const auto state = pa_context_get_state(context_);
      if (state == PA_CONTEXT_READY) return true;
      if (!PA_CONTEXT_IS_GOOD(state)) {
        error = pa_strerror(pa_context_errno(context_)); close(); return false;
      }
      int result = 0;
      if (pa_mainloop_iterate(loop_, 1, &result) < 0) {
        error = "PulseAudio connection failed"; close(); return false;
      }
    }
  }

  bool wait(pa_operation* operation) {
    if (!operation) return false;
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
      int result = 0;
      if (pa_mainloop_iterate(loop_, 1, &result) < 0) {
        pa_operation_unref(operation); return false;
      }
    }
    const bool completed = pa_operation_get_state(operation) == PA_OPERATION_DONE;
    pa_operation_unref(operation);
    return completed;
  }

  pa_context* context() const { return context_; }

private:
  void close() {
    if (context_) { pa_context_disconnect(context_); pa_context_unref(context_); context_ = nullptr; }
    if (loop_) { pa_mainloop_free(loop_); loop_ = nullptr; }
  }
  pa_mainloop* loop_{};
  pa_context* context_{};
};

struct ServerQuery { std::string defaultSink; };
void serverInfo(pa_context*, const pa_server_info* info, void* data) {
  auto& query = *static_cast<ServerQuery*>(data);
  if (info && info->default_sink_name) query.defaultSink = info->default_sink_name;
}

struct SinkQuery {
  std::string wanted;
  std::string monitor;
  std::vector<AudioSink>* sinks{};
  std::string defaultSink;
};
void sinkInfo(pa_context*, const pa_sink_info* info, int end, void* data) {
  if (end || !info) return;
  auto& query = *static_cast<SinkQuery*>(data);
  if (query.sinks) query.sinks->push_back({info->name ? info->name : "",
    info->description ? info->description : (info->name ? info->name : ""),
    query.defaultSink == (info->name ? info->name : "")});
  if (query.wanted == (info->name ? info->name : "") && info->monitor_source_name)
    query.monitor = info->monitor_source_name;
}
void sinkIndexInfo(pa_context*, const pa_sink_info* info, int end, void* data) {
  if (!end && info) *static_cast<uint32_t*>(data) = info->index;
}

bool serverAndSinks(PulseConnection& connection, ServerQuery& server,
                    std::vector<AudioSink>* sinks, std::string* monitor,
                    const std::string& wanted) {
  if (!connection.wait(pa_context_get_server_info(connection.context(), serverInfo, &server))) return false;
  SinkQuery query{wanted.empty() ? server.defaultSink : wanted, {}, sinks, server.defaultSink};
  if (!connection.wait(pa_context_get_sink_info_list(connection.context(), sinkInfo, &query))) return false;
  if (monitor) *monitor = std::move(query.monitor);
  return true;
}

void successCallback(pa_context*, int success, void* data) {
  *static_cast<bool*>(data) = success != 0;
}
void moduleCallback(pa_context*, uint32_t index, void* data) {
  *static_cast<uint32_t*>(data) = index;
}

struct SinkInput { uint32_t index, sink; };
void sinkInputInfo(pa_context*, const pa_sink_input_info* info, int end, void* data) {
  if (!end && info) static_cast<std::vector<SinkInput>*>(data)->push_back({info->index, info->sink});
}

class SilentRouting {
public:
  bool start(std::string& monitor, std::string& error) {
    if (!connection_.connect(error)) return false;
    ServerQuery server;
    if (!connection_.wait(pa_context_get_server_info(connection_.context(), serverInfo, &server)) || server.defaultSink.empty()) {
      error = "PulseAudio has no default output sink"; return false;
    }
    originalDefault_ = server.defaultSink;
    sinkName_ = "mistercast_silent_" + std::to_string(::getpid());
    const std::string arguments = "sink_name=" + sinkName_ +
      " rate=48000 channels=2 channel_map=front-left,front-right sink_properties=device.description=MiSTerCast_Silent_Output";
    connection_.wait(pa_context_load_module(connection_.context(), "module-null-sink", arguments.c_str(), moduleCallback, &moduleIndex_));
    if (moduleIndex_ == PA_INVALID_INDEX) {
      error = "cannot create the MiSTerCast silent output sink"; return false;
    }
    connection_.wait(pa_context_get_sink_info_by_name(connection_.context(), sinkName_.c_str(), sinkIndexInfo, &sinkIndex_));

    bool changedDefault = false;
    connection_.wait(pa_context_set_default_sink(connection_.context(), sinkName_.c_str(), successCallback, &changedDefault));
    if (!changedDefault) { error = "cannot select the MiSTerCast silent output sink"; return false; }

    std::vector<SinkInput> inputs;
    if (connection_.wait(pa_context_get_sink_input_info_list(connection_.context(), sinkInputInfo, &inputs))) {
      for (const auto& input : inputs) {
        bool moved = false;
        connection_.wait(pa_context_move_sink_input_by_name(connection_.context(), input.index, sinkName_.c_str(), successCallback, &moved));
        if (moved) movedInputs_.push_back(input);
      }
    }
    monitor = sinkName_ + ".monitor";
    return true;
  }

  void stop() noexcept {
    if (!connection_.context()) return;
    if (!originalDefault_.empty()) {
      bool restored = false;
      connection_.wait(pa_context_set_default_sink(connection_.context(), originalDefault_.c_str(), successCallback, &restored));
      std::vector<SinkInput> inputs;
      connection_.wait(pa_context_get_sink_input_info_list(connection_.context(), sinkInputInfo, &inputs));
      for (const auto& input : inputs) {
        if (input.sink != sinkIndex_) continue;
        bool moved = false;
        const auto previous = std::find_if(movedInputs_.begin(), movedInputs_.end(),
          [&](const SinkInput& candidate) { return candidate.index == input.index; });
        if (previous != movedInputs_.end())
          connection_.wait(pa_context_move_sink_input_by_index(connection_.context(), input.index, previous->sink, successCallback, &moved));
        else
          connection_.wait(pa_context_move_sink_input_by_name(connection_.context(), input.index, originalDefault_.c_str(), successCallback, &moved));
      }
    }
    if (moduleIndex_ != PA_INVALID_INDEX) {
      bool unloaded = false;
      connection_.wait(pa_context_unload_module(connection_.context(), moduleIndex_, successCallback, &unloaded));
      moduleIndex_ = PA_INVALID_INDEX;
    }
    movedInputs_.clear();
  }

  ~SilentRouting() { stop(); }

private:
  PulseConnection connection_;
  std::string originalDefault_, sinkName_;
  std::vector<SinkInput> movedInputs_;
  uint32_t moduleIndex_{PA_INVALID_INDEX};
  uint32_t sinkIndex_{PA_INVALID_INDEX};
};
} // namespace
#endif

std::vector<AudioSink> pulseAudioSinks(std::string& error) {
#ifdef MISTERCAST_HAVE_PULSE
  PulseConnection connection;
  if (!connection.connect(error)) return {};
  ServerQuery server;
  std::vector<AudioSink> sinks;
  if (!serverAndSinks(connection, server, &sinks, nullptr, {})) {
    error = "cannot enumerate PulseAudio output sinks"; return {};
  }
  return sinks;
#else
  error = "PulseAudio support was unavailable at build time";
  return {};
#endif
}

class PulseCapture final : public IAudioCapture {
#ifdef MISTERCAST_HAVE_PULSE
  pa_simple* stream_{};
  std::unique_ptr<SilentRouting> silentRouting_;
#endif
  std::atomic<bool> running_{false};
  ErrorCallback error_;
  uint32_t rate_{48000};

public:
  ~PulseCapture() override { stop(); }

  bool start(const std::string& sink, ErrorCallback callback) override {
    error_ = std::move(callback);
#ifdef MISTERCAST_HAVE_PULSE
    std::string source, routeError;
    if (sink == SilentAudioSink) {
      silentRouting_ = std::make_unique<SilentRouting>();
      if (!silentRouting_->start(source, routeError)) {
        silentRouting_.reset();
        if (error_) error_({"audio", routeError, "Ensure PulseAudio/pipewire-pulse permits loading module-null-sink."});
        return false;
      }
    } else {
      PulseConnection connection;
      ServerQuery server;
      if (!connection.connect(routeError) || !serverAndSinks(connection, server, nullptr, &source, sink) || source.empty()) {
        if (error_) error_({"audio", routeError.empty() ? "selected output sink has no monitor source" : routeError,
          "Refresh the audio output selection, or disable audio."});
        return false;
      }
    }

    int pulseError = 0;
    for (uint32_t candidate : {48000u, 44100u, 22050u}) {
      pa_sample_spec spec{PA_SAMPLE_S16LE, candidate, 2};
      stream_ = pa_simple_new(nullptr, "MiSTerCast", PA_STREAM_RECORD, source.c_str(),
        "System output", &spec, nullptr, nullptr, &pulseError);
      if (stream_) { rate_ = candidate; break; }
    }
    if (!stream_) {
      silentRouting_.reset();
      if (error_) error_({"audio", pa_strerror(pulseError), "Check pipewire-pulse/PulseAudio and the selected output sink."});
      return false;
    }
    running_ = true;
    return true;
#else
    (void)sink;
    if (error_) error_({"audio", "PulseAudio development support was unavailable at build time",
      "Install libpulse-dev and rebuild, or disable audio."});
    return false;
#endif
  }

  bool next(PcmBlock& block, std::chrono::milliseconds timeout) override {
#ifdef MISTERCAST_HAVE_PULSE
    if (timeout <= std::chrono::milliseconds::zero() || !running_ || !stream_) return false;
    const auto readMs = std::min<int64_t>(timeout.count(), 10);
    const auto frames = std::max<uint64_t>(1, uint64_t(rate_) * uint64_t(readMs) / 1000);
    block.sampleRate = rate_;
    block.samples.resize(size_t(frames) * 2);
    int pulseError = 0;
    if (pa_simple_read(stream_, block.samples.data(), block.samples.size() * sizeof(int16_t), &pulseError) < 0) {
      if (error_) error_({"audio", pa_strerror(pulseError), "Check pipewire-pulse/PulseAudio and restart streaming."});
      return false;
    }
    block.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    return true;
#else
    (void)block; (void)timeout;
    return false;
#endif
  }

  void stop() noexcept override {
    running_ = false;
#ifdef MISTERCAST_HAVE_PULSE
    if (stream_) { pa_simple_free(stream_); stream_ = nullptr; }
    silentRouting_.reset();
#endif
  }

  uint32_t sampleRate() const noexcept override { return rate_; }
};

std::unique_ptr<IAudioCapture> makePulseAudioCapture() { return std::make_unique<PulseCapture>(); }
} // namespace mistercast
