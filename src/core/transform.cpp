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

struct LinearSample {
  uint32_t first{}, second{};
  uint16_t fraction{};  // 0..256, with eight fractional bits.
};

struct AreaTables {
  std::vector<uint32_t> offsets, indices, weights;
};

struct AxisMapping {
  uint32_t origin{}, extent{};
  bool reversed{};
};

struct TransformMapping {
  AxisMapping horizontal, vertical;
  bool swapped{};
};

TransformMapping transformMapping(const CropRect& crop, Rotation rotation) {
  const AxisMapping x{crop.x, crop.width, false};
  const AxisMapping reverseX{crop.x, crop.width, true};
  const AxisMapping y{crop.y, crop.height, false};
  const AxisMapping reverseY{crop.y, crop.height, true};
  switch (rotation) {
    case Rotation::None:
      return {x, y, false};
    case Rotation::Flip180:
      return {reverseX, reverseY, false};
    case Rotation::CW90:
      return {reverseY, x, true};
    case Rotation::CCW90:
      return {y, reverseX, true};
  }
  return {x, y, false};
}

uint32_t fullOutputRow(uint32_t row, bool fieldBuffer, uint8_t field) {
  return fieldBuffer ? row * 2 + !(field & 1) : row;
}

void buildPointTable(uint32_t destinationExtent, const AxisMapping& source,
                     std::vector<uint32_t>& table) {
  table.resize(destinationExtent);
  for (uint32_t destination = 0; destination < destinationExtent;
       ++destination) {
    const double position = (destination + .5) / destinationExtent;
    table[destination] = sampleIndex(source.reversed ? 1 - position : position,
                                     source.origin, source.extent);
  }
}

LinearSample linearSample(uint32_t destination, uint32_t destinationExtent,
                          uint32_t sourceOrigin, uint32_t sourceExtent,
                          bool reversed) {
  // Centered texel coordinate: (d + 0.5) * source / destination - 0.5.
  const int64_t numerator =
      int64_t(2 * uint64_t(destination) + 1) * sourceExtent -
      destinationExtent;
  const uint64_t denominator = uint64_t(destinationExtent) * 2;
  uint32_t first = 0, second = 0;
  uint16_t fraction = 0;
  if (numerator > 0) {
    first = uint32_t(uint64_t(numerator) / denominator);
    const auto remainder = uint64_t(numerator) % denominator;
    if (first >= sourceExtent - 1) {
      first = second = sourceExtent - 1;
    } else {
      second = first + 1;
      fraction = uint16_t((remainder * 256 + denominator / 2) / denominator);
      if (fraction == 256) {
        first = second;
        second = std::min(second + 1, sourceExtent - 1);
        fraction = 0;
      }
    }
  }
  if (reversed) {
    first = sourceExtent - 1 - first;
    second = sourceExtent - 1 - second;
  }
  return {sourceOrigin + first, sourceOrigin + second, fraction};
}

void buildAreaTables(uint32_t destinationExtent, uint32_t sourceOrigin,
                     uint32_t sourceExtent, bool reversed, AreaTables& table) {
  table.offsets.resize(size_t(destinationExtent) + 1);
  table.indices.clear();
  table.weights.clear();
  // Both source and destination boundaries are represented on a grid whose
  // unit is 1/destinationExtent. Each row's weights therefore sum exactly to
  // sourceExtent, including fractional ratios and enlargement.
  for (uint32_t d = 0; d < destinationExtent; ++d) {
    table.offsets[d] = uint32_t(table.indices.size());
    const uint64_t begin = uint64_t(d) * sourceExtent;
    const uint64_t end = uint64_t(d + 1) * sourceExtent;
    const uint32_t first = uint32_t(begin / destinationExtent);
    const uint32_t last = uint32_t((end - 1) / destinationExtent);
    for (uint32_t source = first; source <= last; ++source) {
      const uint64_t pixelBegin = uint64_t(source) * destinationExtent;
      const uint64_t pixelEnd = uint64_t(source + 1) * destinationExtent;
      const auto overlap = std::min(end, pixelEnd) - std::max(begin, pixelBegin);
      const auto index = reversed ? sourceExtent - 1 - source : source;
      table.indices.push_back(sourceOrigin + index);
      table.weights.push_back(uint32_t(overlap));
    }
  }
  table.offsets[destinationExtent] = uint32_t(table.indices.size());
}

