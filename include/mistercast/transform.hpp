#pragma once
#include <vector>

#include "mistercast/types.hpp"
namespace mistercast {
bool calculateCrop(uint32_t sourceWidth, uint32_t sourceHeight,
                   const SourceOptions&, const Modeline&, CropRect&,
                   std::string& error);
// Samples an explicit frame-relative crop. Callers that already restricted
// capture to the crop region pass the whole frame and avoid cropping twice.
bool transformRgb24(const Frame&, const CropRect&, const SourceOptions&,
                    const Modeline&, uint8_t field, std::vector<uint8_t>&,
                    std::string& error);
// Derives the crop from the source options, for whole-monitor frames.
bool transformRgb24(const Frame&, const SourceOptions&, const Modeline&,
                    uint8_t field, std::vector<uint8_t>&, std::string& error);
bool normalizeToBgra(const uint8_t* source, size_t size, uint32_t width,
                     uint32_t height, uint32_t stride, uint8_t bitsPerPixel,
                     uint32_t redMask, uint32_t greenMask, uint32_t blueMask,
                     bool leastSignificantByteFirst, Frame&,
                     std::string& error);
}  // namespace mistercast
