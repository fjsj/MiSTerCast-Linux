#include <sys/ipc.h>
#include <sys/shm.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "mistercast/interfaces.hpp"
#include "mistercast/transform.hpp"
#ifdef MISTERCAST_HAVE_RANDR
#include <xcb/randr.h>
#endif

namespace mistercast {
class X11Capture final : public IVideoCapture {
  xcb_connection_t* connection_{};
  xcb_screen_t* screen_{};
  Monitor selected_;
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
  // Pixel layout of the root visual, resolved once at start() instead of
  // rescanning every visual and pixmap format on every captured frame.
  uint32_t redMask_{0xff0000}, greenMask_{0xff00}, blueMask_{0xff};
  uint8_t depth_{}, bitsPerPixel_{32};
  bool lsbFirst_{true};
  uint32_t consecutiveFailures_{}, shmFailures_{};
  // Roughly two seconds of retries at the capture rate before giving up.
  static constexpr uint32_t kMaxConsecutiveFailures = 120, kMaxShmFailures = 5;
  void resolvePixelFormat(uint8_t depth) {
    auto* setup = xcb_get_setup(connection_);
    lsbFirst_ = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    depth_ = depth;
    for (auto v = xcb_screen_allowed_depths_iterator(screen_); v.rem;
         xcb_depth_next(&v))
      for (auto q = xcb_depth_visuals_iterator(v.data); q.rem;
           xcb_visualtype_next(&q))
        if (q.data->visual_id == screen_->root_visual) {
          redMask_ = q.data->red_mask;
          greenMask_ = q.data->green_mask;
          blueMask_ = q.data->blue_mask;
        }
    bitsPerPixel_ = 32;
    for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem;
         xcb_format_next(&f))
      if (f.data->depth == depth_) bitsPerPixel_ = f.data->bits_per_pixel;
  }
  CropRect clampRegion(const CropRect& r) const {
    if (!r.width || !r.height) return {0, 0, selected_.width, selected_.height};
    CropRect c;
    c.width = std::min<uint32_t>(r.width, selected_.width);
    c.height = std::min<uint32_t>(r.height, selected_.height);
    c.x = std::min<uint32_t>(r.x, selected_.width - c.width);
    c.y = std::min<uint32_t>(r.y, selected_.height - c.height);
    return c;
  }
  void closeShm() {
    if (connection_ && shmSeg_) xcb_shm_detach(connection_, shmSeg_);
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
    if (connection_) {
      xcb_disconnect(connection_);
      connection_ = nullptr;
      screen_ = nullptr;
    }
  }
  bool connect(std::string& error) {
    if (connection_) return true;
    const char* d = std::getenv("DISPLAY");
    if (!d || !*d) {
      error = "X11/Xorg required: DISPLAY is not set";
      return false;
    }
    int index = 0;
    connection_ = xcb_connect(nullptr, &index);
    if (!connection_ || xcb_connection_has_error(connection_)) {
      close();
      error =
          "cannot connect to X11 display; native Wayland capture is not "
          "supported";
      return false;
    }
    auto it = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    while (index-- && it.rem) xcb_screen_next(&it);
    screen_ = it.data;
    if (!screen_) {
      close();
      error = "X11 display has no screen";
      return false;
    }
    return true;
  }

  bool setupShm() {
    auto version = xcb_shm_query_version_reply(
        connection_, xcb_shm_query_version(connection_), nullptr);
    if (!version) return false;
    free(version);
    shmSize_ = size_t(selected_.width) * selected_.height * 4;
    shmId_ = shmget(IPC_PRIVATE, shmSize_, IPC_CREAT | 0600);
    if (shmId_ >= 0) {
      auto* p = shmat(shmId_, nullptr, 0);
      if (p != reinterpret_cast<void*>(-1)) {
        shmData_ = static_cast<uint8_t*>(p);
        shmSeg_ = xcb_generate_id(connection_);
        auto cookie = xcb_shm_attach_checked(connection_, shmSeg_, shmId_, 0);
        auto* attachError = xcb_request_check(connection_, cookie);
        if (!attachError) useShm_ = true;
        free(attachError);
      }
    }
    if (!useShm_) closeShm();
    return useShm_;
  }
  // Re-resolves the display and the selected monitor after a failed capture. A
  // monitor can be resized, rotated, or replugged mid-stream, which invalidates
  // the cached geometry and the shared segment; recovering here keeps the
  // stream alive instead of ending the session on a transient fault.
  bool recoverLocked() {
    if (connection_ && xcb_connection_has_error(connection_)) close();
    std::string ignored;
    auto ms = monitorsLocked(ignored);
    auto it = std::find_if(ms.begin(), ms.end(), [&](const Monitor& m) {
      return m.name == selected_.name;
    });
    if (it == ms.end()) return false;
    const bool moved = it->width != selected_.width ||
                       it->height != selected_.height || it->x != selected_.x ||
                       it->y != selected_.y;
    selected_ = *it;
    region_ = clampRegion(region_);
    if (moved || !useShm_ || !shmData_) {
      closeShm();
      setupShm();
      resolvePixelFormat(screen_ ? screen_->root_depth : depth_);
    }
    return true;
  }
  std::vector<Monitor> monitorsLocked(std::string& error) {
    if (!connect(error)) return {};
    std::vector<Monitor> result;
#ifdef MISTERCAST_HAVE_RANDR
    auto ck = xcb_randr_get_monitors(connection_, screen_->root, 1);
    auto* r = xcb_randr_get_monitors_reply(connection_, ck, nullptr);
    if (r) {
      auto it = xcb_randr_get_monitors_monitors_iterator(r);
      for (; it.rem; xcb_randr_monitor_info_next(&it)) {
        auto* m = it.data;
        auto nc = xcb_get_atom_name(connection_, m->name);
        auto* nr = xcb_get_atom_name_reply(connection_, nc, nullptr);
        std::string name = nr ? std::string(xcb_get_atom_name_name(nr),
                                            xcb_get_atom_name_name_length(nr))
                              : "monitor";
        free(nr);
        result.push_back(
            {name, m->x, m->y, m->width, m->height, bool(m->primary)});
      }
      free(r);
    }
#endif
    if (result.empty())
      result.push_back({"X11-screen-0", 0, 0, screen_->width_in_pixels,
                        screen_->height_in_pixels, true});
    return result;
  }

