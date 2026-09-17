#include "pkg/virtual_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "core/reader.hpp"
#include "pkg/bc_decode.hpp"
#include "pkg/package.hpp"

namespace bl4 {
namespace {

uint32_t div_round_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

// De-interleaves the even (x) or odd (caller pre-shifts for y) bits of a
// 2D Morton (Z-order) code -- how VT tile addresses map to a 2D tile grid.
uint32_t reverse_morton2(uint32_t x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0F0F0F0Fu;
    x = (x | (x >> 4)) & 0x00FF00FFu;
    x = (x | (x >> 8)) & 0x0000FFFFu;
    return x;
}

VtTileOffsetData get_tile_offset_data(const VtBuiltData& vt, int level) {
    if (!vt.is_legacy()) {
        if (level < 0 || static_cast<size_t>(level) >= vt.tile_offset_data.size())
            throw std::runtime_error("VT mip level out of range");
        return vt.tile_offset_data[static_cast<size_t>(level)];
    }
    uint32_t block_w = div_round_up(vt.width, vt.tile_size);
    uint32_t block_h = div_round_up(vt.height, vt.tile_size);
    size_t next = std::min<size_t>(static_cast<size_t>(level) + 1, vt.num_mips);
    uint32_t max_addr = vt.tile_index_per_mip.at(next);
    VtTileOffsetData t;
    t.width = block_w;
    t.height = block_h;
    t.max_address = std::max(max_addr - vt.tile_index_per_mip.at(static_cast<size_t>(level)), 1u);
    return t;
}

bool is_valid_address(const VtBuiltData& vt, int level, uint32_t address) {
    if (vt.is_legacy()) {
        if (static_cast<uint32_t>(level) >= vt.num_mips) return false;
        uint64_t tile_index = static_cast<uint64_t>(vt.tile_index_per_mip.at(static_cast<size_t>(level))) +
                              static_cast<uint64_t>(address) * vt.num_layers;
        return tile_index < vt.tile_index_per_mip.at(static_cast<size_t>(level) + 1);
    }
    if (static_cast<size_t>(level) >= vt.tile_offset_data.size()) return false;
    uint32_t x = reverse_morton2(address);
    uint32_t y = reverse_morton2(address >> 1);
    const auto& tod = vt.tile_offset_data[static_cast<size_t>(level)];
    return x < tod.width && y < tod.height;
}

// Returns (chunk index, byte offset into that chunk) for one layer's tile
// payload, or offset==~0u if this tile has no data (a fully-mipped-out
// region of a sparsely populated VT page table).
std::pair<int, uint32_t> get_tile_data(const VtBuiltData& vt, int level, uint32_t address,
                                       uint32_t layer_index) {
    if (vt.is_legacy()) {
        uint64_t tile_index = static_cast<uint64_t>(vt.tile_index_per_mip.at(static_cast<size_t>(level))) +
                              static_cast<uint64_t>(address) * vt.num_layers;
        if (tile_index >= vt.tile_index_per_mip.at(static_cast<size_t>(level) + 1))
            return {0, ~0u};
        auto offset_of = [&](uint64_t idx) -> uint32_t {
            for (size_t c = 0; c + 1 < vt.tile_index_per_chunk.size(); ++c)
                if (idx >= vt.tile_index_per_chunk[c] && idx < vt.tile_index_per_chunk[c + 1])
                    return vt.tile_offset_in_chunk.at(idx);
            return vt.tile_offset_in_chunk.at(idx);
        };
        int chunk_index = 0;
        for (size_t c = 0; c + 1 < vt.tile_index_per_chunk.size(); ++c)
            if (tile_index >= vt.tile_index_per_chunk[c] && tile_index < vt.tile_index_per_chunk[c + 1])
                chunk_index = static_cast<int>(c);
        return {chunk_index, offset_of(tile_index + layer_index)};
    }

    if (static_cast<size_t>(level) >= vt.base_offset_per_mip.size() ||
        static_cast<size_t>(level) >= vt.tile_offset_data.size())
        return {0, ~0u};
    int chunk_index = static_cast<size_t>(level) < vt.chunk_index_per_mip.size()
        ? static_cast<int>(vt.chunk_index_per_mip[static_cast<size_t>(level)]) : -1;
    uint32_t base_offset = vt.base_offset_per_mip[static_cast<size_t>(level)];
    uint32_t tile_offset = vt.tile_offset_data[static_cast<size_t>(level)].tile_offset(address);
    if (base_offset == ~0u || tile_offset == ~0u) return {chunk_index, ~0u};
    uint32_t tile_data_size = vt.tile_data_offset_per_layer.empty()
        ? 0 : vt.tile_data_offset_per_layer.back();
    // Matches CUE4Parse: per-layer length isn't used for the RawGPU codec
    // (the only one this tool decodes), only the base+tile offset is.
    (void)layer_index;
    return {chunk_index, base_offset + tile_offset * tile_data_size};
}

size_t packed_tile_bytes(const std::string& format, int pixel_size) {
    if (bc_format_supported(format)) {
        int blocks = div_round_up(static_cast<uint32_t>(pixel_size), 4);
        return static_cast<size_t>(blocks) * blocks * bc_block_bytes(format);
    }
    return static_cast<size_t>(pixel_size) * pixel_size * 4;  // uncompressed formats below.
}

std::vector<uint8_t> decode_packed_tile(const uint8_t* data, size_t size,
                                        int pixel_size, const std::string& format) {
    if (bc_format_supported(format))
        return decode_bc_image(data, size, pixel_size, pixel_size, format);

    size_t n = static_cast<size_t>(pixel_size) * pixel_size;
    if (size < n * 4) throw std::runtime_error("truncated uncompressed VT tile for " + format);
    std::vector<uint8_t> out(n * 4);
    if (format == "B8G8R8A8" || format == "PF_B8G8R8A8") {
        for (size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = data[i * 4 + 2];
            out[i * 4 + 1] = data[i * 4 + 1];
            out[i * 4 + 2] = data[i * 4 + 0];
            out[i * 4 + 3] = data[i * 4 + 3];
        }
    } else if (format == "R8G8B8A8" || format == "PF_R8G8B8A8") {
        std::memcpy(out.data(), data, n * 4);
    } else if (format == "G8" || format == "PF_G8") {
        for (size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = data[i];
            out[i * 4 + 3] = 255;
        }
    } else {
        throw std::runtime_error("unsupported VT layer pixel format: " + format);
    }
    return out;
}

}  // namespace

uint32_t VtTileOffsetData::tile_offset(uint32_t address) const {
    size_t block_index = 0;
    if (!addresses.empty()) {
        bool found = false;
        for (size_t i = 0; i < addresses.size(); ++i) {
            if (addresses[i] > address) {
                block_index = i > 0 ? i - 1 : 0;
                found = true;
                break;
            }
        }
        if (!found) block_index = addresses.size() - 1;
    }
    uint32_t base_offset = offsets.at(block_index);
    if (base_offset == ~0u) return ~0u;
    uint32_t base_address = addresses.at(block_index);
    return base_offset + (address - base_address);
}

VtBuiltData parse_vt_built_data(Reader& r, Package& pkg) {
    VtBuiltData vt;
    r.read_bool();  // bCooked; always true for a shipped build.

    vt.num_layers = r.read<uint32_t>();
    r.read<uint32_t>();  // WidthInBlocks (UDIM; not needed for pixel decode).
    r.read<uint32_t>();  // HeightInBlocks.
    vt.tile_size = r.read<uint32_t>();
    vt.tile_border_size = r.read<uint32_t>();
    vt.tile_data_offset_per_layer = r.read_tarray<uint32_t>();

    vt.num_mips = r.read<uint32_t>();
    vt.width = r.read<uint32_t>();
    vt.height = r.read<uint32_t>();

    vt.chunk_index_per_mip = r.read_tarray<uint32_t>();
    vt.base_offset_per_mip = r.read_tarray<uint32_t>();
    int32_t tod_count = r.read_count();
    vt.tile_offset_data.resize(static_cast<size_t>(tod_count));
    for (auto& t : vt.tile_offset_data) {
        t.width = r.read<uint32_t>();
        t.height = r.read<uint32_t>();
        t.max_address = r.read<uint32_t>();
        t.addresses = r.read_tarray<uint32_t>();
        t.offsets = r.read_tarray<uint32_t>();
    }

    vt.tile_index_per_chunk = r.read_tarray<uint32_t>();
    vt.tile_index_per_mip = r.read_tarray<uint32_t>();
    vt.tile_offset_in_chunk = r.read_tarray<uint32_t>();

    vt.layer_formats.resize(vt.num_layers);
    for (auto& f : vt.layer_formats) f = r.read_fstring();

    r.skip(static_cast<size_t>(vt.num_layers) * 16);  // LayerFallbackColors (unused).

    int32_t chunk_count = r.read_count();
    vt.chunks.resize(static_cast<size_t>(chunk_count));
    for (auto& c : vt.chunks) {
        r.skip(20);  // FSHAHash bulk data hash (unused).
        r.read<uint32_t>();  // SizeInBytes.
        r.read<uint32_t>();  // CodecPayloadSize.
        std::vector<uint8_t> codec_type(vt.num_layers);
        for (uint32_t l = 0; l < vt.num_layers; ++l) {
            codec_type[l] = r.read<uint8_t>();
            r.read<uint32_t>();  // CodecPayloadOffset[l] (only used by deprecated codecs).
        }
        constexpr uint8_t kRawGPU = 4;
        for (uint8_t t : codec_type)
            if (t != kRawGPU)
                throw std::runtime_error("VT chunk uses an unsupported (deprecated) codec");
        int32_t data_index = r.read<int32_t>();
        c.data = pkg.read_bulk_data(r, data_index);
    }
    return vt;
}

VtDecodedImage decode_vt_level(const VtBuiltData& vt, int level) {
    if (level < 0 || static_cast<uint32_t>(level) >= vt.num_mips)
        throw std::runtime_error("VT mip level out of range");
    int tile_size = static_cast<int>(vt.tile_size);
    int tile_border = static_cast<int>(vt.tile_border_size);
    int tile_pixel_size = static_cast<int>(vt.physical_tile_size());
    VtTileOffsetData tod = get_tile_offset_data(vt, level);

    int bitmap_w = static_cast<int>(tod.width) * tile_size;
    int bitmap_h = static_cast<int>(tod.height) * tile_size;
    double max_level = std::ceil(std::log2(static_cast<double>(std::max(tod.width, tod.height))));
    if (tod.max_address > 1 && (max_level == 0.0 || vt.is_legacy())) {
        double base_level = vt.is_legacy() ? max_level
            : std::ceil(std::log2(static_cast<double>(
                  std::max(vt.tile_offset_data.at(0).width, vt.tile_offset_data.at(0).height))));
        int factor = std::max(1, static_cast<int>(
            std::pow(2.0, vt.is_legacy() ? level : level - base_level)));
        bitmap_w /= factor;
        bitmap_h /= factor;
    }

    VtDecodedImage img;
    img.width = bitmap_w;
    img.height = bitmap_h;
    img.rgba.assign(static_cast<size_t>(bitmap_w) * static_cast<size_t>(bitmap_h) * 4, 0);

    for (uint32_t layer = 0; layer < vt.num_layers; ++layer) {
        const std::string& format = vt.layer_formats.at(layer);
        size_t packed_size = packed_tile_bytes(format, tile_pixel_size);

        for (uint32_t addr = 0; addr < tod.max_address; ++addr) {
            if (!is_valid_address(vt, level, addr)) continue;
            int tile_x = static_cast<int>(reverse_morton2(addr)) * tile_size;
            int tile_y = static_cast<int>(reverse_morton2(addr >> 1)) * tile_size;
            auto [chunk_index, tile_start] = get_tile_data(vt, level, addr, layer);
            if (tile_start == ~0u || chunk_index < 0 ||
                static_cast<size_t>(chunk_index) >= vt.chunks.size())
                continue;
            const auto& chunk_data = vt.chunks[static_cast<size_t>(chunk_index)].data;
            if (static_cast<size_t>(tile_start) + packed_size > chunk_data.size()) continue;

            std::vector<uint8_t> tile_rgba = decode_packed_tile(
                chunk_data.data() + tile_start, packed_size, tile_pixel_size, format);

            for (int ty = 0; ty < tile_size; ++ty) {
                int py = tile_y + ty;
                if (py < 0 || py >= bitmap_h) continue;
                int src_row = (ty + tile_border) * tile_pixel_size + tile_border;
                for (int tx = 0; tx < tile_size; ++tx) {
                    int px = tile_x + tx;
                    if (px < 0 || px >= bitmap_w) continue;
                    size_t src = static_cast<size_t>(src_row + tx) * 4;
                    size_t dst = (static_cast<size_t>(py) * bitmap_w + px) * 4;
                    std::memcpy(&img.rgba[dst], &tile_rgba[src], 4);
                }
            }
        }
    }
    return img;
}

}  // namespace bl4
