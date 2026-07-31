#pragma once
#include "mistercast/types.hpp"
#include <vector>
namespace mistercast {
struct CropRect { uint32_t x{}, y{}, width{}, height{}; };
bool calculateCrop(uint32_t sourceWidth, uint32_t sourceHeight, const SourceOptions&, CropRect&, std::string& error);
bool transformRgb24(const Frame&, const SourceOptions&, const Modeline&, uint8_t field, std::vector<uint8_t>&, std::string& error);
bool normalizeToBgra(const uint8_t* source, size_t size, uint32_t width, uint32_t height, uint32_t stride,
                     uint8_t bitsPerPixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask,
                     bool leastSignificantByteFirst, Frame&, std::string& error);
}
