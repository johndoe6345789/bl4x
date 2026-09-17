#pragma once
// Reads a UTexture2D export's cooked pixel data (CUE4Parse's UTexture/
// UTexture2D/FTexturePlatformData), left compressed (BC1/BC3/BC5/BC7/
// etc.) rather than decoded to raw RGBA -- a GPU can sample these
// formats directly, and it avoids needing a block-decompressor here.
#include <cstdint>
#include <string>
#include <vector>

namespace bl4 {

class Package;
class Usmap;

struct TextureMip {
    int32_t width = 0, height = 0;
    std::vector<uint8_t> data;
};

struct TextureData {
    int32_t width = 0, height = 0;
    std::string pixel_format;  // e.g. "PF_BC7", "PF_DXT1", "PF_B8G8R8A8"
    bool srgb = true;
    std::vector<TextureMip> mips;  // largest first, matching cook order
};

// Throws ParseError/runtime_error (caught per-export by callers) if
// the export's properties or pixel data don't parse, or if no mip has
// its bulk data resident (all-virtual-texture assets aren't supported).
TextureData load_texture(Package& pkg, const Usmap& usmap, uint32_t export_index);

// Writes a single-format DDS: BC1/2/3/4/5 use the legacy FourCC header;
// BC6H/BC7 and uncompressed formats use the DX10 extension header.
// Throws if pixel_format isn't recognized.
void write_dds(const TextureData& tex, const std::string& path);

}  // namespace bl4
