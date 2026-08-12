#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>

#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"

#ifdef MISTERCAST_HAVE_PORTAL
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <unistd.h>

#include "portal_screencast.hpp"
#endif

namespace mistercast {
#ifndef MISTERCAST_HAVE_PORTAL
namespace {
// Reported by start() when the backend was selected but not built. Selection
// deliberately still resolves to the portal on a Wayland session, so the message
// names the missing build dependency instead of blaming the display server.
class UnbuiltPortalCapture final : public IVideoCapture {
 public:
  bool start(const CaptureSource&, ErrorCallback callback) override {
    if (callback)
      callback({"video", "this build has no Wayland screen capture",
                "Rebuild MiSTerCast with libpipewire-0.3-dev and "
                "libsystemd-dev installed, or log into an Xorg session."});
    return false;
  }
  SourceGeometry selectedGeometry() const override { return {}; }
  void setRegion(const CropRect&) override {}
  bool next(Frame&, std::chrono::milliseconds) override { return false; }
  void stop() noexcept override {}
};
}  // namespace
#endif

#ifdef MISTERCAST_HAVE_PORTAL
namespace {
// How long PipeWire gets to negotiate a format after the portal grant. The
// producer is already running by then, so this only covers scheduling.
constexpr auto kNegotiationTimeout = std::chrono::seconds(5);

// The byte orders cropToBgra understands. Anything else is refused rather than
// guessed at, and because these four are the only formats advertised, a
// well-behaved producer cannot pick another one.
bool pixelOrderFor(uint32_t format, PixelOrder& order) {
  switch (format) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
      order = PixelOrder::Bgra;
      return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
      order = PixelOrder::Rgba;
      return true;
    default:
      return false;
  }
}

// EnumFormat for a screen-capture consumer. No SPA_FORMAT_VIDEO_modifier is
// advertised, and that omission is the whole dmabuf policy: a producer only
// offers DMA-BUF planes to a client that announced modifiers, so leaving it out
// keeps negotiation on mappable memory and off a GBM/EGL import path this
// application has no other use for.
const spa_pod* buildEnumFormat(spa_pod_builder& builder) {
  static const uint32_t formats[] = {SPA_VIDEO_FORMAT_BGRx,
                                     SPA_VIDEO_FORMAT_BGRA,
                                     SPA_VIDEO_FORMAT_RGBx,
                                     SPA_VIDEO_FORMAT_RGBA};
  // Named locals rather than SPA's SPA_RECTANGLE/SPA_FRACTION helpers: those
  // expand to C compound literals, which -Wpedantic rejects in C++17.
  const spa_rectangle preferredSize{1920, 1080}, minimumSize{1, 1},
      maximumSize{8192, 8192};
  const spa_fraction preferredRate{60, 1}, minimumRate{0, 1},
      maximumRate{1000, 1};
  spa_pod_frame object{}, choice{};
  spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&builder, SPA_FORMAT_mediaType,
                      SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
                      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
  spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_format, 0);
  spa_pod_builder_push_choice(&builder, &choice, SPA_CHOICE_Enum, 0);
  // A choice names its preferred value first, then lists every alternative,
  // so the preferred one appears twice.
  spa_pod_builder_id(&builder, formats[0]);
  for (const auto format : formats) spa_pod_builder_id(&builder, format);
  spa_pod_builder_pop(&builder, &choice);
  spa_pod_builder_add(&builder, SPA_FORMAT_VIDEO_size,
                      SPA_POD_CHOICE_RANGE_Rectangle(&preferredSize,
                                                     &minimumSize,
                                                     &maximumSize),
                      SPA_FORMAT_VIDEO_framerate,
                      SPA_POD_CHOICE_RANGE_Fraction(&preferredRate,
                                                    &minimumRate,
                                                    &maximumRate),
                      0);
  return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object));
}
}  // namespace

// Wayland desktop capture: the ScreenCast portal grants a source and hands back
// a PipeWire node, and this turns that node's push-model stream into the pull
// model IVideoCapture presents to the session.
class PortalCapture final : public IVideoCapture {
  PortalCaptureOptions options_;
  PortalScreenCast portal_;
  pw_thread_loop* loop_{};
  pw_context* context_{};
  pw_core* core_{};
  pw_stream* stream_{};
  spa_hook streamListener_{};
  bool listening_{};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  // The newest frame, already cropped. Held rather than handed over, so a still
  // desktop can be re-delivered; see next().
  Frame pending_;
  uint64_t pendingCount_{}, deliveredCount_{}, sequence_{};
  CropRect region_{};
  uint32_t sourceWidth_{}, sourceHeight_{};
  PixelOrder order_{PixelOrder::Bgra};
  bool negotiated_{};
  // Set by the PipeWire and portal callbacks, reported by next(). Failures are
  // deliberately not delivered from those threads: X11 capture reports through
  // the capture thread inside next(), and the session's error handling is
  // written for that one caller.
  std::optional<SessionError> failure_;
  ErrorCallback error_;
  std::atomic<bool> running_{false};

