#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/composite.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>

#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"
#include "x11_display.hpp"
#include "x11_source_catalog.hpp"

namespace mistercast {
namespace {
struct DrawableSnapshot {
  xcb_drawable_t drawable{};
  int16_t x{}, y{};
  uint16_t width{}, height{};
  uint8_t depth{};
  xcb_visualid_t visual{};
};

struct MonitorBinding {
  std::string name;
  Monitor monitor;

  bool refresh(const std::vector<Monitor>& monitors) {
    const auto it = std::find_if(monitors.begin(), monitors.end(),
                                 [&](const auto& candidate) {
                                   return candidate.name == name;
                                 });
    if (it == monitors.end()) return false;
    monitor = *it;
    return true;
  }

  DrawableSnapshot snapshot(const xcb_screen_t& screen) const {
    return {screen.root,
            monitor.x,
            monitor.y,
            monitor.width,
            monitor.height,
            screen.root_depth,
            screen.root_visual};
  }
};

struct WindowBinding {
  xcb_window_t window{};
  bool expired{};
  xcb_pixmap_t pixmap{};
  bool viewable{};
  uint16_t width{}, height{};
  uint8_t depth{};
  xcb_visualid_t visual{};

  void release(xcb_connection_t* connection) {
    if (connection && pixmap) xcb_free_pixmap(connection, pixmap);
    pixmap = 0;
    viewable = false;
  }

  void expire() {
    expired = true;
    pixmap = 0;
    viewable = false;
  }

  bool refresh(xcb_connection_t* connection, bool compositeAvailable) {
    if (expired) return false;
    const auto geometryCookie = xcb_get_geometry(connection, window);
    const auto attributesCookie = xcb_get_window_attributes(connection, window);
    auto* geometry = xcb_get_geometry_reply(connection, geometryCookie, nullptr);
    auto* attributes =
        xcb_get_window_attributes_reply(connection, attributesCookie, nullptr);
    if (!geometry || !attributes ||
        attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
      free(geometry);
      free(attributes);
      release(connection);
      return false;
    }
    const bool resized = geometry->width != width || geometry->height != height;
    const bool remapped = !viewable;
    width = geometry->width;
    height = geometry->height;
    depth = geometry->depth;
    visual = attributes->visual;
    viewable = true;
    if (compositeAvailable && (resized || remapped)) {
      release(connection);
      viewable = true;
      const auto candidate = xcb_generate_id(connection);
      auto* error = xcb_request_check(
          connection, xcb_composite_name_window_pixmap_checked(
                          connection, window, candidate));
      if (!error) pixmap = candidate;
      free(error);
    }
    free(geometry);
    free(attributes);
    return true;
  }

