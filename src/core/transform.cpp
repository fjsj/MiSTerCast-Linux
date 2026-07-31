#include "mistercast/transform.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
namespace mistercast {
bool calculateCrop(uint32_t sw, uint32_t sh, const SourceOptions& o,
                   const Modeline& m, CropRect& r, std::string& e) {
  if (!sw || !sh) {
    e = "capture frame has zero dimensions";
    return false;
  }
  uint32_t w = o.width, h = o.height;
  const bool quarterTurn =
      o.rotation == Rotation::CW90 || o.rotation == Rotation::CCW90;
  if (o.crop == CropMode::Full43 || o.crop == CropMode::Full54) {
    // A quarter turn maps source height onto output width, so the crop has to
    // carry the inverse aspect for the rotated result to stay 4:3 or 5:4.
    const uint32_t wide = o.crop == CropMode::Full43 ? 4 : 5;
    const uint32_t tall = o.crop == CropMode::Full43 ? 3 : 4;
    h = sh;
    w = std::min(sw, quarterTurn ? sh * tall / wide : sh * wide / tall);
  } else if (o.crop != CropMode::Custom) {
    // 1x-5x are multiples of the modeline's active area, not of the custom
    // width/height fields, so a 1x crop is pixel-exact for the target mode.
    const uint32_t multiple = static_cast<uint32_t>(o.crop);
    w = std::min(sw, uint32_t(m.hActive) * multiple);
    h = std::min(sh, uint32_t(m.vActive) * multiple);
  }
  w = std::min(w, sw);
  h = std::min(h, sh);
  if (!w || !h) {
    e = "crop is empty";
    return false;
  }
  int64_t x = 0, y = 0;
  switch (o.alignment) {
    case Alignment::Center:
      x = (sw - w) / 2;
      y = (sh - h) / 2;
      break;
    case Alignment::TopLeft:
      break;
    case Alignment::Top:
      x = (sw - w) / 2;
      break;
    case Alignment::TopRight:
      x = sw - w;
      break;
    case Alignment::Right:
      x = sw - w;
      y = (sh - h) / 2;
      break;
    case Alignment::BottomRight:
      x = sw - w;
      y = sh - h;
      break;
    case Alignment::Bottom:
      x = (sw - w) / 2;
      y = sh - h;
      break;
    case Alignment::BottomLeft:
      y = sh - h;
      break;
    case Alignment::Left:
      y = (sh - h) / 2;
      break;
  }
  x += o.xOffset;
  y += o.yOffset;
  // An offset that would push the crop off-screen is clamped back into range
  // rather than refusing to stream, matching the upstream Windows sender.
  x = std::clamp<int64_t>(x, 0, int64_t(sw) - w);
  y = std::clamp<int64_t>(y, 0, int64_t(sh) - h);
  r = {uint32_t(x), uint32_t(y), w, h};
  return true;
}
namespace {
// Nearest-neighbour source index for normalized position p within extent.
uint32_t sampleIndex(double p, uint32_t origin, uint32_t extent) {
  return origin + std::min(extent - 1, uint32_t(p * extent));
}
}  // namespace

bool transformRgb24(const Frame& f, const CropRect& c, const SourceOptions& o,
                    const Modeline& m, uint8_t field, std::vector<uint8_t>& out,
                    std::string& e) {
  if (auto x = m.validate()) {
    e = *x;
    return false;
  }
  if (f.stride < f.width * 4 || f.bgra.size() < size_t(f.stride) * f.height) {
    e = "invalid BGRA frame buffer";
    return false;
  }
  if (!c.width || !c.height || c.x + uint64_t(c.width) > f.width ||
      c.y + uint64_t(c.height) > f.height) {
    e = "crop rectangle lies outside the captured frame";
    return false;
  }
  const bool fieldBuffer = m.interlaced && !o.progressiveInterlaceBuffer;
  uint32_t oh = fieldBuffer ? m.vActive / 2 : m.vActive;
  if (!oh) {
    e = "interlaced active height must be at least two";
    return false;
  }
  // A quarter turn feeds the output column from the source Y axis and the
  // output row from the source X axis; the other rotations keep the axes.
  const bool swapAxes =
      o.rotation == Rotation::CW90 || o.rotation == Rotation::CCW90;
  // Source coordinates are separable per axis, so they are tabulated once per
  // frame instead of recomputing two divisions for every output pixel. Reused
  // across calls on the rendering thread to keep the hot path allocation-free.
  thread_local std::vector<uint32_t> columnSource, rowSource;
  columnSource.resize(m.hActive);
  rowSource.resize(m.vActive);
  for (uint32_t dx = 0; dx < m.hActive; ++dx) {
    const double u = (dx + .5) / m.hActive;
    switch (o.rotation) {
      case Rotation::None:
        columnSource[dx] = sampleIndex(u, c.x, c.width);
        break;
      case Rotation::Flip180:
        columnSource[dx] = sampleIndex(1 - u, c.x, c.width);
        break;
      case Rotation::CW90:
        columnSource[dx] = sampleIndex(1 - u, c.y, c.height);
        break;
      case Rotation::CCW90:
        columnSource[dx] = sampleIndex(u, c.y, c.height);
        break;
    }
  }
  for (uint32_t y = 0; y < m.vActive; ++y) {
    const double v = (y + .5) / m.vActive;
    switch (o.rotation) {
      case Rotation::None:
        rowSource[y] = sampleIndex(v, c.y, c.height);
        break;
      case Rotation::Flip180:
        rowSource[y] = sampleIndex(1 - v, c.y, c.height);
        break;
      case Rotation::CW90:
        rowSource[y] = sampleIndex(v, c.x, c.width);
        break;
      case Rotation::CCW90:
        rowSource[y] = sampleIndex(1 - v, c.x, c.width);
        break;
    }
  }
  out.resize(size_t(m.hActive) * oh * 3);
  uint8_t* d = out.data();
  for (uint32_t dy = 0; dy < oh; ++dy) {
    // Field-buffer mode maps protocol field 0 to display field 1 (and vice
    // versa), so sample the matching source parity. Progressive-buffer mode
    // sends every line and does not select a field index here.
    const uint32_t fullY = fieldBuffer ? dy * 2 + !(field & 1) : dy;
    if (swapAxes) {
      const size_t column = size_t(rowSource[fullY]) * 4;
      for (uint32_t dx = 0; dx < m.hActive; ++dx, d += 3) {
        const uint8_t* p =
            &f.bgra[size_t(columnSource[dx]) * f.stride + column];
        d[0] = p[0];
        d[1] = p[1];
        d[2] = p[2];
      }
    } else {
      const uint8_t* row = &f.bgra[size_t(rowSource[fullY]) * f.stride];
      for (uint32_t dx = 0; dx < m.hActive; ++dx, d += 3) {
        const uint8_t* p = row + size_t(columnSource[dx]) * 4;
        d[0] = p[0];
        d[1] = p[1];
        d[2] = p[2];
      }
    }
  }
  return true;
}

bool transformRgb24(const Frame& f, const SourceOptions& o, const Modeline& m,
                    uint8_t field, std::vector<uint8_t>& out, std::string& e) {
  CropRect c;
  if (!calculateCrop(f.width, f.height, o, m, c, e)) return false;
  return transformRgb24(f, c, o, m, field, out, e);
}
static uint8_t channel(uint32_t p, uint32_t mask) {
  if (!mask) return 0;
  unsigned shift = 0;
  while (((mask >> shift) & 1) == 0) ++shift;
  uint32_t max = mask >> shift;
  return uint8_t(((p & mask) >> shift) * 255 / max);
}
bool normalizeToBgra(const uint8_t* s, size_t n, uint32_t w, uint32_t h,
                     uint32_t stride, uint8_t bpp, uint32_t rm, uint32_t gm,
                     uint32_t bm, bool lsb, Frame& o, std::string& e) {
  unsigned bytes = (bpp + 7) / 8;
  if ((bytes != 2 && bytes != 3 && bytes != 4) ||
      stride < uint64_t(w) * bytes || n < uint64_t(stride) * h) {
    e = "unsupported or truncated X11 pixel buffer";
    return false;
  }
  o.width = w;
  o.height = h;
  o.stride = w * 4;
  o.sequence = 0;
  // Reuses the caller's buffer. A fresh vector per frame cost a 33 MB
  // value-initializing allocation plus its free on every 4K frame; resize() on
  // an already-correctly-sized buffer neither allocates nor zero-fills.
  o.bgra.resize(size_t(w) * h * 4);
  // TrueColor Xorg desktops overwhelmingly use this native BGRX layout. Copying
  // rows directly avoids expanding every pixel of a 4K desktop one channel at
  // a time; the alpha/padding byte is deliberately ignored by the pipeline.
  if (bytes == 4 && lsb && rm == 0x00ff0000 && gm == 0x0000ff00 &&
      bm == 0x000000ff) {
    if (stride == o.stride)
      std::memcpy(o.bgra.data(), s, size_t(o.stride) * h);
    else
      for (uint32_t y = 0; y < h; ++y)
        std::memcpy(o.bgra.data() + size_t(y) * o.stride,
                    s + size_t(y) * stride, size_t(w) * 4);
    return true;
  }
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = s + size_t(y) * stride + x * bytes;
      uint32_t v = 0;
      for (unsigned i = 0; i < bytes; ++i)
        v |= uint32_t(p[lsb ? i : bytes - 1 - i]) << (8 * i);
      auto* d = &o.bgra[(size_t(y) * w + x) * 4];
      d[0] = channel(v, bm);
      d[1] = channel(v, gm);
      d[2] = channel(v, rm);
      d[3] = 255;
    }
  }
  return true;
}
}  // namespace mistercast
