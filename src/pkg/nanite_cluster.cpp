#include "pkg/nanite_cluster.hpp"

#include <algorithm>
#include <cstring>

#include "pkg/nanite_bits.hpp"

namespace bl4::nanite {
namespace {

// address(cluster i, slot k) = cluster_origin + 16*(k*num_clusters + i)
// -- clusters are stored transposed (structure-of-arrays) across their
// 8 float4 slots, to speed up GPU transcoding; see FNaniteStreamableData.
uint32_t slot_u32(std::span<const uint8_t> page, int64_t cluster_origin, uint32_t num_clusters,
                  uint32_t cluster_index, int slot, int byte_off) {
    int64_t addr = cluster_origin + 16 * (static_cast<int64_t>(slot) * num_clusters + cluster_index) + byte_off;
    return read_u32_le_at(page, addr);
}

}  // namespace

uint32_t NaniteCluster::material_index_for_tri(uint32_t tri) const {
    if (use_material_table) {
        for (const auto& r : material_ranges)
            if (tri >= r.tri_start && tri < r.tri_start + r.tri_length) return r.material_index;
        return 0xFFFFFFFFu;
    }
    if (tri < mat0_length) return mat0_index;
    if (tri < mat0_length + mat1_length) return mat1_index;
    return mat2_index;
}

NaniteCluster parse_cluster_header(std::span<const uint8_t> page, int64_t gpu_page_header_offset,
                                   int64_t cluster_origin, uint32_t num_clusters,
                                   uint32_t cluster_index) {
    NaniteCluster c;
    auto su = [&](int slot, int off) { return slot_u32(page, cluster_origin, num_clusters, cluster_index, slot, off); };

    // Slot 0: vertex/triangle counts, color minima + bit widths.
    uint32_t A = su(0, 0);
    c.num_verts = get_bits(A, 9, 0);
    uint32_t B = su(0, 4);
    c.num_tris = get_bits(B, 8, 0);
    int64_t color_min_addr = cluster_origin + 16 * (static_cast<int64_t>(cluster_index)) + 8;
    for (int i = 0; i < 4; ++i) c.color_min[i] = static_cast<uint8_t>(byte_at(page, color_min_addr + i));
    uint32_t C = su(0, 12);
    for (uint32_t i = 0; i < 4; ++i) c.color_bits[i] = get_bits(C, 4, i * 4);

    // Slot 1: position quantization.
    c.pos_start[0] = static_cast<int32_t>(su(1, 0));
    c.pos_start[1] = static_cast<int32_t>(su(1, 4));
    c.pos_start[2] = static_cast<int32_t>(su(1, 8));
    uint32_t D = su(1, 12);
    c.pos_precision = static_cast<int32_t>(get_bits(D, 6, 3)) - 20;  // NANITE_MIN_POSITION_PRECISION_504
    c.pos_bits[0] = get_bits(D, 5, 9);
    c.pos_bits[1] = get_bits(D, 5, 14);
    c.pos_bits[2] = get_bits(D, 5, 19);
    c.normal_precision = get_bits(D, 4, 24);
    c.tangent_precision = get_bits(D, 4, 28);
    c.pos_scale = precision_scale(c.pos_precision);

    // Slot 2 (LODBounds) unused. Slot 3: BoxBoundsCenter unused, EdgeLength
    // (half float at byte 14) selects leaf clusters for output.
    {
        int64_t base = cluster_origin + 16 * (3 * static_cast<int64_t>(num_clusters) + cluster_index);
        uint16_t edge_raw = static_cast<uint16_t>(byte_at(page, base + 14) | (byte_at(page, base + 15) << 8));
        c.edge_length = half_to_float(edge_raw);
    }
    // Slot 4 (BoxBoundsExtent, Flags) unused for CPU decode.

    // Slot 5: attribute decode info + material encoding.
    uint32_t F = su(5, 4);
    c.decode_info_offset = static_cast<int64_t>(get_bits(F, 22, 0));
    c.has_tangents = get_bits(F, 1, 22) != 0;
    c.num_uvs = get_bits(F, 3, 24);
    c.color_mode_variable = get_bits(F, 1, 27) != 0;
    uint32_t material_encoding = su(5, 12);

    // Slot 6 (ExtendedData/BrickData) unused: for BL4 (< UE5.6) there are
    // no voxel/brick clusters with real content -- bVoxel reduces to
    // "this cluster has no triangles" (see README's Nanite section).
    c.is_voxel = (c.num_tris == 0);

    // Slot 5 material_encoding tail + slot 7's material table/reuse info.
    constexpr uint32_t kSlowPathThreshold = 0xFE000000u;
    if (material_encoding < kSlowPathThreshold) {
        c.mat0_index = get_bits(material_encoding, 6, 0);
        c.mat1_index = get_bits(material_encoding, 6, 6);
        c.mat2_index = get_bits(material_encoding, 6, 12);
        c.mat0_length = get_bits(material_encoding, 7, 18) + 1;
        c.mat1_length = get_bits(material_encoding, 7, 25);
        c.use_material_table = false;
    } else {
        c.material_table_offset_dwords = static_cast<int64_t>(get_bits(material_encoding, 19, 0));
        c.material_table_length = get_bits(material_encoding, 6, 19) + 1;
        c.mat0_length = 0;
        c.use_material_table = true;
    }

    if (c.use_material_table) {
        int64_t table_addr = gpu_page_header_offset + c.material_table_offset_dwords * 4;
        c.material_ranges.reserve(c.material_table_length);
        for (uint32_t i = 0; i < c.material_table_length; ++i) {
            uint32_t data = read_u32_le_at(page, table_addr + static_cast<int64_t>(i) * 4);
            c.material_ranges.push_back({get_bits(data, 8, 0), get_bits(data, 8, 8), get_bits(data, 6, 16)});
        }
    }

    c.tri_indices.assign(c.num_tris, {0u, 0u, 0u});
    c.vertices.assign(c.num_verts, NaniteVertex{});
    return c;
}

namespace {

// One triangle's 3 vertex indices (already resolved to cluster-local
// vertex slots, not yet canonically rotated) -- FCluster.GetTriangleIndices.
struct TriResult { uint32_t x, y, z; };

TriResult decode_triangle(std::span<const uint8_t> page, int64_t strip_base,
                          int64_t index_data_base, const ClusterDiskHeader& dh, uint32_t tri) {
    uint32_t dword_index = tri >> 5;
    uint32_t bit_index = tri & 31u;
    int64_t dword_addr = strip_base + static_cast<int64_t>(dword_index) * 12;
    uint32_t smask = read_u32_le_at(page, dword_addr + 0);
    uint32_t lmask = read_u32_le_at(page, dword_addr + 4);
    uint32_t wmask = read_u32_le_at(page, dword_addr + 8);
    uint32_t slmask = smask & lmask;
    uint32_t head_ref_vertex_mask = (slmask | ~smask) & wmask;
    uint32_t prev_bits_mask = bit_index == 0 ? 0u : ((1u << bit_index) - 1u);

    uint32_t num_prev_ref_before_dword = dword_index == 0 ? 0u
        : get_bits(dh.num_prev_ref_vertices_before_dwords, 10, (dword_index - 1) * 10);
    uint32_t num_prev_new_before_dword = dword_index == 0 ? 0u
        : get_bits(dh.num_prev_new_vertices_before_dwords, 10, (dword_index - 1) * 10);

    uint32_t cur_ref = 2u * popcount32(slmask & prev_bits_mask) + popcount32(wmask & prev_bits_mask);
    uint32_t cur_new = 2u * popcount32(smask & prev_bits_mask) + bit_index - cur_ref;
    uint32_t num_prev_ref_vertices = num_prev_ref_before_dword + cur_ref;
    uint32_t num_prev_new_vertices = num_prev_new_before_dword + cur_new;

    int32_t is_start = get_bits_as_signed(smask, 1, bit_index);
    int32_t is_left = get_bits_as_signed(lmask, 1, bit_index);
    int32_t is_ref = get_bits_as_signed(wmask, 1, bit_index);
    uint32_t base_vertex = num_prev_new_vertices - 1u;  // wraps to ~0u when 0, matches C#

    int64_t bit_offset = (static_cast<int64_t>(num_prev_ref_vertices) + (~is_start)) * 5;
    uint32_t index_data = read_unaligned_dword(page, index_data_base, bit_offset);

    uint32_t x, y, z;
    if (is_start != 0) {
        int32_t minus_num_ref = (is_left << 1) + is_ref;
        uint32_t next_vertex = num_prev_new_vertices;
        if (minus_num_ref <= -1) { x = base_vertex - (index_data & 31u); index_data >>= 5; } else x = next_vertex++;
        if (minus_num_ref <= -2) { y = base_vertex - (index_data & 31u); index_data >>= 5; } else y = next_vertex++;
        if (minus_num_ref <= -3) { z = base_vertex - (index_data & 31u); } else z = next_vertex++;
    } else {
        uint32_t prev_bit_index = bit_index - 1u;  // wraps if bit_index==0, matches C# uint wrap
        int32_t is_prev_start = get_bits_as_signed(smask, 1, prev_bit_index);
        int32_t is_prev_head_ref = get_bits_as_signed(head_ref_vertex_mask, 1, prev_bit_index);
        uint32_t lw = (get_bits(lmask, 1, prev_bit_index) << 1) | get_bits(wmask, 1, prev_bit_index);
        int32_t num_prev_new_in_tri = is_prev_start & (3 - static_cast<int32_t>(lw));

        y = base_vertex + static_cast<uint32_t>(
            is_prev_head_ref & (num_prev_new_in_tri - static_cast<int32_t>(index_data & 31u)));
        z = num_prev_new_vertices + static_cast<uint32_t>(
            is_ref & (-1 - static_cast<int32_t>(get_bits(index_data, 5, 5))));

        uint32_t search_mask = smask | (lmask ^ static_cast<uint32_t>(is_left));
        uint32_t found_bit_index = first_bit_high(search_mask & prev_bits_mask);
        int32_t is_found_case_s = get_bits_as_signed(smask, 1, found_bit_index);

        uint32_t found_prev_bits_mask = shl32(1u, found_bit_index) - 1u;
        uint32_t found_cur_ref = 2u * popcount32(slmask & found_prev_bits_mask) + popcount32(wmask & found_prev_bits_mask);
        uint32_t found_cur_new = 2u * popcount32(smask & found_prev_bits_mask) + found_bit_index - found_cur_ref;
        uint32_t found_num_prev_new_vertices = num_prev_new_before_dword + found_cur_new;
        uint32_t found_num_prev_ref_vertices = num_prev_ref_before_dword + found_cur_ref;

        uint32_t found_num_ref_vertices = (get_bits(lmask, 1, found_bit_index) << 1) | get_bits(wmask, 1, found_bit_index);
        uint32_t is_before_found_ref_vertex = get_bits(head_ref_vertex_mask, 1, found_bit_index - 1u);

        int32_t read_offset = (is_found_case_s != 0) ? is_left : 1;
        int64_t found_bit_offset = (static_cast<int64_t>(found_num_prev_ref_vertices) - read_offset) * 5;
        uint32_t found_index_data = read_unaligned_dword(page, index_data_base, found_bit_offset);
        uint32_t found_index = (found_num_prev_new_vertices - 1u) - get_bits(found_index_data, 5, 0);

        bool condition = (is_found_case_s != 0)
            ? (static_cast<int32_t>(found_num_ref_vertices) >= 1 - is_left)
            : (is_before_found_ref_vertex != 0);
        uint32_t found_new_vertex = found_num_prev_new_vertices + static_cast<uint32_t>(
            (is_found_case_s != 0) ? (is_left & (found_num_ref_vertices == 0 ? 1 : 0)) : -1);
        x = condition ? found_index : found_new_vertex;
    }

    // Canonical rotation: smallest index leads (preserves winding).
    if (y < std::min(x, z)) { uint32_t t = x; x = y; y = z; z = t; }
    else if (z < std::min(x, y)) { uint32_t t = z; z = y; y = x; x = t; }
    return {x, y, z};
}

struct UvRangeLocal { uint32_t min[2], num_bits[2]; int bytes_per_value; };

}  // namespace

void decode_cluster(NaniteCluster& c, std::span<const uint8_t> page,
                    int64_t page_disk_header_offset, int64_t gpu_page_header_offset,
                    int64_t strip_bitmask_offset, int64_t vertex_ref_bitmask_offset,
                    uint32_t cluster_index_in_page, const ClusterDiskHeader& dh) {
    if (c.is_voxel) return;  // no triangles to decode (see README's Nanite section).

    // ---- Triangle indices (generalized triangle strip) ----
    int64_t strip_base = page_disk_header_offset + strip_bitmask_offset +
                         static_cast<int64_t>(cluster_index_in_page) * 4 * 12;
    int64_t index_data_base = page_disk_header_offset + dh.index_data_offset;
    for (uint32_t tri = 0; tri < c.num_tris; ++tri) {
        TriResult t = decode_triangle(page, strip_base, index_data_base, dh, tri);
        c.tri_indices[tri] = {t.x, t.y, t.z};
    }

    // ---- Reference vs. new vertex classification ----
    int64_t ref_bitmask_addr = page_disk_header_offset + vertex_ref_bitmask_offset +
                               static_cast<int64_t>(cluster_index_in_page) * 32;
    uint32_t group_refs_8888[2] = {0, 0};
    for (uint32_t group_index = 0; group_index < 7; ++group_index) {
        uint32_t count = popcount32(read_u32_le_at(page, ref_bitmask_addr + group_index * 4));
        uint32_t count8888 = count * 0x01010101u;
        uint32_t idx = group_index + 1;
        group_refs_8888[idx >> 2] += shl32(count8888, (idx & 3u) << 3);
        if (c.num_verts > 128 && idx < 4) group_refs_8888[1] += count8888;
    }

    uint32_t num_non_ref_vertices = c.num_verts - dh.num_vertex_refs;
    c.group_ref_to_vertex.assign(dh.num_vertex_refs, 0);
    std::vector<uint32_t> group_non_ref_to_vertex(num_non_ref_vertices, 0);
    for (uint32_t v = 0; v < c.num_verts; ++v) {
        uint32_t d = v >> 5, b = v & 31u;
        uint32_t num_refs_in_prev_dwords = get_bits(group_refs_8888[d >> 2], 8, (d & 3u) * 8);
        uint32_t dword_mask = read_u32_le_at(page, ref_bitmask_addr + static_cast<int64_t>(d) * 4);
        uint32_t num_prev_ref_vertices = popcount32(dword_mask & ((1u << b) - 1u)) + num_refs_in_prev_dwords;
        if (dword_mask & (1u << b)) {
            if (num_prev_ref_vertices < c.group_ref_to_vertex.size())
                c.group_ref_to_vertex[num_prev_ref_vertices] = v;
        } else {
            uint32_t nonref_index = v - num_prev_ref_vertices;
            if (nonref_index < group_non_ref_to_vertex.size()) group_non_ref_to_vertex[nonref_index] = v;
        }
    }

    // ---- Per-UV decode ranges (FUVRange), read from this cluster's own
    // DecodeInfoOffset (relative to the page's GPUPageHeader). ----
    std::vector<UvRangeLocal> uv_ranges(c.num_uvs);
    int64_t uv_range_addr = gpu_page_header_offset + c.decode_info_offset;
    for (uint32_t k = 0; k < c.num_uvs; ++k) {
        uint32_t packed0 = read_u32_le_at(page, uv_range_addr + static_cast<int64_t>(k) * 8);
        uint32_t packed1 = read_u32_le_at(page, uv_range_addr + static_cast<int64_t>(k) * 8 + 4);
        UvRangeLocal r;
        r.min[0] = packed0 >> 5; r.num_bits[0] = packed0 & 31u;
        r.min[1] = packed1 >> 5; r.num_bits[1] = packed1 & 31u;
        r.bytes_per_value = (static_cast<int>(std::max(r.num_bits[0], r.num_bits[1])) + 7) / 8;
        uv_ranges[k] = r;
    }

    // ---- Non-reference vertex attribute decode (LMH byte-plane deltas) ----
    LmhPlanes next{page_disk_header_offset + dh.low_bytes_offset,
                  page_disk_header_offset + dh.mid_bytes_offset,
                  page_disk_header_offset + dh.high_bytes_offset};

    int pos_bpv = (static_cast<int>(std::max({c.pos_bits[0], c.pos_bits[1], c.pos_bits[2]})) + 7) / 8;
    LmhPlanes pos_base = next;
    lmh_advance(next, pos_bpv, 3 * static_cast<int64_t>(num_non_ref_vertices));
    int32_t pos_prev[3] = {
        static_cast<int32_t>(shl32(1u, c.pos_bits[0] - 1u)),
        static_cast<int32_t>(shl32(1u, c.pos_bits[1] - 1u)),
        static_cast<int32_t>(shl32(1u, c.pos_bits[2] - 1u))};
    uint32_t pos_mask[3] = {shl32(1u, c.pos_bits[0]) - 1u, shl32(1u, c.pos_bits[1]) - 1u, shl32(1u, c.pos_bits[2]) - 1u};

    int norm_bpv = (static_cast<int>(c.normal_precision) + 7) / 8;
    LmhPlanes norm_base = next;
    lmh_advance(next, norm_bpv, 2 * static_cast<int64_t>(num_non_ref_vertices));
    int32_t norm_prev[2] = {0, 0};
    uint32_t norm_mask = shl32(1u, c.normal_precision) - 1u;

    int tan_bpv = (static_cast<int>(c.tangent_precision) + 1 + 7) / 8;
    LmhPlanes tan_base = next;
    if (c.has_tangents) lmh_advance(next, tan_bpv, 1 * static_cast<int64_t>(num_non_ref_vertices));
    int32_t tan_prev[1] = {0};
    uint32_t tan_mask = shl32(1u, c.tangent_precision + 1u) - 1u;  // corrected vs. CUE4Parse's operator-precedence bug

    LmhPlanes color_base = next;
    if (c.color_mode_variable) lmh_advance(next, 1, 4 * static_cast<int64_t>(num_non_ref_vertices));
    int32_t color_prev[4] = {0, 0, 0, 0};
    uint32_t color_mask[4] = {shl32(1u, c.color_bits[0]) - 1u, shl32(1u, c.color_bits[1]) - 1u,
                              shl32(1u, c.color_bits[2]) - 1u, shl32(1u, c.color_bits[3]) - 1u};

    std::vector<LmhPlanes> uv_base(c.num_uvs);
    std::vector<std::array<int32_t, 2>> uv_prev(c.num_uvs, {0, 0});
    for (uint32_t k = 0; k < c.num_uvs; ++k) {
        uv_base[k] = next;
        lmh_advance(next, uv_ranges[k].bytes_per_value, 2 * static_cast<int64_t>(num_non_ref_vertices));
    }

    for (uint32_t i = 0; i < num_non_ref_vertices; ++i) {
        uint32_t local_vertex_index = group_non_ref_to_vertex[i];
        NaniteVertex& vert = c.vertices[local_vertex_index];
        vert.valid = true;

        int32_t raw[3];
        lmh_read(page, pos_base, pos_bpv, 3, i, pos_prev, raw);
        for (int a = 0; a < 3; ++a) {
            int32_t v = raw[a] & static_cast<int32_t>(pos_mask[a]);
            vert.raw_pos[a] = v + c.pos_start[a];
            vert.pos[a] = static_cast<float>(vert.raw_pos[a]) * c.pos_scale;
        }

        int32_t norm_raw[2];
        lmh_read(page, norm_base, norm_bpv, 2, i, norm_prev, norm_raw);
        uint32_t n0 = static_cast<uint32_t>(norm_raw[0]) & norm_mask;
        uint32_t n1 = static_cast<uint32_t>(norm_raw[1]) & norm_mask;
        uint32_t packed_normal = n0 | (n1 << c.normal_precision);
        Vec3f n = unpack_octahedral_normal(packed_normal, static_cast<int>(c.normal_precision));
        vert.normal[0] = n.x; vert.normal[1] = n.y; vert.normal[2] = n.z;

        vert.has_tangent = c.has_tangents;
        if (c.has_tangents) {
            int32_t tan_raw[1];
            lmh_read(page, tan_base, tan_bpv, 1, i, tan_prev, tan_raw);
            uint32_t packed_tangent = static_cast<uint32_t>(tan_raw[0]) & tan_mask;
            bool y_sign = (packed_tangent & shl32(1u, c.tangent_precision)) != 0;
            uint32_t angle_bits = get_bits(packed_tangent, c.tangent_precision, 0);
            Vec3f tx = unpack_tangent_x(n, angle_bits, static_cast<int>(c.tangent_precision));
            vert.tangent[0] = tx.x; vert.tangent[1] = tx.y; vert.tangent[2] = tx.z;
            vert.tangent[3] = y_sign ? -1.0f : 1.0f;
        }

        if (c.color_mode_variable) {
            int32_t col_raw[4];
            lmh_read(page, color_base, 1, 4, i, color_prev, col_raw);
            for (int a = 0; a < 4; ++a) {
                uint32_t v = (static_cast<uint32_t>(col_raw[a]) & color_mask[a]) + c.color_min[a];
                vert.color[a] = static_cast<uint8_t>(v & 0xFFu);
            }
        } else {
            for (int a = 0; a < 4; ++a) vert.color[a] = c.color_min[a];
        }

        for (uint32_t k = 0; k < c.num_uvs && k < 4; ++k) {
            int32_t uv_raw[2];
            lmh_read(page, uv_base[k], uv_ranges[k].bytes_per_value, 2, i, uv_prev[k].data(), uv_raw);
            uint32_t gu = (static_cast<uint32_t>(uv_raw[0]) & (shl32(1u, uv_ranges[k].num_bits[0]) - 1u)) + uv_ranges[k].min[0];
            uint32_t gv = (static_cast<uint32_t>(uv_raw[1]) & (shl32(1u, uv_ranges[k].num_bits[1]) - 1u)) + uv_ranges[k].min[1];
            vert.uv[k][0] = decode_uv_float(gu);
            vert.uv[k][1] = decode_uv_float(gv);
        }
    }
}

void resolve_vertex_references(
    NaniteCluster& c, std::span<const uint8_t> page, int64_t page_disk_header_offset,
    uint32_t page_wide_num_vertex_refs, const ClusterDiskHeader& dh,
    const std::function<const NaniteCluster*(uint32_t, uint32_t)>& find_source) {
    int32_t prev_ref_vertex_index = 0;
    for (uint32_t ref_idx = 0; ref_idx < dh.num_vertex_refs; ++ref_idx) {
        uint32_t vertex_index = c.group_ref_to_vertex.at(ref_idx);

        int64_t plane1_addr = page_disk_header_offset + dh.vertex_ref_data_offset + ref_idx;
        uint32_t page_cluster_index = byte_at(page, plane1_addr);

        int64_t map_addr = page_disk_header_offset + dh.page_cluster_map_offset +
                           static_cast<int64_t>(page_cluster_index) * 4;
        uint32_t page_cluster_data = read_u32_le_at(page, map_addr);
        uint32_t parent_page_index = page_cluster_data >> 8;
        uint32_t src_local_cluster_index = page_cluster_data & 0xFFu;

        int64_t plane2_addr = page_disk_header_offset + dh.vertex_ref_data_offset + ref_idx +
                              page_wide_num_vertex_refs;
        uint32_t src_coded = byte_at(page, plane2_addr);
        int32_t temp = decode_zigzag(src_coded) + prev_ref_vertex_index;
        prev_ref_vertex_index = temp;
        uint32_t src_coded_vertex_index = static_cast<uint32_t>(temp) & 0xFFu;

        const NaniteCluster* src_cluster = find_source(parent_page_index, src_local_cluster_index);
        if (!src_cluster || src_coded_vertex_index >= src_cluster->vertices.size()) continue;
        const NaniteVertex& src_vert = src_cluster->vertices[src_coded_vertex_index];
        if (!src_vert.valid) continue;

        NaniteVertex v = src_vert;
        for (int a = 0; a < 3; ++a) v.pos[a] = static_cast<float>(v.raw_pos[a]) * c.pos_scale;
        v.valid = true;
        if (vertex_index < c.vertices.size()) c.vertices[vertex_index] = v;
    }
}

}  // namespace bl4::nanite