  static void onStateChanged(void* data, pw_stream_state, pw_stream_state state,
                             const char* error) {
    auto& self = *static_cast<PortalCapture*>(data);
    if (state != PW_STREAM_STATE_ERROR) return;
    std::lock_guard<std::mutex> lock(self.mutex_);
    if (!self.failure_)
      self.failure_ = SessionError{
          "video",
          std::string("the PipeWire capture stream failed") +
              (error ? std::string(": ") + error : std::string()),
          "Check that PipeWire is running, then start the stream again."};
    self.cv_.notify_all();
  }

  static void onParamChanged(void* data, uint32_t id, const spa_pod* param) {
    auto& self = *static_cast<PortalCapture*>(data);
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
        mediaType != SPA_MEDIA_TYPE_video ||
        mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
      return;
    spa_video_info_raw raw{};
    if (spa_format_video_raw_parse(param, &raw) < 0) return;
    PixelOrder order{PixelOrder::Bgra};
    {
      std::lock_guard<std::mutex> lock(self.mutex_);
      if (!pixelOrderFor(raw.format, order)) {
        self.failure_ = SessionError{
            "video", "the compositor offered an unsupported pixel format",
            "Report the compositor and PipeWire versions; MiSTerCast needs a "
            "32-bit BGRA or RGBA screen-capture format."};
        self.cv_.notify_all();
        return;
      }
      self.order_ = order;
      self.sourceWidth_ = raw.size.width;
      self.sourceHeight_ = raw.size.height;
      // A renegotiation can shrink the source under an existing crop. Reset to
      // the whole frame so nothing is dropped in the window before the session
      // notices the new geometry and narrows the region again.
      self.region_ = {};
      self.negotiated_ = true;
      self.cv_.notify_all();
    }
    // Only mappable memory is accepted, matching the format request above.
    uint8_t buffer[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) |
                                 (1 << SPA_DATA_MemPtr))));
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
        SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(spa_meta_header))));
    pw_stream_update_params(self.stream_, params, 2);
  }

  static void onProcess(void* data) {
    auto& self = *static_cast<PortalCapture*>(data);
    // Drain to the newest buffer: the MiSTer is paced by its own raster, so an
    // older queued frame has no consumer and only delays the current one.
    pw_buffer* newest = nullptr;
    while (auto* buffer = pw_stream_dequeue_buffer(self.stream_)) {
      if (newest) pw_stream_queue_buffer(self.stream_, newest);
      newest = buffer;
    }
    if (!newest) return;
    self.consume(*newest->buffer);
    pw_stream_queue_buffer(self.stream_, newest);
  }

  void consume(const spa_buffer& buffer) {
    if (!buffer.n_datas) return;
    const auto& data = buffer.datas[0];
    if (!data.data || !data.chunk || !data.chunk->size) return;
    if (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sourceHeight_) return;
    const uint32_t stride =
        data.chunk->stride > 0 ? uint32_t(data.chunk->stride)
                               : uint32_t(data.chunk->size) / sourceHeight_;
    std::string error;
    if (cropToBgra(static_cast<const uint8_t*>(data.data) + data.chunk->offset,
                   data.chunk->size, sourceWidth_, sourceHeight_, stride,
                   order_, region_, pending_, error)) {
      ++pendingCount_;
      cv_.notify_all();
    }
  }

  void teardownPipeWire() noexcept {
    if (loop_) pw_thread_loop_stop(loop_);
    if (stream_) {
      if (listening_) {
        spa_hook_remove(&streamListener_);
        listening_ = false;
      }
      pw_stream_destroy(stream_);
      stream_ = nullptr;
    }
    if (core_) {
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    if (loop_) {
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
    }
  }

  bool startPipeWire(const PortalStream& granted, SessionError& error) {
    // Zeroed then assigned by name: designated initializers are not C++17, and
    // positional initialization would silently bind to the wrong slot if
    // PipeWire ever inserts an event rather than appending one.
    static pw_stream_events events{};
    static std::once_flag eventsOnce;
    std::call_once(eventsOnce, [] {
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = onStateChanged;
      events.param_changed = onParamChanged;
      events.process = onProcess;
    });
    loop_ = pw_thread_loop_new("mistercast-pw", nullptr);
    if (!loop_) {
      error = {"video", "cannot start the PipeWire loop",
               "Check that the PipeWire client library is installed."};
      return false;
    }
    if (pw_thread_loop_start(loop_) < 0) {
      error = {"video", "cannot run the PipeWire loop",
               "Check that PipeWire is running in this session."};
      return false;
    }
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    // The descriptor is handed over here: PipeWire closes it with the core, so
    // it must not be closed again on the failure paths below.
    if (context_)
      core_ = pw_context_connect_fd(context_, granted.pipewireFd, nullptr, 0);
    if (!core_) {
      pw_thread_loop_unlock(loop_);
      if (!context_) ::close(granted.pipewireFd);
      error = {"video", "cannot connect to the PipeWire remote the portal gave",
               "Check that PipeWire is running in this session."};
      return false;
    }
    stream_ = pw_stream_new(
        core_, "mistercast",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                          "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!stream_) {
      pw_thread_loop_unlock(loop_);
      error = {"video", "cannot create the PipeWire capture stream",
               "Check that PipeWire is running in this session."};
      return false;
    }
    pw_stream_add_listener(stream_, &streamListener_, &events, this);
    listening_ = true;
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod* params[1] = {buildEnumFormat(builder)};
    const int connected = pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, granted.nodeId,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS),
        params, 1);
    pw_thread_loop_unlock(loop_);
    if (connected < 0) {
      error = {"video", "cannot read the capture stream the portal granted",
               "Start the stream again; if it keeps failing, restart the "
               "desktop portal."};
      return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    // The session asks for the source geometry as soon as start() returns, so
    // the negotiated size has to be known by then rather than a frame later.
    if (!cv_.wait_for(lock, kNegotiationTimeout,
                      [this] { return negotiated_ || failure_.has_value(); })) {
      error = {"video", "the compositor did not start the capture stream",
               "Check that PipeWire and the desktop portal are running."};
      return false;
    }
    if (failure_) {
      error = *failure_;
      failure_.reset();
      return false;
    }
    return true;
  }

 public:
  explicit PortalCapture(PortalCaptureOptions options)
      : options_(std::move(options)) {}
  ~PortalCapture() override { stop(); }

  bool start(const CaptureSource& source, ErrorCallback callback) override {
    stop();
    // Once per process, and never torn down: pw_deinit would have to outlive
    // every capture object to be safe, and the process is about to exit anyway.
    static std::once_flag pipewireOnce;
    std::call_once(pipewireOnce, [] { pw_init(nullptr, nullptr); });
    PortalScreenCast::Request request;
    // The portal picker decides which monitor or window is shared, so the
    // runtime source only says which kind to ask for. A monitor name from the
    // configuration has no meaning here; there is no way to name an output.
    request.preference = std::holds_alternative<WindowCaptureSource>(source)
                             ? CapturePreference::Window
                             : CapturePreference::Monitor;
    request.restoreToken = options_.restoreToken;
    request.embedCursor = options_.embedCursor;
    PortalStream granted;
    SessionError error;
    if (!portal_.open(request, granted, error)) {
      if (callback) callback(error);
      return false;
    }
    if (options_.onRestoreToken && !granted.restoreToken.empty())
      options_.onRestoreToken(granted.restoreToken);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pendingCount_ = deliveredCount_ = sequence_ = 0;
      negotiated_ = false;
      failure_.reset();
      region_ = {};
    }
    if (!startPipeWire(granted, error)) {
      teardownPipeWire();
      portal_.close();
      if (callback) callback(error);
      return false;
    }
    error_ = std::move(callback);
    portal_.watch([this] {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!failure_)
        failure_ = SessionError{"video", "desktop screen sharing was stopped",
                                "Start the stream again and allow sharing."};
      cv_.notify_all();
    });
    running_ = true;
    return true;
  }

  SourceGeometry selectedGeometry() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return {uint16_t(sourceWidth_), uint16_t(sourceHeight_)};
  }

  void setRegion(const CropRect& region) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!region.width || !region.height ||
        uint64_t(region.x) + region.width > sourceWidth_ ||
        uint64_t(region.y) + region.height > sourceHeight_) {
      region_ = {};
      return;
    }
    region_ = region;
  }

  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    if (!running_) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] {
      return failure_.has_value() || pendingCount_ > deliveredCount_;
    });
    if (failure_) {
      const auto reported = *failure_;
      failure_.reset();
      lock.unlock();
      running_ = false;
      if (error_) error_(reported);
      return false;
    }
    // Nothing has arrived at all yet: the compositor has not produced its first
    // frame, and there is no previous one to stand in for it.
    if (!pendingCount_) return false;
    // A compositor produces a frame only when the screen changes, so a still
    // desktop legitimately has nothing new. Handing back the last frame again
    // keeps the MiSTer refreshing at the modeline rate, which is exactly what
    // X11 capture does when it re-reads unchanged pixels. The copy is what pays
    // for it: the frame stays here so it can be delivered more than once.
    out.width = pending_.width;
    out.height = pending_.height;
    out.stride = pending_.stride;
    out.bgra.resize(pending_.bgra.size());
    std::memcpy(out.bgra.data(), pending_.bgra.data(), pending_.bgra.size());
    deliveredCount_ = pendingCount_;
    out.sequence = ++sequence_;
    return true;
  }

  void stop() noexcept override {
    running_ = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cv_.notify_all();
    }
    teardownPipeWire();
    portal_.close();
    std::lock_guard<std::mutex> lock(mutex_);
    negotiated_ = false;
    failure_.reset();
    error_ = {};
  }
};
#endif  // MISTERCAST_HAVE_PORTAL

bool portalCaptureAvailable() noexcept {
#ifdef MISTERCAST_HAVE_PORTAL
  return true;
#else
  return false;
#endif
}

std::unique_ptr<IVideoCapture> makePortalCapture(PortalCaptureOptions options) {
#ifdef MISTERCAST_HAVE_PORTAL
  return std::make_unique<PortalCapture>(std::move(options));
#else
  (void)options;
  return std::make_unique<UnbuiltPortalCapture>();
#endif
}
}  // namespace mistercast