 public:
  ~X11Capture() override { stop(); }
  std::vector<Monitor> monitors(std::string& error) override {
    std::lock_guard<std::mutex> l(mutex_);
    return monitorsLocked(error);
  }
  bool start(const std::string& name, ErrorCallback cb) override {
    std::string e;
    auto ms = monitors(e);
    if (ms.empty()) {
      if (cb) cb({"video", e, "Log into an Xorg session and set DISPLAY."});
      return false;
    }
    auto it = name.empty() ? std::find_if(ms.begin(), ms.end(),
                                          [](auto& m) { return m.primary; })
                           : std::find_if(ms.begin(), ms.end(), [&](auto& m) {
                               return m.name == name;
                             });
    if (it == ms.end()) {
      if (cb)
        cb({"video", "selected monitor is unavailable",
            "Run 'mistercast list-monitors'."});
      return false;
    }
    selected_ = *it;
    region_ = {0, 0, selected_.width, selected_.height};
    error_ = std::move(cb);
    resolvePixelFormat(screen_->root_depth);
    setupShm();
    consecutiveFailures_ = 0;
    running_ = true;
    return true;
  }
  Monitor selected() const override {
    std::lock_guard<std::mutex> l(mutex_);
    return selected_;
  }
  void setRegion(const CropRect& region) override {
    std::lock_guard<std::mutex> l(mutex_);
    region_ = clampRegion(region);
  }
  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    (void)timeout;
    if (!running_) return false;
    std::lock_guard<std::mutex> l(mutex_);
    if (captureLocked(out)) {
      consecutiveFailures_ = 0;
      out.sequence = ++sequence_;
      return true;
    }
    // A resize, rotation, replug, or X server restart invalidates the cached
    // geometry and the shared segment. Recover and let the caller retry, so a
    // transient fault costs frames instead of the whole session; only a fault
    // that persists for roughly two seconds is reported as fatal.
    recoverLocked();
    if (++consecutiveFailures_ >= kMaxConsecutiveFailures) {
      running_ = false;
      if (error_)
        error_({"video", "X11 capture kept failing",
                "Check the monitor selection and X11 session."});
    }
    return false;
  }
  bool captureLocked(Frame& out) {
    if (!connection_ || xcb_connection_has_error(connection_)) return false;
    xcb_generic_error_t* xe = nullptr;
    uint8_t* data = nullptr;
    size_t len = 0;
    uint8_t depth = depth_;
    xcb_get_image_reply_t* normal = nullptr;
    xcb_shm_get_image_reply_t* shared = nullptr;
    // Only the crop region is transferred. Pulling the whole monitor and
    // cropping afterwards cost 6 ms per 4K frame where a small crop costs
    // microseconds, and that cost sat in front of every blit.
    const int16_t x = int16_t(selected_.x + region_.x);
    const int16_t y = int16_t(selected_.y + region_.y);
    const uint16_t width = uint16_t(region_.width);
    const uint16_t height = uint16_t(region_.height);
    if (useShm_) {
      auto ck =
          xcb_shm_get_image(connection_, screen_->root, x, y, width, height,
                            ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, shmSeg_, 0);
      shared = xcb_shm_get_image_reply(connection_, ck, &xe);
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
      auto ck = xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP,
                              screen_->root, x, y, width, height, ~0u);
      normal = xcb_get_image_reply(connection_, ck, &xe);
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
  }
};
std::unique_ptr<IVideoCapture> makeX11Capture() {
  return std::make_unique<X11Capture>();
}
}  // namespace mistercast
