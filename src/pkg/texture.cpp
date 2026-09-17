#include "pkg/texture.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "core/reader.hpp"
#include "pkg/package.hpp"
#include "pkg/property.hpp"
#include "pkg/usmap.hpp"
#include "pkg/virtual_texture.hpp"

namespace bl4 {
namespace {

size_t export_start(Package& pkg, uint32_t i) {
    return static_cast<size_t>(pkg.all_export_data_offset()) +
           static_cast<size_t>(pkg.export_at(i).cooked_serial_offset);
}

// UObject's per-instance GUID tail, read right after every property
// block (see pkg/object.cpp for the fuller writeup); duplicated here
// rather than shared, since texture.cpp doesn't otherwise depend on
// object.cpp's tail helpers.
void skip_object_guid_tail(Reader& r, const ExportEntry& e) {
    constexpr uint32_t kClassDefaultObject = 0x10;
    if (e.object_flags & kClassDefaultObject) return;
    if (r.read<int32_t>() != 0) r.skip(16);
}

}  // namespace

TextureData load_texture(Package& pkg, const Usmap& usmap, uint32_t export_index) {
    Reader r(pkg.data());
    r.seek(export_start(pkg, export_index));
    const ExportEntry& e = pkg.export_at(export_index);
    std::string class_name = pkg.resolve_object_name(e.class_index);

    PropertyBag props = read_unversioned_properties(r, usmap, pkg, class_name);
    skip_object_guid_tail(r, e);

    // UTexture's own tail: source-art strip flags, then (if not
    // stripped) the source image -- always stripped in a shipping
    // cook, which is all this tool targets.
    uint8_t global_strip = r.read<uint8_t>();
    r.skip(1);
    if ((global_strip & 1) == 0)
        throw std::runtime_error("texture has un-stripped editor source art (unsupported)");

    // UTexture2D's own properties (ImportedSize/AddressX/AddressY/...)
    // were already folded into `props` above (same property block).
    uint8_t strip2 = r.read<uint8_t>();
    r.skip(1);
    (void)strip2;
    bool cooked = r.read<int32_t>() != 0;
    if (!cooked) throw std::runtime_error("texture has no cooked platform data");
    bool serialize_mip_data = r.read<int32_t>() != 0;  // BL4 is >= UE5.3

    TextureData tex;
    tex.srgb = props.get_bool("SRGB", true);

    for (;;) {
        int32_t name_idx = r.read<int32_t>(), name_extra = r.read<int32_t>();
        std::string format_name = pkg.resolve_local_name(name_idx, name_extra);
        if (format_name == "None") break;
        int64_t base_pos = static_cast<int64_t>(r.pos());
        int64_t skip_offset = base_pos + r.read<int64_t>();
        if (!tex.mips.empty() || !tex.pixel_format.empty()) {
            r.seek(static_cast<size_t>(skip_offset));
            continue;
        }
        tex.pixel_format = format_name;

        // FTexturePlatformData.
        bool using_derived_data = r.read<uint8_t>() != 0;
        r.skip(15);
        if (using_derived_data)
            throw std::runtime_error("texture uses derived-data-cache storage (unsupported)");
        tex.width = r.read<int32_t>();
        tex.height = r.read<int32_t>();
        uint32_t packed = r.read<uint32_t>();
        r.read_fstring();  // PixelFormat string (redundant with format_name)
        bool has_opt_data = (packed & (1u << 30)) != 0;
        bool has_cpu_copy = (packed & (1u << 29)) != 0;
        if (has_opt_data) r.skip(8);
        if (has_cpu_copy) throw std::runtime_error("texture has a CPU copy block (unsupported)");
        int32_t first_mip = r.read<int32_t>();
        (void)first_mip;
        int32_t mip_count = r.read<int32_t>();
        tex.mips.reserve(static_cast<size_t>(mip_count));
        for (int32_t m = 0; m < mip_count; ++m) {
            // FTexture2DMipMap: cooked bulk-data index (if
            // serialize_mip_data), then SizeX/SizeY/SizeZ.
            std::vector<uint8_t> data;
            if (serialize_mip_data) {
                int32_t data_index = r.read<int32_t>();
                try {
                    data = pkg.read_bulk_data(r, data_index);
                } catch (const std::exception& ex) {
                    if (std::getenv("BL4X_TRACE"))
                        std::cerr << "  mip " << m << " data_index=" << data_index
                                  << " failed: " << ex.what() << "\n";
                    // BULKDATA_Unused / zero-size mips (common for the
                    // very top mips of a streamed texture, which the
                    // engine loads from a texture-streaming pool
                    // rather than the base package): leave this mip
                    // empty rather than failing the whole texture.
                }
            }
            TextureMip mip;
            mip.width = r.read<int32_t>();
            mip.height = r.read<int32_t>();
            r.read<int32_t>();  // SizeZ (depth/slices), unused for 2D
            mip.data = std::move(data);
            tex.mips.push_back(std::move(mip));
        }
        // Immediately after the regular mips, UE optionally serializes an
        // FVirtualTextureBuiltData -- BL4's world/material textures almost
        // always take this path (mip_count above is 0 for them; the actual
        // pixel data lives in a tiled, Morton-addressed atlas instead).
        bool is_virtual = r.read_bool();
        if (is_virtual) {
            int32_t lod_bias = static_cast<int32_t>(props.get_double("LODBias", 0));
            VtBuiltData vt = parse_vt_built_data(r, pkg);
            (void)first_mip;
            (void)lod_bias;  // matches CUE4Parse's FirstMipToSerialize-LODBias; unused by our single-level decode.
            VtDecodedImage img = decode_vt_level(vt, 0);
            TextureMip mip;
            mip.width = img.width;
            mip.height = img.height;
            mip.data = std::move(img.rgba);
            tex.mips.clear();
            tex.mips.push_back(std::move(mip));
            tex.pixel_format = "R8G8B8A8";
        }
        // Skip past anything else this format entry has (VT tile data
        // extends well past the summary just parsed) using the file's own
        // recorded end.
        r.seek(static_cast<size_t>(skip_offset));
    }
    if (tex.pixel_format.empty()) throw std::runtime_error("texture has no supported platform data");

    // Keep only mips with actual data, largest first; a streamed
    // texture's top N mips are often not resident in the base package.
    std::vector<TextureMip> resident;
    for (auto& m : tex.mips) if (!m.data.empty()) resident.push_back(std::move(m));
    if (resident.empty()) throw std::runtime_error("texture has no resident mip data");
    tex.mips = std::move(resident);
    tex.width = tex.mips.front().width;
    tex.height = tex.mips.front().height;
    return tex;
}

namespace {

struct FormatInfo { const char* dds_fourcc; uint32_t dxgi; int block; int bytes; bool block_compressed; };

// DXGI_FORMAT values needed below (avoids depending on d3d11.h).
constexpr uint32_t kDXGI_BC4_UNORM = 80;
constexpr uint32_t kDXGI_BC5_UNORM = 83;
constexpr uint32_t kDXGI_BC6H_UF16 = 95;
constexpr uint32_t kDXGI_BC7_UNORM = 98;
constexpr uint32_t kDXGI_B8G8R8A8_UNORM = 87;
constexpr uint32_t kDXGI_R8G8B8A8_UNORM = 28;
constexpr uint32_t kDXGI_R8_UNORM = 61;

bool lookup_format(const std::string& name, FormatInfo& info) {
    auto is = [&](const char* s) { return name == s || name == (std::string("PF_") + s); };
    if (is("DXT1")) { info = {"DXT1", 0, 4, 8, true}; return true; }
    if (is("DXT3")) { info = {"DXT3", 0, 4, 16, true}; return true; }
    if (is("DXT5")) { info = {"DXT5", 0, 4, 16, true}; return true; }
    if (is("BC4")) { info = {nullptr, kDXGI_BC4_UNORM, 4, 8, true}; return true; }
    if (is("BC5")) { info = {nullptr, kDXGI_BC5_UNORM, 4, 16, true}; return true; }
    if (is("BC6H")) { info = {nullptr, kDXGI_BC6H_UF16, 4, 16, true}; return true; }
    if (is("BC7")) { info = {nullptr, kDXGI_BC7_UNORM, 4, 16, true}; return true; }
    if (is("B8G8R8A8")) { info = {nullptr, kDXGI_B8G8R8A8_UNORM, 1, 4, false}; return true; }
    if (is("R8G8B8A8")) { info = {nullptr, kDXGI_R8G8B8A8_UNORM, 1, 4, false}; return true; }
    if (is("G8")) { info = {nullptr, kDXGI_R8_UNORM, 1, 1, false}; return true; }
    return false;
}

}  // namespace

void write_dds(const TextureData& tex, const std::string& path) {
    FormatInfo fmt;
    if (!lookup_format(tex.pixel_format, fmt))
        throw std::runtime_error("unsupported pixel format for DDS: " + tex.pixel_format);

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot create " + path);

    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x1000;  // CAPS|HEIGHT|WIDTH|PIXELFORMAT
    uint32_t pitch_or_linear = 0;
    if (fmt.block_compressed) {
        flags |= 0x80000;  // LINEARSIZE
        uint32_t blocks_w = (static_cast<uint32_t>(tex.width) + fmt.block - 1) / fmt.block;
        uint32_t blocks_h = (static_cast<uint32_t>(tex.height) + fmt.block - 1) / fmt.block;
        pitch_or_linear = blocks_w * blocks_h * static_cast<uint32_t>(fmt.bytes);
    } else {
        flags |= 0x8;  // PITCH
        pitch_or_linear = static_cast<uint32_t>(tex.width) * static_cast<uint32_t>(fmt.bytes);
    }
    uint32_t mip_count = static_cast<uint32_t>(tex.mips.size());
    uint32_t caps = 0x1000 | (mip_count > 1 ? (0x8 | 0x400000) : 0);  // TEXTURE | (COMPLEX|MIPMAP)

    uint8_t header[128] = {};
    std::memcpy(header, "DDS ", 4);
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(header + off, &v, 4); };
    put32(4, 124);            // dwSize
    put32(8, flags);
    put32(12, static_cast<uint32_t>(tex.height));
    put32(16, static_cast<uint32_t>(tex.width));
    put32(20, pitch_or_linear);
    put32(28, mip_count);
    // pixel format block at offset 76, size 32
    put32(76, 32);
    put32(80, fmt.dds_fourcc ? 0x4 : 0x40);  // FOURCC or RGB
    if (fmt.dds_fourcc) {
        std::memcpy(header + 84, fmt.dds_fourcc, 4);
    } else {
        std::memcpy(header + 84, "DX10", 4);
    }
    put32(108, caps);

    out.write(reinterpret_cast<const char*>(header), 128);
    if (!fmt.dds_fourcc) {
        uint8_t dx10[20] = {};
        auto put32b = [&](size_t off, uint32_t v) { std::memcpy(dx10 + off, &v, 4); };
        put32b(0, fmt.dxgi);
        put32b(4, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        put32b(8, 0);   // miscFlag
        put32b(12, 1);  // arraySize
        put32b(16, 0);  // miscFlags2 (alpha mode unknown)
        out.write(reinterpret_cast<const char*>(dx10), 20);
    }
    for (auto& mip : tex.mips) out.write(reinterpret_cast<const char*>(mip.data.data()),
                                         static_cast<std::streamsize>(mip.data.size()));
}

}  // namespace bl4