  DrawableSnapshot snapshot() const {
    return {pixmap ? pixmap : window,
            0,
            0,
            width,
            height,
            depth,
            visual};
  }
};
}  // namespace

class X11Capture final : public IVideoCapture {
  X11CaptureOptions options_;
  X11DisplayConnection display_;
  std::variant<std::monostate, MonitorBinding, WindowBinding> source_;
  bool compositePixmapAvailable_{};
  CropRect region_{};
  ErrorCallback error_;
  uint64_t sequence_{};
  std::atomic<bool> running_{false};
  mutable std::mutex mutex_;
  int shmId_{-1};
  uint8_t* shmData_{};
  xcb_shm_seg_t shmSeg_{};
  size_t shmSize_{};
  bool useShm_{};
  bool shmAttempted_{};
  // Pixel layout of the root visual, resolved once at start() instead of
  // rescanning every visual and pixmap format on every captured frame.
  uint32_t redMask_{0xff0000}, greenMask_{0xff00}, blueMask_{0xff};
  uint8_t depth_{}, bitsPerPixel_{32};
  bool lsbFirst_{true};
  uint32_t shmFailures_{};
  uint32_t windowGeometryPoll_{};
  std::optional<std::chrono::steady_clock::time_point> captureFailureSince_;
  static constexpr auto kCaptureFailureTimeout = std::chrono::seconds(2);
  static constexpr uint32_t kMaxShmFailures = 5;
  xcb_connection_t* connection() const { return display_.connection(); }
  xcb_screen_t* screen() const { return display_.screen(); }
  void resolvePixelFormat(uint8_t depth, xcb_visualid_t visual = 0) {
    auto* setup = xcb_get_setup(connection());
    lsbFirst_ = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    depth_ = depth;
    for (auto v = xcb_screen_allowed_depths_iterator(screen()); v.rem;
         xcb_depth_next(&v))
      for (auto q = xcb_depth_visuals_iterator(v.data); q.rem;
           xcb_visualtype_next(&q))
        if (q.data->visual_id == (visual ? visual : screen()->root_visual)) {
          redMask_ = q.data->red_mask;
          greenMask_ = q.data->green_mask;
          blueMask_ = q.data->blue_mask;
        }
    bitsPerPixel_ = 32;
    for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem;
         xcb_format_next(&f))
      if (f.data->depth == depth_) bitsPerPixel_ = f.data->bits_per_pixel;
  }
  DrawableSnapshot snapshot() const {
    if (const auto* monitor = std::get_if<MonitorBinding>(&source_)) {
      // There is no screen while the connection is down, and both
      // selectedGeometry() and recovery are reached in exactly that window when
      // the X server restarts under a monitor capture. An empty snapshot makes
      // recovery treat the geometry as changed, which is what it is.
      if (auto* current = screen()) return monitor->snapshot(*current);
      return {};
    }
    if (const auto* window = std::get_if<WindowBinding>(&source_))
      return window->snapshot();
    return {};
  }
  const char* failureHint() const {
    return std::holds_alternative<WindowBinding>(source_)
               ? "Restore or reselect the shared window."
               : "Check the monitor selection and X11 session.";
  }
  CropRect clampRegion(const CropRect& r) const {
    const auto source = snapshot();
    if (!r.width || !r.height) return {0, 0, source.width, source.height};
    CropRect c;
    c.width = std::min<uint32_t>(r.width, source.width);
    c.height = std::min<uint32_t>(r.height, source.height);
    c.x = std::min<uint32_t>(r.x, source.width - c.width);
    c.y = std::min<uint32_t>(r.y, source.height - c.height);
    return c;
  }
  void closeShm() {
    if (connection() && shmSeg_) xcb_shm_detach(connection(), shmSeg_);
    if (shmData_) shmdt(shmData_);
    if (shmId_ >= 0) shmctl(shmId_, IPC_RMID, nullptr);
    shmId_ = -1;
    shmData_ = nullptr;
    shmSeg_ = 0;
    shmSize_ = 0;
    useShm_ = false;
  }
  void close() {
    closeShm();
    if (auto* window = std::get_if<WindowBinding>(&source_))
      window->release(connection());
    display_.reset();
    compositePixmapAvailable_ = false;
    shmAttempted_ = false;
  }
  bool connect(std::string& error) {
    if (connection()) return true;
    if (!display_.connect(error)) return false;
    if (x11ExtensionPresent(connection(), xcb_composite_id)) {
      auto* version = xcb_composite_query_version_reply(
          connection(), xcb_composite_query_version(connection(), 0, 4),
          nullptr);
      compositePixmapAvailable_ =
          options_.useComposite && version &&
          (version->major_version > 0 || version->minor_version >= 2);
      free(version);
    }
    return true;
  }

