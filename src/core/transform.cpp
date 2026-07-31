#include "mistercast/transform.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
namespace mistercast {
bool calculateCrop(uint32_t sw, uint32_t sh, const SourceOptions& o,
                   CropRect& r, std::string& e) {
  if (!sw || !sh) {
    e = "capture frame has zero dimensions";
    return false;
  }
  uint32_t w = o.width, h = o.height;
  if (o.crop == CropMode::Full43) {
    h = sh;
    w = std::min(sw, sh * 4 / 3);
  } else if (o.crop == CropMode::Full54) {
    h = sh;
    w = std::min(sw, sh * 5 / 4);
  } else if (o.crop != CropMode::Custom) {
    unsigned m = static_cast<unsigned>(o.crop);
    w = std::min(sw, uint32_t(o.width * m));
    h = std::min(sh, uint32_t(o.height * m));
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
  if (x < 0 || y < 0 || x + int64_t(w) > sw || y + int64_t(h) > sh) {
    e = "crop offsets place the source outside the monitor";
    return false;
  }
  r = {uint32_t(x), uint32_t(y), w, h};
  return true;
}
bool transformRgb24(const Frame& f, const SourceOptions& o, const Modeline& m,
                    uint8_t field, std::vector<uint8_t>& out, std::string& e) {
  if (auto x = m.validate()) {
    e = *x;
    return false;
  }
  if (f.stride < f.width * 4 || f.bgra.size() < size_t(f.stride) * f.height) {
    e = "invalid BGRA frame buffer";
    return false;
  }
  CropRect c;
  if (!calculateCrop(f.width, f.height, o, c, e)) return false;
  const bool fieldBuffer = m.interlaced && !o.progressiveInterlaceBuffer;
  uint32_t oh = fieldBuffer ? m.vActive / 2 : m.vActive;
  if (!oh) {
    e = "interlaced active height must be at least two";
    return false;
  }
  out.resize(size_t(m.hActive) * oh * 3);
  for (uint32_t dy = 0; dy < oh; ++dy) {
    for (uint32_t dx = 0; dx < m.hActive; ++dx) {
      // Field-buffer mode maps protocol field 0 to display field 1 (and vice
      // versa), so sample the matching source parity. Progressive-buffer mode
      // sends every line and does not select a field index here.
      uint32_t fullY = fieldBuffer ? dy * 2 + !(field & 1) : dy;
      double u = (dx + .5) / m.hActive, v = (fullY + .5) / m.vActive, ru = u,
             rv = v;
      switch (o.rotation) {
        case Rotation::None:
          break;
        case Rotation::CW90:
          ru = v;
          rv = 1 - u;
          break;
        case Rotation::CCW90:
          ru = 1 - v;
          rv = u;
          break;
        case Rotation::Flip180:
          ru = 1 - u;
          rv = 1 - v;
          break;
      }
      uint32_t sx = c.x + std::min(c.width - 1, uint32_t(ru * c.width));
      uint32_t sy = c.y + std::min(c.height - 1, uint32_t(rv * c.height));
      const uint8_t* p = &f.bgra[size_t(sy) * f.stride + sx * 4];
      size_t q = (size_t(dy) * m.hActive + dx) * 3;
      out[q] = p[0];
      out[q + 1] = p[1];
      out[q + 2] = p[2];
    }
  }
  return true;
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
  o = {w, h, w * 4, 0, std::vector<uint8_t>(size_t(w) * h * 4)};
  // TrueColor Xorg desktops overwhelmingly use this native BGRX layout. Copying
  // rows directly avoids expanding every pixel of a 4K desktop one channel at
  // a time; the alpha/padding byte is deliberately ignored by the pipeline.
  if (bytes == 4 && lsb && rm == 0x00ff0000 && gm == 0x0000ff00 &&
      bm == 0x000000ff) {
    for (uint32_t y = 0; y < h; ++y)
      std::memcpy(o.bgra.data() + size_t(y) * o.stride, s + size_t(y) * stride,
                  size_t(w) * 4);
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
