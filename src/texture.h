#pragma once

#include "mesh.h"

// Decoded RGBA8 image with a full mip chain, ready for an immutable D3D texture.
struct Image
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t>  pixels;    // all mips back to back
    std::vector<size_t>   offsets;   // byte offset of each mip in `pixels`
    bool cutout = false;             // alpha is mostly 0 or 255: foliage, fences, decals
};

// Decodes the first loadable candidate of `src` (embedded bytes, then paths).
// Formats: whatever WIC supports (PNG, JPEG, TIFF, BMP, GIF, DDS, ...) plus TGA.
// Must be called on a thread with COM initialized.
bool load_image(const TextureSource& src, Image& out);
