#include "pkg/nanite.hpp"

#include <stdexcept>

#include "core/reader.hpp"
#include "pkg/nanite_bits.hpp"
#include "pkg/package.hpp"

namespace bl4::nanite {

void NaniteResources::init_page_cache() {
    size_t n = page_streaming_states.size();
    loaded_.resize(n);
    failed_.assign(n, 0);
    loading_.assign(n, 0);
}

namespace {

// FFixupChunk + FPageDiskHeader + FClusterDiskHeader[] + FPageGPUHeader,
// then every cluster's packed SOA header -- everything needed before any
// bit-packed decode can start (see the Nanite decode spec's §1-4).
void parse_page_structure(Page& page) {
    Reader r(page.bytes);
    uint16_t magic = r.read<uint16_t>();
    if (magic != 0x464Eu) throw std::runtime_error("bad Nanite fixup chunk magic");
    uint16_t fixup_num_clusters = r.read<uint16_t>();
    uint16_t num_hierarchy_fixups = r.read<uint16_t>();
    uint16_t num_cluster_fixups = r.read<uint16_t>();
    r.skip(static_cast<size_t>(num_hierarchy_fixups) * 16);
    r.skip(static_cast<size_t>(num_cluster_fixups) * 8);

    page.page_disk_header_offset = static_cast<int64_t>(r.pos());
    uint32_t num_clusters = r.read<uint32_t>();
    r.read<uint32_t>();  // NumRawFloat4s, unused
    page.num_vertex_refs_page_wide = r.read<uint32_t>();
    r.read<uint32_t>();  // page-level DecodeInfoOffset, unused (clusters carry their own)
    page.strip_bitmask_offset = r.read<uint32_t>();
    page.vertex_ref_bitmask_offset = r.read<uint32_t>();

    if (num_clusters > 256) throw std::runtime_error("too many clusters in Nanite page");
    if (num_clusters != fixup_num_clusters)
        throw std::runtime_error("Nanite page/fixup cluster count mismatch");

    page.disk_headers.resize(num_clusters);
    for (auto& dh : page.disk_headers) {
        dh.index_data_offset = r.read<uint32_t>();
        dh.page_cluster_map_offset = r.read<uint32_t>();
        dh.vertex_ref_data_offset = r.read<uint32_t>();
        dh.low_bytes_offset = r.read<uint32_t>();
        dh.mid_bytes_offset = r.read<uint32_t>();
        dh.high_bytes_offset = r.read<uint32_t>();
        dh.num_vertex_refs = r.read<uint32_t>();
        dh.num_prev_ref_vertices_before_dwords = r.read<uint32_t>();
        dh.num_prev_new_vertices_before_dwords = r.read<uint32_t>();
    }

    page.gpu_page_header_offset = static_cast<int64_t>(r.pos());
    uint32_t packed = r.read<uint32_t>();
    r.skip(12);  // 3 pad uint32
    if (get_bits(packed, 16, 0) != num_clusters)
        throw std::runtime_error("Nanite page GPU header cluster count mismatch");

    int64_t cluster_origin = page.gpu_page_header_offset + 16;
    page.clusters.reserve(num_clusters);
    for (uint32_t i = 0; i < num_clusters; ++i)
        page.clusters.push_back(
            parse_cluster_header(page.bytes, page.gpu_page_header_offset, cluster_origin, num_clusters, i));
}

}  // namespace

Page* NaniteResources::get_page(uint32_t index) {
    if (index >= page_streaming_states.size()) return nullptr;
    if (loaded_[index]) return loaded_[index].get();
    if (failed_[index] || loading_[index]) return nullptr;
    loading_[index] = 1;

    auto page = std::make_unique<Page>();
    bool ok = true;
    try {
        const PageStreamingState& st = page_streaming_states[index];
        const std::vector<uint8_t>& src =
            (index < static_cast<uint32_t>(num_root_pages)) ? root_data : streamable_pages;
        if (static_cast<uint64_t>(st.bulk_offset) + st.bulk_size > src.size())
            throw std::runtime_error("Nanite page bulk range out of bounds");
        page->bytes.assign(src.begin() + st.bulk_offset, src.begin() + st.bulk_offset + st.bulk_size);

        parse_page_structure(*page);

        for (uint32_t i = 0; i < page->clusters.size(); ++i) {
            NaniteCluster& c = page->clusters[i];
            if (!c.is_voxel)
                decode_cluster(c, page->bytes, page->page_disk_header_offset, page->gpu_page_header_offset,
                               page->strip_bitmask_offset, page->vertex_ref_bitmask_offset, i,
                               page->disk_headers[i]);
        }

        Page* page_ptr = page.get();
        for (uint32_t i = 0; i < page->clusters.size(); ++i) {
            resolve_vertex_references(
                page->clusters[i], page_ptr->bytes, page_ptr->page_disk_header_offset,
                page_ptr->num_vertex_refs_page_wide, page_ptr->disk_headers[i],
                [&](uint32_t parent_page_index, uint32_t local_cluster_index) -> const NaniteCluster* {
                    if (parent_page_index == 0) {
                        return local_cluster_index < page_ptr->clusters.size()
                            ? &page_ptr->clusters[local_cluster_index] : nullptr;
                    }
                    uint32_t dep_start = page_streaming_states[index].dependencies_start;
                    uint32_t dep_index = dep_start + (parent_page_index - 1);
                    if (dep_index >= page_dependencies.size()) return nullptr;
                    Page* parent = get_page(page_dependencies[dep_index]);
                    return (parent && local_cluster_index < parent->clusters.size())
                        ? &parent->clusters[local_cluster_index] : nullptr;
                });
        }
    } catch (const std::exception&) {
        ok = false;
    }

    loading_[index] = 0;
    if (!ok) { failed_[index] = 1; return nullptr; }
    loaded_[index] = std::move(page);
    return loaded_[index].get();
}

RawNaniteMesh load_nanite_mesh(Reader& r, Package& pkg) {
    uint8_t global_strip = r.read<uint8_t>();
    r.skip(1);  // class strip flags, unused
    RawNaniteMesh raw;
    if ((global_strip & 2) != 0) return raw;  // audio-visual data stripped: not a Nanite mesh

    NaniteResources res;
    r.read<uint32_t>();  // ResourceFlags, informational only
    int32_t bulk_index = r.read<int32_t>();
    res.streamable_pages = pkg.read_bulk_data(r, bulk_index);
    res.root_data = r.read_tarray<uint8_t>();

    int32_t n_pss = r.read_count();
    res.page_streaming_states.resize(static_cast<size_t>(n_pss));
    for (auto& st : res.page_streaming_states) {
        st.bulk_offset = r.read<uint32_t>();
        st.bulk_size = r.read<uint32_t>();
        r.read<uint32_t>();  // PageSize, unused
        st.dependencies_start = r.read<uint32_t>();
        st.dependencies_num = r.read<uint16_t>();
        r.read<uint8_t>();  // MaxHierarchyDepth, unused
        r.read<uint8_t>();  // Flags (RELATIVE_ENCODING), unused by CPU decode
    }

    int32_t n_hierarchy = r.read_count();
    r.skip(static_cast<size_t>(n_hierarchy) * 208);  // FPackedHierarchyNode: 4x52-byte slices, unused
    r.read_tarray<uint32_t>();                        // HierarchyRootOffsets, unused
    res.page_dependencies = r.read_tarray<uint32_t>();
    r.read_tarray<uint16_t>();  // ImposterAtlas (present, Game < UE5.8), unused

    res.num_root_pages = r.read<int32_t>();
    r.read<int32_t>();   // PositionPrecision (resource-wide fallback; clusters carry their own), unused
    r.read<int32_t>();   // NormalPrecision (>= UE5.2), unused
    r.read<uint32_t>();  // NumInputTriangles, informational
    r.read<uint32_t>();  // NumInputVertices, informational
    r.read<uint16_t>();  // NumInputMeshes (< UE5.6), informational
    raw.num_tex_coords = r.read<uint16_t>();
    r.read<uint32_t>();  // NumClusters (>= UE5.1), informational

    if (res.page_streaming_states.empty()) return raw;
    res.init_page_cache();

    uint32_t vert_offset = 0;
    for (uint32_t p = 0; p < res.page_streaming_states.size(); ++p) {
        Page* page = res.get_page(p);
        if (!page) continue;
        for (auto& c : page->clusters) {
            if (c.is_voxel || c.edge_length >= 0.0f) continue;  // only finest-LOD (leaf) clusters
            uint32_t base = vert_offset;
            for (uint32_t t = 0; t < c.tri_indices.size(); ++t) {
                auto& tri = c.tri_indices[t];
                raw.triangles.push_back({tri[0] + base, tri[1] + base, tri[2] + base, c.material_index_for_tri(t)});
            }
            for (auto& v : c.vertices) raw.vertices.push_back(v);
            vert_offset += static_cast<uint32_t>(c.vertices.size());
        }
    }
    return raw;
}

}  // namespace bl4::nanite