void transformPoint(const Frame& frame, const TransformMapping& mapping,
                    const Modeline& modeline, bool fieldBuffer, uint8_t field,
                    uint32_t outputHeight, std::vector<uint8_t>& output) {
  thread_local std::vector<uint32_t> columns, rows;
  buildPointTable(modeline.hActive, mapping.horizontal, columns);
  buildPointTable(modeline.vActive, mapping.vertical, rows);
  output.resize(size_t(modeline.hActive) * outputHeight * 3);
  uint8_t* destination = output.data();
  for (uint32_t y = 0; y < outputHeight; ++y) {
    const uint32_t fullY = fullOutputRow(y, fieldBuffer, field);
    if (mapping.swapped) {
      const size_t column = size_t(rows[fullY]) * 4;
      for (uint32_t x = 0; x < modeline.hActive; ++x, destination += 3) {
        const uint8_t* pixel =
            &frame.bgra[size_t(columns[x]) * frame.stride + column];
        destination[0] = pixel[0];
        destination[1] = pixel[1];
        destination[2] = pixel[2];
      }
    } else {
      const uint8_t* row = &frame.bgra[size_t(rows[fullY]) * frame.stride];
      for (uint32_t x = 0; x < modeline.hActive; ++x, destination += 3) {
        const uint8_t* pixel = row + size_t(columns[x]) * 4;
        destination[0] = pixel[0];
        destination[1] = pixel[1];
        destination[2] = pixel[2];
      }
    }
  }
}

void transformBilinear(const Frame& frame, const TransformMapping& mapping,
                       const Modeline& modeline, bool fieldBuffer,
                       uint8_t field, uint32_t outputHeight,
                       std::vector<uint8_t>& output) {
  thread_local std::vector<LinearSample> columns, rows;
  columns.resize(modeline.hActive);
  rows.resize(modeline.vActive);
  for (uint32_t x = 0; x < modeline.hActive; ++x)
    columns[x] = linearSample(x, modeline.hActive, mapping.horizontal.origin,
                              mapping.horizontal.extent,
                              mapping.horizontal.reversed);
  for (uint32_t y = 0; y < modeline.vActive; ++y)
    rows[y] = linearSample(y, modeline.vActive, mapping.vertical.origin,
                           mapping.vertical.extent, mapping.vertical.reversed);
  const auto pixel = [&](uint32_t horizontal, uint32_t vertical) {
    const uint32_t sx = mapping.swapped ? vertical : horizontal;
    const uint32_t sy = mapping.swapped ? horizontal : vertical;
    return &frame.bgra[size_t(sy) * frame.stride + size_t(sx) * 4];
  };
  output.resize(size_t(modeline.hActive) * outputHeight * 3);
  uint8_t* destination = output.data();
  for (uint32_t y = 0; y < outputHeight; ++y) {
    const auto& vertical = rows[fullOutputRow(y, fieldBuffer, field)];
    const uint32_t wy1 = vertical.fraction, wy0 = 256 - wy1;
    for (uint32_t x = 0; x < modeline.hActive; ++x, destination += 3) {
      const auto& horizontal = columns[x];
      const uint32_t wx1 = horizontal.fraction, wx0 = 256 - wx1;
      const uint8_t* p00 = pixel(horizontal.first, vertical.first);
      const uint8_t* p10 = pixel(horizontal.second, vertical.first);
      const uint8_t* p01 = pixel(horizontal.first, vertical.second);
      const uint8_t* p11 = pixel(horizontal.second, vertical.second);
      for (unsigned channel = 0; channel < 3; ++channel) {
        const uint32_t sum = uint32_t(p00[channel]) * wx0 * wy0 +
                             uint32_t(p10[channel]) * wx1 * wy0 +
                             uint32_t(p01[channel]) * wx0 * wy1 +
                             uint32_t(p11[channel]) * wx1 * wy1;
        destination[channel] = uint8_t((sum + 32768) >> 16);
      }
    }
  }
}

