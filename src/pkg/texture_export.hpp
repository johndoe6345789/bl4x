#pragma once
// Writes a loaded texture as a plain 32-bit TGA, for consumers (like the
// SDL3CPlusPlus bl4 package's stb_image loader) that don't read DDS.
// Block-compressed data is decoded with pkg/bc_decode; BC6H/BC7 aren't
// supported there, so those textures are reported as not written.
#include <string>

namespace bl4 {

struct TextureData;

// Uses the largest mip no bigger than max_size on either side (halving
// the largest one on the CPU if none is that small). Returns false,
// writing nothing, if the pixel format can't be decoded.
bool write_texture_tga(const TextureData& tex, const std::string& path, int max_size);

}  // namespace bl4
