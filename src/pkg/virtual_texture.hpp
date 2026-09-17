#pragma once
// FVirtualTextureBuiltData: BL4's open-world material/terrain textures are
// almost all Virtual Textured (mip_count==0 in the regular per-mip array --
// see pkg/texture.cpp), so their pixel data lives here instead: a tiled
// atlas of small square pages ("tiles"), addressed per mip level through a
// Morton-coded (Z-order) tile grid rather than a flat mip image.
//
// Only decoding one representative mip level's worth of tiles into a plain
// RGBA8 bitmap is supported (matching CUE4Parse's own export behavior) --
// not the full streaming/indirection-table machinery real UE uses at
// runtime, which this offline extractor has no use for.
#include <cstdint>
#include <string>
#include <vector>

namespace bl4 {

class Package;
class Reader;

struct VtTileOffsetData {
    uint32_t width = 0, height = 0, max_address = 0;
    std::vector<uint32_t> addresses, offsets;
    // ~0u return means "no data at this tile address".
    uint32_t tile_offset(uint32_t address) const;
};

struct VtChunk {
    std::vector<uint8_t> data;  // resolved bulk data payload (RawGPU only).
};

struct VtBuiltData {
    uint32_t num_layers = 0, num_mips = 0, width = 0, height = 0;
    uint32_t tile_size = 0, tile_border_size = 0;
    std::vector<std::string> layer_formats;  // EPixelFormat name per layer.
    std::vector<VtChunk> chunks;

    // Modern (UE5) per-mip addressing.
    std::vector<uint32_t> chunk_index_per_mip, base_offset_per_mip;
    std::vector<VtTileOffsetData> tile_offset_data;
    std::vector<uint32_t> tile_data_offset_per_layer;

    // Legacy addressing, kept for older content; empty tile_offset_in_chunk
    // means the modern fields above are authoritative.
    std::vector<uint32_t> tile_index_per_chunk, tile_index_per_mip, tile_offset_in_chunk;

    bool is_legacy() const { return !tile_offset_in_chunk.empty(); }
    uint32_t physical_tile_size() const { return tile_size + tile_border_size * 2; }
};

// Parses an FVirtualTextureBuiltData right after the FirstMipToSerialize
// bool that gates it in FTexturePlatformData (see pkg/texture.cpp).
VtBuiltData parse_vt_built_data(Reader& r, Package& pkg);

struct VtDecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, tightly packed.
};

// Decodes one mip level (0 = highest resolution) into a flat RGBA8 bitmap,
// stitching every resident tile at that level and dropping each tile's
// border pixels. Throws if a layer uses a codec/pixel format this tool
// doesn't decode (see pkg/bc_decode.hpp for which BC formats are covered).
VtDecodedImage decode_vt_level(const VtBuiltData& vt, int level);

}  // namespace bl4