void transformLineBlend(const Frame& frame, const TransformMapping& mapping,
                        const Modeline& modeline, bool fieldBuffer,
                        uint8_t field, uint32_t outputHeight,
                        std::vector<uint8_t>& output) {
  thread_local std::vector<uint32_t> columns;
  thread_local AreaTables rows;
  thread_local std::vector<size_t> rowOffsets;
  buildPointTable(modeline.hActive, mapping.horizontal, columns);
  buildAreaTables(modeline.vActive, mapping.vertical.origin,
                  mapping.vertical.extent, mapping.vertical.reversed, rows);
  if (!mapping.swapped) {
    rowOffsets.resize(rows.indices.size());
    for (size_t i = 0; i < rows.indices.size(); ++i)
      rowOffsets[i] = size_t(rows.indices[i]) * frame.stride;
  }
  const uint32_t verticalExtent = mapping.vertical.extent;
  const uint64_t reciprocal = (uint64_t{1} << 32) / verticalExtent;
  const auto normalize = [verticalExtent, reciprocal](uint32_t sum) {
    const uint32_t rounded = sum + verticalExtent / 2;
    uint32_t quotient = uint32_t((uint64_t(rounded) * reciprocal) >> 32);
    if (rounded - quotient * verticalExtent >= verticalExtent) ++quotient;
    return uint8_t(quotient);
  };
  output.resize(size_t(modeline.hActive) * outputHeight * 3);
  uint8_t* destination = output.data();
  for (uint32_t y = 0; y < outputHeight; ++y) {
    const uint32_t fullY = fullOutputRow(y, fieldBuffer, field);
    if (!mapping.swapped) {
      for (uint32_t x = 0; x < modeline.hActive; ++x, destination += 3) {
        uint32_t blue = 0, green = 0, red = 0;
        const size_t column = size_t(columns[x]) * 4;
        for (uint32_t i = rows.offsets[fullY]; i < rows.offsets[fullY + 1];
             ++i) {
          const uint8_t* pixel = &frame.bgra[rowOffsets[i] + column];
          blue += uint32_t(pixel[0]) * rows.weights[i];
          green += uint32_t(pixel[1]) * rows.weights[i];
          red += uint32_t(pixel[2]) * rows.weights[i];
        }
        destination[0] = normalize(blue);
        destination[1] = normalize(green);
        destination[2] = normalize(red);
      }
      continue;
    }
    for (uint32_t x = 0; x < modeline.hActive; ++x, destination += 3) {
      uint32_t blue = 0, green = 0, red = 0;
      for (uint32_t i = rows.offsets[fullY]; i < rows.offsets[fullY + 1];
           ++i) {
        const uint32_t sx = rows.indices[i];
        const uint32_t sy = columns[x];
        const uint8_t* pixel =
            &frame.bgra[size_t(sy) * frame.stride + size_t(sx) * 4];
        blue += uint32_t(pixel[0]) * rows.weights[i];
        green += uint32_t(pixel[1]) * rows.weights[i];
        red += uint32_t(pixel[2]) * rows.weights[i];
      }
      destination[0] = normalize(blue);
      destination[1] = normalize(green);
      destination[2] = normalize(red);
    }
  }
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
  const auto mapping = transformMapping(c, o.rotation);
  switch (o.sampling) {
    case SamplingMode::Point:
      transformPoint(f, mapping, m, fieldBuffer, field, oh, out);
      return true;
    case SamplingMode::Bilinear:
      transformBilinear(f, mapping, m, fieldBuffer, field, oh, out);
      return true;
    case SamplingMode::LineBlend:
      transformLineBlend(f, mapping, m, fieldBuffer, field, oh, out);
      return true;
  }
  e = "unsupported sampling mode";
  return false;
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
bool cropToBgra(const uint8_t* s, size_t n, uint32_t sw, uint32_t sh,
                uint32_t stride, PixelOrder order, const CropRect& region,
                Frame& o, std::string& e) {
  if (!s || !sw || !sh || stride < uint64_t(sw) * 4 ||
      n < uint64_t(stride) * (sh - 1) + uint64_t(sw) * 4) {
    e = "unsupported or truncated PipeWire pixel buffer";
    return false;
  }
  const uint32_t w = region.width ? region.width : sw;
  const uint32_t h = region.height ? region.height : sh;
  if (uint64_t(region.x) + w > sw || uint64_t(region.y) + h > sh) {
    e = "crop region falls outside the captured frame";
    return false;
  }
  o.width = w;
  o.height = h;
  o.stride = w * 4;
  o.sequence = 0;
  o.bgra.resize(size_t(w) * h * 4);
  const uint8_t* row = s + size_t(region.y) * stride + size_t(region.x) * 4;
  // BGRA is what the rest of the pipeline consumes, so the negotiated-first
  // layout is a straight row copy and never touches a pixel individually. Only
  // an RGBA producer pays for the channel swap.
  if (order == PixelOrder::Bgra) {
    if (stride == o.stride && !region.x && w == sw)
      std::memcpy(o.bgra.data(), row, size_t(o.stride) * h);
    else
      for (uint32_t y = 0; y < h; ++y)
        std::memcpy(o.bgra.data() + size_t(y) * o.stride,
                    row + size_t(y) * stride, size_t(w) * 4);
    return true;
  }
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* p = row + size_t(y) * stride;
    uint8_t* d = o.bgra.data() + size_t(y) * o.stride;
    for (uint32_t x = 0; x < w; ++x, p += 4, d += 4) {
      d[0] = p[2];
      d[1] = p[1];
      d[2] = p[0];
      d[3] = p[3];
    }
  }
  return true;
}
}  // namespace mistercast