  bool setupShm() {
    shmAttempted_ = true;
    if (!options_.useShm) return false;
    // Without MIT-SHM the documented xcb_get_image fallback is the whole point:
    // remote X and some VNC/RDP servers have no SHM at all.
    if (!x11ExtensionPresent(connection(), xcb_shm_id)) return false;
    auto version = xcb_shm_query_version_reply(
        connection(), xcb_shm_query_version(connection()), nullptr);
    if (!version) return false;
    free(version);
    const auto source = snapshot();
    shmSize_ = size_t(source.width) * source.height * 4;
    shmId_ = shmget(IPC_PRIVATE, shmSize_, IPC_CREAT | 0600);
    if (shmId_ >= 0) {
      auto* p = shmat(shmId_, nullptr, 0);
      if (p != reinterpret_cast<void*>(-1)) {
        shmData_ = static_cast<uint8_t*>(p);
        shmSeg_ = xcb_generate_id(connection());
        auto cookie = xcb_shm_attach_checked(connection(), shmSeg_, shmId_, 0);
        auto* attachError = xcb_request_check(connection(), cookie);
        if (!attachError) useShm_ = true;
        free(attachError);
      }
    }
    if (!useShm_) closeShm();
    return useShm_;
  }
  // Re-resolves the selected source after a failed capture or periodic window
  // check. Geometry changes invalidate the drawable and shared segment;
  // recovering here keeps transient changes from ending the session.
  bool recoverLocked() {
    auto* window = std::get_if<WindowBinding>(&source_);
    if (window && window->expired) return false;
    const bool connectionLost = display_.lost();
    // A window ID is scoped to one X server connection and may identify a
    // different client after a server restart. Never rebind it across that
    // boundary; the user must make a fresh privacy-sensitive selection.
    if (connectionLost && window) {
      window->expire();
      close();
      return false;
    }
    const auto previous = snapshot();
    if (connectionLost) close();
    std::string ignored;
    if (!connect(ignored)) return false;
    bool refreshed = false;
    if (window) {
      refreshed = window->refresh(connection(), compositePixmapAvailable_);
    } else if (auto* monitor = std::get_if<MonitorBinding>(&source_)) {
      refreshed =
          monitor->refresh(enumerateX11Monitors(connection(), screen()));
    }
    if (!refreshed) return false;
    const auto current = snapshot();
    region_ = clampRegion(region_);
    const bool geometryChanged =
        current.width != previous.width || current.height != previous.height;
    if (geometryChanged || connectionLost) {
      closeShm();
      setupShm();
    } else if (!shmAttempted_)
      setupShm();
    resolvePixelFormat(current.depth, current.visual);
    return true;
  }
 public:
  explicit X11Capture(X11CaptureOptions options = {}) : options_(options) {}
  ~X11Capture() override { stop(); }
  bool start(const CaptureSource& source, ErrorCallback cb) override {
    std::lock_guard<std::mutex> l(mutex_);
    running_ = false;
    close();
    source_.emplace<std::monostate>();
    shmFailures_ = 0;
    std::string e;
    if (!connect(e)) {
      if (cb) cb({"video", e, "Log into an Xorg session and set DISPLAY."});
      return false;
    }
    if (const auto* window = std::get_if<WindowCaptureSource>(&source)) {
      if (!window->id) {
        if (cb)
          cb({"video", "no window was selected",
              "Choose an open, visible window and try again."});
        return false;
      }
      source_.emplace<WindowBinding>(WindowBinding{xcb_window_t(window->id)});
      if (!recoverLocked()) {
        if (cb)
          cb({"video", "selected window is unavailable",
              "Choose an open, visible window and try again."});
        return false;
      }
    } else {
      const auto monitor = selectX11Monitor(
          enumerateX11Monitors(connection(), screen()),
          std::get<MonitorCaptureSource>(source).name);
      if (!monitor) {
        if (cb)
          cb({"video", "selected monitor is unavailable",
              "Run 'mistercast list-monitors'."});
        return false;
      }
      source_.emplace<MonitorBinding>(MonitorBinding{monitor->name, *monitor});
      const auto selected = snapshot();
      resolvePixelFormat(selected.depth, selected.visual);
      setupShm();
    }
    const auto selected = snapshot();
    region_ = {0, 0, selected.width, selected.height};
    error_ = std::move(cb);
    captureFailureSince_.reset();
    windowGeometryPoll_ = 0;
    running_ = true;
    return true;
  }
  SourceGeometry selectedGeometry() const override {
    std::lock_guard<std::mutex> l(mutex_);
    const auto selected = snapshot();
    return {selected.width, selected.height};
  }
  void setRegion(const CropRect& region) override {
    std::lock_guard<std::mutex> l(mutex_);
    region_ = clampRegion(region);
  }
  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    (void)timeout;
    if (!running_) return false;
    std::lock_guard<std::mutex> l(mutex_);
    // A growing window would otherwise keep producing the old-sized region
    // without an X error. Poll at a low rate so resize tracking does not add a
    // synchronous X11 round-trip to every captured frame.
    if (std::holds_alternative<WindowBinding>(source_) &&
        ++windowGeometryPoll_ >= 30) {
      windowGeometryPoll_ = 0;
      if (!recoverLocked()) return captureFailedLocked();
    }
    if (captureLocked(out)) {
      captureFailureSince_.reset();
      out.sequence = ++sequence_;
      return true;
    }
    // A resize, rotation, replug, or X server restart invalidates the cached
    // geometry and the shared segment. Recover and let the caller retry, so a
    // transient fault costs frames instead of the whole session; only a fault
    // that persists for roughly two seconds is reported as fatal.
    recoverLocked();
    return captureFailedLocked();
  }
  bool captureFailedLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (!captureFailureSince_) captureFailureSince_ = now;
    if (now - *captureFailureSince_ >= kCaptureFailureTimeout) {
      running_ = false;
      if (error_)
        error_({"video", "X11 capture kept failing", failureHint()});
    }
    return false;
  }
  bool captureLocked(Frame& out) {
    if (!connection() || display_.lost()) return false;
    xcb_generic_error_t* xe = nullptr;
    uint8_t* data = nullptr;
    size_t len = 0;
    uint8_t depth = depth_;
    xcb_get_image_reply_t* normal = nullptr;
    xcb_shm_get_image_reply_t* shared = nullptr;
    // Only the crop region is transferred. Pulling the whole monitor and
    // cropping afterwards cost 6 ms per 4K frame where a small crop costs
    // microseconds, and that cost sat in front of every blit.
    const auto selected = snapshot();
    const int16_t x = int16_t(selected.x + region_.x);
    const int16_t y = int16_t(selected.y + region_.y);
    const uint16_t width = uint16_t(region_.width);
    const uint16_t height = uint16_t(region_.height);
    if (useShm_) {
      auto ck =
          xcb_shm_get_image(connection(), selected.drawable, x, y, width, height,
                            ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, shmSeg_, 0);
      shared = xcb_shm_get_image_reply(connection(), ck, &xe);
      if (shared) {
        depth = shared->depth;
        len = std::min<size_t>(shared->size, shmSize_);
        data = shmData_;
        shmFailures_ = 0;
      } else {
        free(xe);
        xe = nullptr;
        // One failed request does not mean MIT-SHM is unusable: a stale region
        // after a resize fails the same way. Fall back for this frame and only
        // settle on the slow path once it keeps failing.
        if (++shmFailures_ >= kMaxShmFailures) closeShm();
      }
    }
    if (!data) {
      auto ck = xcb_get_image(connection(), XCB_IMAGE_FORMAT_Z_PIXMAP,
                              selected.drawable, x, y, width, height, ~0u);
      normal = xcb_get_image_reply(connection(), ck, &xe);
      if (normal) {
        depth = normal->depth;
        len = xcb_get_image_data_length(normal);
        data = xcb_get_image_data(normal);
      }
    }
    if (!data) {
      free(xe);
      free(shared);
      free(normal);
      return false;
    }
    if (depth != depth_) resolvePixelFormat(depth);
    uint32_t stride = height ? uint32_t(len) / height : 0;
    std::string e;
    bool ok =
        normalizeToBgra(data, len, width, height, stride, bitsPerPixel_,
                        redMask_, greenMask_, blueMask_, lsbFirst_, out, e);
    free(shared);
    free(normal);
    return ok;
  }
  void stop() noexcept override {
    running_ = false;
    std::lock_guard<std::mutex> l(mutex_);
    close();
    source_.emplace<std::monostate>();
  }
};
std::unique_ptr<IVideoCapture> makeX11Capture() {
  return std::make_unique<X11Capture>();
}
std::unique_ptr<IVideoCapture> makeX11Capture(X11CaptureOptions options) {
  return std::make_unique<X11Capture>(options);
}
}  // namespace mistercast
