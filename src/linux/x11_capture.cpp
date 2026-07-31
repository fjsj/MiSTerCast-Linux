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
  ErrorCallback error_;
  uint64_t sequence_{};
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  int shmId_{-1};
  uint8_t* shmData_{};
  xcb_shm_seg_t shmSeg_{};
  size_t shmSize_{};
  bool useShm_{};
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

 public:
  ~X11Capture() override { stop(); }
  std::vector<Monitor> monitors(std::string& error) override {
    std::lock_guard<std::mutex> l(mutex_);
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
    error_ = std::move(cb);
    auto version = xcb_shm_query_version_reply(
        connection_, xcb_shm_query_version(connection_), nullptr);
    if (version) {
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
    }
    running_ = true;
    return true;
  }
  bool next(Frame& out, std::chrono::milliseconds timeout) override {
    (void)timeout;
    if (!running_) return false;
    std::lock_guard<std::mutex> l(mutex_);
    if (!connection_ || xcb_connection_has_error(connection_)) {
      running_ = false;
      if (error_)
        error_({"video", "X11 session disconnected",
                "Restart after reconnecting the display."});
      return false;
    }
    xcb_generic_error_t* xe = nullptr;
    uint8_t* data = nullptr;
    size_t len = 0;
    uint8_t depth = screen_->root_depth;
    xcb_get_image_reply_t* normal = nullptr;
    xcb_shm_get_image_reply_t* shared = nullptr;
    if (useShm_) {
      auto ck = xcb_shm_get_image(
          connection_, screen_->root, selected_.x, selected_.y, selected_.width,
          selected_.height, ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, shmSeg_, 0);
      shared = xcb_shm_get_image_reply(connection_, ck, &xe);
      if (shared) {
        depth = shared->depth;
        len = std::min<size_t>(shared->size, shmSize_);
        data = shmData_;
      } else {
        free(xe);
        xe = nullptr;
        closeShm();
      }
    }
    if (!data) {
      auto ck = xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP,
                              screen_->root, selected_.x, selected_.y,
                              selected_.width, selected_.height, ~0u);
      normal = xcb_get_image_reply(connection_, ck, &xe);
      if (normal) {
        depth = normal->depth;
        len = xcb_get_image_data_length(normal);
        data = xcb_get_image_data(normal);
      }
    }
    if (!data) {
      std::string m =
          xe ? "X11 capture request failed" : "X11 monitor disappeared";
      free(xe);
      free(shared);
      free(normal);
      if (error_)
        error_({"video", m, "Check monitor selection and X11 session."});
      return false;
    }
    auto* setup = xcb_get_setup(connection_);
    uint32_t rm = 0xff0000, gm = 0xff00, bm = 0xff;
    for (auto it = xcb_setup_roots_iterator(setup); it.rem;
         xcb_screen_next(&it))
      for (auto v = xcb_screen_allowed_depths_iterator(it.data); v.rem;
           xcb_depth_next(&v))
        for (auto q = xcb_depth_visuals_iterator(v.data); q.rem;
             xcb_visualtype_next(&q))
          if (q.data->visual_id == it.data->root_visual) {
            rm = q.data->red_mask;
            gm = q.data->green_mask;
            bm = q.data->blue_mask;
          }
    uint8_t bpp = 32;
    for (auto f = xcb_setup_pixmap_formats_iterator(setup); f.rem;
         xcb_format_next(&f))
      if (f.data->depth == depth) bpp = f.data->bits_per_pixel;
    uint32_t stride = selected_.height ? uint32_t(len) / selected_.height : 0;
    std::string e;
    bool ok = normalizeToBgra(
        data, len, selected_.width, selected_.height, stride, bpp, rm, gm, bm,
        setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST, out, e);
    free(shared);
    free(normal);
    if (ok)
      out.sequence = ++sequence_;
    else if (error_)
      error_({"video", e, "Use a standard TrueColor X11 visual."});
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
