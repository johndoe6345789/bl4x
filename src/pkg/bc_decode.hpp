#pragma once
// Software BC1/BC3/BC4/BC5 (DXT1/DXT5/ATI1/ATI2) block decoders, needed to
// stitch virtual-texture tiles into a plain RGBA8 bitmap (see
// pkg/virtual_texture.hpp) -- tile borders can't be trimmed from data that's
// still block-compressed, so VT assembly has to decode first.
//
// BC6H/BC7 tiles are not decoded (thrown as unsupported): both need large
// partition/endpoint tables that are easy to transcribe wrong with no ground
// truth to diff against, unlike the rest of this codebase's layers. BC1/3/4/5
// cover every VT layer format observed in BL4 so far.
#include <cstdint>
#include <string>
#include <vector>

namespace bl4 {

// True if `format` (an EPixelFormat name, with or without "PF_") is one this
// file can decode.
bool bc_format_supported(const std::string& format);
// Block size in bytes for a supported format (8 for BC1/BC4, 16 for BC3/BC5).
size_t bc_block_bytes(const std::string& format);

// Decodes a block-compressed image (width x height, not necessarily a
// multiple of 4) into tightly packed RGBA8. `data` must hold at least
// ceil(width/4)*ceil(height/4)*bc_block_bytes(format) bytes.
std::vector<uint8_t> decode_bc_image(const uint8_t* data, size_t data_size,
                                      int width, int height,
                                      const std::string& format);

}  // namespace bl4
