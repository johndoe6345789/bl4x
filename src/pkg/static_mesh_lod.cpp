#include "pkg/static_mesh_lod.hpp"

#include <algorithm>
#include <cstdint>

#include "core/reader.hpp"
#include "pkg/nanite_bits.hpp"  // half_to_float, a general bit-decode leaf utility
#include "pkg/package.hpp"

namespace bl4::classic_mesh {
namespace {

struct PackedVec3f { float x, y, z; };
struct PackedColor { uint8_t b, g, r, a; };  // FColor's on-disk field order

Section read_section(Reader& r) {
    Section s;
    s.material_index = static_cast<uint32_t>(r.read<int32_t>());
    s.first_index = static_cast<uint32_t>(r.read<int32_t>());
    s.num_triangles = static_cast<uint32_t>(r.read<int32_t>());
    r.read<int32_t>();  // MinVertexIndex
    r.read<int32_t>();  // MaxVertexIndex
    r.read<int32_t>();  // bEnableCollision
    r.read<int32_t>();  // bCastShadow
    r.read<int32_t>();  // bForceOpaque (RenderingObjectVersion >= StaticMeshSectionForceOpaqueField)
    r.read<int32_t>();  // bVisibleInRayTracing (StaticMesh.HasVisibleInRayTracing)
    r.read<int32_t>();  // bAffectDistanceFieldLighting (Game >= UE5.1)
    return s;
}

// FPackedNormal (4 signed bytes, XOR 0x80 per byte post-IncreaseNormalPrecision)
// -> 4 floats in [-1,1]. Used for low-precision tangent basis.
void decode_low_tangent(uint32_t raw, float out[4]) {
    uint32_t d = raw ^ 0x80808080u;
    for (int i = 0; i < 4; ++i) {
        int8_t s = static_cast<int8_t>((d >> (8 * i)) & 0xFFu);
        out[i] = std::clamp(static_cast<float>(s) / 127.0f, -1.0f, 1.0f);
    }
}

// FPackedRGBA16N (4 signed uint16, XOR 0x8000) -> 4 floats. High-precision
// tangent basis.
void decode_high_tangent(const uint16_t raw[4], float out[4]) {
    for (int i = 0; i < 4; ++i) {
        uint16_t u = static_cast<uint16_t>(raw[i] ^ 0x8000u);
        int16_t s = static_cast<int16_t>(u);
        out[i] = std::clamp(static_cast<float>(s) / 32767.0f, -1.0f, 1.0f);
    }
}

// FRawStaticIndexBuffer: b32Bit flag, a byte-typed bulk array (element
// size 1, count = total byte length), then a trailing informational bool.
std::vector<uint32_t> read_index_buffer(Reader& r) {
    bool b32 = r.read<int32_t>() != 0;
    r.read<int32_t>();  // ElementSize, always 1 for this byte-typed bulk array
    int32_t byte_count = r.read<int32_t>();
    if (byte_count < 0) throw ParseError("negative index buffer byte count");
    auto bytes = r.read_array<uint8_t>(static_cast<size_t>(byte_count));
    r.read<int32_t>();  // bShouldExpandTo32Bit -- informational, on-disk width is only b32Bit

    std::vector<uint32_t> out;
    if (b32) {
        out.resize(bytes.size() / 4);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = bytes[i * 4] | (bytes[i * 4 + 1] << 8) | (bytes[i * 4 + 2] << 16) | (bytes[i * 4 + 3] << 24);
    } else {
        out.resize(bytes.size() / 2);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<uint32_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
    }
    return out;
}

void skip_weighted_random_sampler(Reader& r) {
    int32_t n = r.read_count();
    r.skip(static_cast<size_t>(n) * 4);
    int32_t m = r.read_count();
    r.skip(static_cast<size_t>(m) * 4);
    r.read<float>();  // TotalWeight
}

// The ~UE4.23+ "new cooked format" buffer block: positions, tangents/UVs,
// vertex color, and index buffers -- see the classic-mesh decode spec's
// §1b/4/5/6 for the exact byte layout this mirrors.
void serialize_buffers(Reader& r, Lod& lod) {
    uint8_t inner_global_strip = r.read<uint8_t>();  // inner FStripDataFlags
    uint8_t inner_class_strip = r.read<uint8_t>();

    r.read<int32_t>();  // Stride (12)
    uint32_t pos_num_vertices = static_cast<uint32_t>(r.read<int32_t>());
    {
        auto raw = r.read_bulk_array<PackedVec3f>();
        lod.positions.resize(raw.size() * 3);
        for (size_t i = 0; i < raw.size(); ++i) {
            lod.positions[i * 3 + 0] = raw[i].x;
            lod.positions[i * 3 + 1] = raw[i].y;
            lod.positions[i * 3 + 2] = raw[i].z;
        }
    }
    lod.num_vertices = pos_num_vertices;

    r.skip(2);  // FStaticMeshVertexBuffer's own FStripDataFlags
    uint32_t num_tex_coords = static_cast<uint32_t>(r.read<int32_t>());
    uint32_t num_vertices = static_cast<uint32_t>(r.read<int32_t>());
    r.read<int32_t>();  // bUseFullPrecisionUVs -- re-derived below from the item size instead
    bool high_precision_tangents_flag = r.read<int32_t>() != 0;
    (void)high_precision_tangents_flag;

    int32_t tan_item_size = r.read<int32_t>();
    int32_t tan_count = r.read<int32_t>();
    lod.normals.assign(static_cast<size_t>(num_vertices) * 3, 0.0f);
    lod.tangents.assign(static_cast<size_t>(num_vertices) * 4, 0.0f);
    for (int32_t i = 0; i < tan_count && static_cast<uint32_t>(i) < num_vertices; ++i) {
        float dtx[4], dtz[4];
        if (tan_item_size == 16) {
            uint16_t tx[4], tz[4];
            for (auto& v : tx) v = r.read<uint16_t>();
            for (auto& v : tz) v = r.read<uint16_t>();
            decode_high_tangent(tx, dtx);
            decode_high_tangent(tz, dtz);
        } else {
            uint32_t tx_raw = r.read<uint32_t>();
            uint32_t tz_raw = r.read<uint32_t>();
            decode_low_tangent(tx_raw, dtx);
            decode_low_tangent(tz_raw, dtz);
        }
        size_t vi = static_cast<size_t>(i);
        lod.normals[vi * 3 + 0] = dtz[0];
        lod.normals[vi * 3 + 1] = dtz[1];
        lod.normals[vi * 3 + 2] = dtz[2];
        lod.tangents[vi * 4 + 0] = dtx[0];
        lod.tangents[vi * 4 + 1] = dtx[1];
        lod.tangents[vi * 4 + 2] = dtx[2];
        lod.tangents[vi * 4 + 3] = dtz[3];  // TangentZ.W = bitangent sign
    }

    int32_t uv_item_size = r.read<int32_t>();
    int32_t uv_count = r.read_count();
    uint32_t tex_coord_num_verts = num_vertices;
    if (num_tex_coords > 0 && static_cast<uint32_t>(uv_count) != num_vertices * num_tex_coords)
        tex_coord_num_verts = num_vertices + ((num_vertices > 0 && (num_tex_coords % 2) != 0) ? 1u : 0u);
    lod.num_tex_coords = num_tex_coords;
    lod.uv_channels.assign(num_tex_coords, std::vector<float>(static_cast<size_t>(num_vertices) * 2, 0.0f));
    for (uint32_t row = 0; row < tex_coord_num_verts; ++row) {
        for (uint32_t ch = 0; ch < num_tex_coords; ++ch) {
            float u, v;
            if (uv_item_size == 8) {
                u = r.read<float>();
                v = r.read<float>();
            } else {
                u = nanite::half_to_float(r.read<uint16_t>());
                v = nanite::half_to_float(r.read<uint16_t>());
            }
            if (row < num_vertices) {
                lod.uv_channels[ch][static_cast<size_t>(row) * 2] = u;
                lod.uv_channels[ch][static_cast<size_t>(row) * 2 + 1] = v;
            }
        }
    }

    r.skip(2);  // FColorVertexBuffer's own FStripDataFlags
    r.read<int32_t>();  // Stride
    uint32_t color_num_vertices = static_cast<uint32_t>(r.read<int32_t>());
    lod.colors.assign(static_cast<size_t>(num_vertices) * 4, 255);
    if (color_num_vertices > 0) {
        auto raw = r.read_bulk_array<PackedColor>();
        for (size_t i = 0; i < raw.size() && i < num_vertices; ++i) {
            lod.colors[i * 4 + 0] = raw[i].r;
            lod.colors[i * 4 + 1] = raw[i].g;
            lod.colors[i * 4 + 2] = raw[i].b;
            lod.colors[i * 4 + 3] = raw[i].a;
        }
    }

    lod.indices = read_index_buffer(r);
    constexpr uint8_t kReversedIndexBufferStripped = 4;
    constexpr uint8_t kRayTracingResourcesStripped = 8;
    constexpr uint8_t kEditorDataStripped = 1;
    if (!(inner_class_strip & kReversedIndexBufferStripped)) read_index_buffer(r);  // ReversedIndexBuffer
    read_index_buffer(r);  // DepthOnlyIndexBuffer, always present
    if (!(inner_class_strip & kReversedIndexBufferStripped)) read_index_buffer(r);  // ReversedDepthOnlyIndexBuffer
    if (!(inner_global_strip & kEditorDataStripped)) read_index_buffer(r);  // WireframeIndexBuffer
    // No AdjacencyIndexBuffer: FUE5ReleaseStreamObjectVersion is past
    // RemovingTessellation for BL4's engine version (always).

    if (!(inner_class_strip & kRayTracingResourcesStripped)) {
        int64_t rt_elem_size = r.read<int32_t>();
        int64_t rt_elem_count = r.read<int32_t>();
        r.skip(static_cast<size_t>(rt_elem_size * rt_elem_count));
    }

    for (size_t i = 0; i < lod.sections.size(); ++i) skip_weighted_random_sampler(r);
    skip_weighted_random_sampler(r);
}

}  // namespace

Lod parse_lod(Reader& r, Package& pkg) {
    Lod lod;
    uint8_t global_strip = r.read<uint8_t>();
    r.read<uint8_t>();  // class strip flags (outer) -- unused at this level

    int32_t num_sections = r.read_count();
    lod.sections.resize(static_cast<size_t>(num_sections));
    for (auto& s : lod.sections) s = read_section(r);

    r.read<float>();  // MaxDeviation
    bool cooked_out = r.read<int32_t>() != 0;
    bool inlined = r.read<int32_t>() != 0;

    bool av_stripped = (global_strip & 2) != 0;
    if (!av_stripped && !cooked_out) {
        r.read<int32_t>();  // bHasRayTracingGeometry (Game >= UE5.5, always present for BL4)
        if (inlined) {
            serialize_buffers(r, lod);
            lod.has_buffers = true;
        } else {
            int32_t data_index = r.read<int32_t>();
            std::vector<uint8_t> bulk = pkg.read_bulk_data(r, data_index);
            if (!bulk.empty()) {
                Reader br(bulk);
                serialize_buffers(br, lod);
                lod.has_buffers = true;
            }
            r.skip(8 + 72);  // mirrored metadata tail (see spec §1a/8b)
        }
        r.read<int32_t>();  // FStaticMeshBuffersSize.SerializedBuffersSize
        r.read<int32_t>();  // .DepthOnlyIBSize
        r.read<int32_t>();  // .ReversedIBsSize
    }
    return lod;
}

}  // namespace bl4::classic_mesh
