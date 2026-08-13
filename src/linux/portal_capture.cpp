#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "mistercast/frame_slot.hpp"
#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"

#ifdef MISTERCAST_HAVE_PORTAL
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>
#include <unistd.h>

#include "portal_screencast.hpp"

// SPA_DATA_FLAG_MAPPABLE arrived after PipeWire 0.3.48, the version Ubuntu 22.04
// ships. The value is fixed by the ABI, and an SPA that old has no producer that
// sets it, so on 22.04 the DmaBuf branch this guards simply never matches.
#ifndef SPA_DATA_FLAG_MAPPABLE
#define SPA_DATA_FLAG_MAPPABLE (1u << 3)
#endif
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

// One buffer's pixels, mapped here rather than by PW_STREAM_FLAG_MAP_BUFFERS.
// PipeWire takes the mapping's protection from the producer's
// SPA_DATA_FLAG_READABLE/WRITABLE, and xdg-desktop-portal-wlr sets neither, so
// that flag maps buffers this side cannot read. PROT_READ says what is needed
// instead of depending on a producer's flags.
struct MappedBuffer {
  void* base{};
  size_t length{};
  // Where the frame starts, which is the mapping plus the data's map offset.
  const uint8_t* pixels{};

  ~MappedBuffer() {
    if (base) ::munmap(base, length);
  }
  bool map(const spa_data& data) {
    if (data.type == SPA_DATA_MemPtr) {
      // Already a pointer into this process; there is nothing to map.
      pixels = static_cast<const uint8_t*>(data.data);
      return pixels != nullptr;
    }
    if (data.type != SPA_DATA_MemFd && data.type != SPA_DATA_DmaBuf)
      return false;
    if (data.type == SPA_DATA_DmaBuf && !(data.flags & SPA_DATA_FLAG_MAPPABLE))
      return false;
    if (data.fd < 0) return false;
    length = size_t(data.mapoffset) + data.maxsize;
    void* mapped = ::mmap(nullptr, length, PROT_READ, MAP_SHARED,
                          int(data.fd), 0);
    if (mapped == MAP_FAILED) {
      length = 0;
      return false;
    }
    base = mapped;
    pixels = static_cast<const uint8_t*>(base) + data.mapoffset;
    return true;
  }
};
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
  // Touched only by the PipeWire thread while the loop runs, and by
  // teardownPipeWire once it has stopped.
  std::map<const pw_buffer*, std::unique_ptr<MappedBuffer>> mappings_;

  // Every rule about waiting for, re-delivering and dropping frames lives in the
  // slot, which needs no PipeWire and is tested without a compositor.
  FrameSlot slot_;
  // Guards the negotiated format and the crop derived from it. Where both locks
  // are held, this one is always taken first and the slot's second -- consume()
  // and setRegion() do exactly that. The reverse cannot happen, because FrameSlot
  // never calls back into this class.
  mutable std::mutex mutex_;
  std::condition_variable formatReady_;
  CropRect region_{};
  uint32_t sourceWidth_{}, sourceHeight_{};
  PixelOrder order_{PixelOrder::Bgra};
  bool negotiated_{};
  // Mirrors "the slot has a failure" under mutex_. The negotiation wait needs it
  // in its predicate, and a predicate reading state guarded by another lock while
  // waiting on this one loses wakeups: every notify below fires unlocked.
  bool negotiationFailed_{};
  ErrorCallback error_;
  std::atomic<bool> running_{false};

  static void onStateChanged(void* data, pw_stream_state, pw_stream_state state,
                             const char* error) {
    auto& self = *static_cast<PortalCapture*>(data);
    if (state != PW_STREAM_STATE_ERROR) return;
    self.slot_.fail(
        {"video",
         std::string("the PipeWire capture stream failed") +
             (error ? std::string(": ") + error : std::string()),
         "Check that PipeWire is running, then start the stream again."});
    // Also wakes a start() still waiting for the first format.
    self.failNegotiation();
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
    if (!pixelOrderFor(raw.format, order)) {
      self.slot_.fail(
          {"video", "the compositor offered an unsupported pixel format",
           "Report the compositor and PipeWire versions; MiSTerCast needs a "
           "32-bit BGRA or RGBA screen-capture format."});
      self.failNegotiation();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(self.mutex_);
      self.order_ = order;
      self.sourceWidth_ = raw.size.width;
      self.sourceHeight_ = raw.size.height;
      // A renegotiation can shrink the source under an existing crop. Reset to
      // the whole frame so nothing is dropped in the window before the session
      // notices the new geometry and narrows the region again.
      self.region_ = {};
      self.negotiated_ = true;
    }
    // The held frame was cropped from the old format, so it no longer describes
    // what a caller asking for this one expects.
    self.slot_.discard();
    self.formatReady_.notify_all();
    // No pw_stream_update_params reply on purpose: answering with
    // SPA_PARAM_Buffers renegotiates, which reallocates buffers and fires
    // param_changed again, and that churn reached consume() with metadata that
    // no longer described the mapping. Nothing here needs it -- the EnumFormat
    // omits SPA_FORMAT_VIDEO_modifier, which is what keeps buffers mappable.
  }

  // Mapped once per buffer rather than once per frame: an mmap and munmap every
  // refresh would be pure overhead, and this is the only point where a mapping
  // failure can be reported before pixels are expected.
  static void onAddBuffer(void* data, pw_buffer* buffer) {
    auto& self = *static_cast<PortalCapture*>(data);
    auto mapping = std::make_unique<MappedBuffer>();
    if (!buffer->buffer->n_datas ||
        !mapping->map(buffer->buffer->datas[0])) {
      self.slot_.fail(
          {"video", "cannot read the compositor's capture buffers",
           "Report the compositor and PipeWire versions; MiSTerCast needs "
           "shared-memory screen capture buffers."});
      self.failNegotiation();
      return;
    }
    // Owned here rather than by user_data alone, so teardown frees the mappings
    // whether or not PipeWire announced their removal first. A stream that went
    // away without a remove_buffer for each buffer would otherwise leak a whole
    // screen's worth of mapping per start/stop cycle.
    buffer->user_data = mapping.get();
    self.mappings_.emplace(buffer, std::move(mapping));
  }

  static void onRemoveBuffer(void* data, pw_buffer* buffer) {
    auto& self = *static_cast<PortalCapture*>(data);
    buffer->user_data = nullptr;
    self.mappings_.erase(buffer);
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
    self.consume(*newest);
    pw_stream_queue_buffer(self.stream_, newest);
  }

  void consume(const pw_buffer& buffer) {
    const auto* mapping = static_cast<const MappedBuffer*>(buffer.user_data);
    if (!mapping || !mapping->pixels) return;
    if (!buffer.buffer->n_datas) return;
    const auto& data = buffer.buffer->datas[0];
    if (!data.chunk || !data.chunk->size) return;
    if (data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) return;
    if (data.chunk->offset > data.maxsize) return;
    // chunk->size describes what the producer wrote and maxsize the buffer it was
    // written into, so the copy is bounded by both: a buffer whose metadata does
    // not match the negotiated format then fails cropToBgra's own bounds check
    // and is skipped rather than read past.
    const size_t available = data.maxsize - data.chunk->offset;
    const size_t size = std::min<size_t>(data.chunk->size, available);
    // The crop happens under the lock, and publishing before releasing it is the
    // point: setRegion runs on the capture thread and drops the held frame when
    // the region changes, so a crop that read the old region outside the lock
    // could publish afterwards and put that frame back -- exactly what setRegion
    // exists to prevent, with a whole-frame copy as the window. Nothing waits on
    // this lock that matters: next() takes only the slot's, so holding it here
    // delays setRegion, selectedGeometry and onParamChanged and nothing else.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sourceHeight_) return;
    const uint32_t stride = data.chunk->stride > 0
                                ? uint32_t(data.chunk->stride)
                                : uint32_t(size / sourceHeight_);
    std::string error;
    if (cropToBgra(mapping->pixels + data.chunk->offset, size, sourceWidth_,
                   sourceHeight_, stride, order_, region_, slot_.staging(),
                   error))
      slot_.publish();
  }

  // Records a failure for the negotiation wait and wakes it. Set under mutex_ so
  // the wait's predicate reads only state that mutex_ guards.
  void failNegotiation() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      negotiationFailed_ = true;
    }
    formatReady_.notify_all();
  }

  // Safe to touch mappings_ only after the loop has stopped, which is the point
  // at which the PipeWire thread -- their only other user -- is gone.
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
    mappings_.clear();
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
      events.add_buffer = onAddBuffer;
      events.remove_buffer = onRemoveBuffer;
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
        // No PW_STREAM_FLAG_MAP_BUFFERS: see MappedBuffer for why the mapping
        // is done here instead.
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT),
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
    const bool answered = formatReady_.wait_for(
        lock, kNegotiationTimeout,
        [this] { return negotiated_ || negotiationFailed_; });
    lock.unlock();
    if (auto failure = slot_.takeFailure()) {
      error = *failure;
      return false;
    }
    if (!answered) {
      error = {"video", "the compositor did not start the capture stream",
               "Check that PipeWire and the desktop portal are running."};
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
    PortalStream granted;
    SessionError error;
    if (!portal_.open(request, granted, error)) {
      // A stored grant the portal will not honour any more has to cost a dialog,
      // not the stream. Retried once, with the token dropped, so a token that is
      // somehow always refused cannot loop.
      if (request.restoreToken.empty()) {
        if (callback) callback(error);
        return false;
      }
      portal_.close();
      request.restoreToken.clear();
      if (!portal_.open(request, granted, error)) {
        if (callback) callback(error);
        return false;
      }
    }
    if (options_.onRestoreToken && !granted.restoreToken.empty())
      options_.onRestoreToken(granted.restoreToken);
    slot_.reset();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      negotiated_ = false;
      negotiationFailed_ = false;
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
      slot_.fail({"video", "desktop screen sharing was stopped",
                  "Start the stream again and allow sharing."});
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
    const bool usable = region.width && region.height &&
                        uint64_t(region.x) + region.width <= sourceWidth_ &&
                        uint64_t(region.y) + region.height <= sourceHeight_;
    // A region that cannot fit the source would read past the buffer; capturing
    // the whole frame is the safe reading of it, and the session narrows the
    // region again once it sees the geometry that made this one impossible.
    const CropRect wanted = usable ? region : CropRect{};
    if (wanted.x == region_.x && wanted.y == region_.y &&
        wanted.width == region_.width && wanted.height == region_.height)
      return;
    region_ = wanted;
    // Only on a real change, so a session re-asserting the same region every
    // cycle is not starved of frames.
    slot_.discard();
  }

  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    if (!running_) return false;
    switch (slot_.next(out, timeout)) {
      case FrameDelivery::Delivered:
        return true;
      case FrameDelivery::Empty:
        return false;
      case FrameDelivery::Failed:
        break;
    }
    // Reported here rather than from the callback that recorded it: X11 capture
    // reports through the capture thread inside next(), and the session's error
    // handling is written for that one caller.
    const auto failure = slot_.takeFailure();
    running_ = false;
    if (error_ && failure) error_(*failure);
    return false;
  }

  void stop() noexcept override {
    running_ = false;
    slot_.stop();
    formatReady_.notify_all();
    teardownPipeWire();
    portal_.close();
    std::lock_guard<std::mutex> lock(mutex_);
    negotiated_ = false;
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
