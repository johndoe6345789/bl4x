#pragma once
// One Nanite cluster: its packed (SOA) header, the bit-packed triangle/
// vertex decode, and cross-cluster vertex-reference resolution. See
// D:\BL4Export\src\cpp\README.md for how this fits into the page/
// resource hierarchy above it.
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace bl4::nanite {

struct NaniteVertex {
    bool valid = false;       // filled by Decode (non-ref) or Resolve (ref)
    int32_t raw_pos[3] = {0, 0, 0};
    float pos[3] = {0, 0, 0};
    float normal[3] = {0, 0, 1};
    float tangent[4] = {1, 0, 0, 1};  // xyz + bitangent-sign w
    bool has_tangent = false;
    uint8_t color[4] = {255, 255, 255, 255};
    float uv[4][2] = {};
};

struct MaterialRangeEntry { uint32_t tri_start, tri_length, material_index; };

// One cluster's disk header (FClusterDiskHeader, BL4/UE5.4-5.5 layout):
// all *_offset fields are byte offsets from the page's PageDiskHeader.
struct ClusterDiskHeader {
    uint32_t index_data_offset = 0;
    uint32_t page_cluster_map_offset = 0;
    uint32_t vertex_ref_data_offset = 0;
    uint32_t low_bytes_offset = 0, mid_bytes_offset = 0, high_bytes_offset = 0;
    uint32_t num_vertex_refs = 0;
    uint32_t num_prev_ref_vertices_before_dwords = 0;   // 3x10-bit packed
    uint32_t num_prev_new_vertices_before_dwords = 0;   // 3x10-bit packed
};

struct NaniteCluster {
    uint32_t num_verts = 0, num_tris = 0;
    uint8_t color_min[4] = {0, 0, 0, 0};
    uint32_t color_bits[4] = {0, 0, 0, 0};  // per-channel bit width
    int32_t pos_start[3] = {0, 0, 0};
    int32_t pos_precision = 0;
    uint32_t pos_bits[3] = {0, 0, 0};
    uint32_t normal_precision = 0, tangent_precision = 0;
    float pos_scale = 1.0f;
    float edge_length = 0.0f;  // negative => finest-LOD (leaf) cluster to emit
    int64_t decode_info_offset = 0;  // relative to the page's GPUPageHeader
    bool has_tangents = false;
    uint32_t num_uvs = 0;
    bool color_mode_variable = false;
    bool is_voxel = false;

    bool use_material_table = false;
    int64_t material_table_offset_dwords = 0;
    uint32_t material_table_length = 0;
    uint32_t mat0_index = 0, mat1_index = 0, mat2_index = 0;
    uint32_t mat0_length = 0, mat1_length = 0;
    std::vector<MaterialRangeEntry> material_ranges;  // slow path only

    std::vector<std::array<uint32_t, 3>> tri_indices;  // num_tris entries
    std::vector<NaniteVertex> vertices;                // num_verts entries
    std::vector<uint32_t> group_ref_to_vertex;          // num_vertex_refs entries

    uint32_t material_index_for_tri(uint32_t tri) const;
};

// Parses one cluster's 128-byte packed header, transposed SOA across
// `num_clusters` (address(cluster i, slot k) = cluster_origin + 16*(k*num_clusters+i)),
// plus its material range table if the slow (table) path is used.
NaniteCluster parse_cluster_header(std::span<const uint8_t> page, int64_t gpu_page_header_offset,
                                   int64_t cluster_origin, uint32_t num_clusters,
                                   uint32_t cluster_index);

// Decodes triangle indices and every non-reference vertex's attributes.
// Reference vertex slots are left invalid (filled later by resolve_vertex_references).
// strip_bitmask_offset/vertex_ref_bitmask_offset are page-wide fields
// from FPageDiskHeader; cluster_index_in_page picks this cluster's slice
// of those page-wide bitmask arrays.
void decode_cluster(NaniteCluster& c, std::span<const uint8_t> page,
                    int64_t page_disk_header_offset, int64_t gpu_page_header_offset,
                    int64_t strip_bitmask_offset, int64_t vertex_ref_bitmask_offset,
                    uint32_t cluster_index_in_page, const ClusterDiskHeader& disk_header);

// Fills every reference vertex slot by copying from a source cluster's
// already-decoded vertex (same page or a page this cluster depends on).
// `find_source` resolves (parent_page_index [0 = same page], local
// cluster index) -> that cluster, already Decode()d (and, if it itself
// has refs, already resolved) -- the caller is responsible for that
// ordering (see pkg/nanite.cpp's page/resource orchestration).
void resolve_vertex_references(
    NaniteCluster& c, std::span<const uint8_t> page, int64_t page_disk_header_offset,
    uint32_t page_wide_num_vertex_refs, const ClusterDiskHeader& disk_header,
    const std::function<const NaniteCluster*(uint32_t parent_page_index, uint32_t local_cluster_index)>& find_source);

}  // namespace bl4::nanite
