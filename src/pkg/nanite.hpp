#pragma once
// FNaniteResources: BL4's primary mesh geometry format (classic per-
// vertex buffers are usually absent -- see pkg/mesh.cpp). Parses the
// resource header, loads every page (root pages inline, streaming pages
// via the shared bulk-data mechanism), decodes every cluster, resolves
// cross-cluster/cross-page vertex references, and flattens the result
// to one leaf-cluster-only vertex/triangle soup (see pkg/nanite_cluster.hpp
// for the per-cluster decode, and D:\BL4Export\src\cpp\README.md for the
// page/cluster hierarchy overview).
#include <cstdint>
#include <memory>
#include <vector>

#include "pkg/nanite_cluster.hpp"

namespace bl4 {
class Package;
class Reader;
}  // namespace bl4

namespace bl4::nanite {

struct PageStreamingState {
    uint32_t bulk_offset = 0, bulk_size = 0;
    uint32_t dependencies_start = 0;
    uint16_t dependencies_num = 0;
};

// One decoded page: its own byte buffer plus every cluster's header,
// decoded (non-reference) vertices, and reference-resolved vertices.
struct Page {
    std::vector<uint8_t> bytes;
    int64_t page_disk_header_offset = 0, gpu_page_header_offset = 0;
    int64_t strip_bitmask_offset = 0, vertex_ref_bitmask_offset = 0;
    uint32_t num_vertex_refs_page_wide = 0;
    std::vector<ClusterDiskHeader> disk_headers;
    std::vector<NaniteCluster> clusters;
};

// Flattened, leaf-cluster-only (EdgeLength < 0) geometry: one vertex
// array (duplicated per contributing cluster, matching CUE4Parse's own
// exporter -- see MeshLodDto.NaniteClusters.cs) and a triangle list
// carrying each triangle's raw (unclamped) material index, since the
// owning UStaticMesh's material count isn't known until later in the
// export's byte stream.
struct RawNaniteMesh {
    std::vector<NaniteVertex> vertices;
    struct Tri { uint32_t v0, v1, v2, material_index; };
    std::vector<Tri> triangles;
    uint32_t num_tex_coords = 0;
};

class NaniteResources {
public:
    std::vector<uint8_t> streamable_pages;
    std::vector<uint8_t> root_data;
    std::vector<PageStreamingState> page_streaming_states;
    std::vector<uint32_t> page_dependencies;
    int32_t num_root_pages = 0;

    void init_page_cache();
    // Loads (if needed), fully decodes, and resolves page `index`,
    // memoized; recurses into dependency pages as needed. Returns
    // nullptr if the index is out of range or decode fails.
    Page* get_page(uint32_t index);

private:
    std::vector<std::unique_ptr<Page>> loaded_;
    std::vector<uint8_t> failed_, loading_;
};

// Reads one FNaniteResources block starting at its FStripDataFlags (the
// very first thing FStaticMeshRenderData's Nanite field reads), and
// returns the fully-decoded leaf geometry. Always consumes exactly the
// right number of bytes so the caller's stream stays aligned, even for
// a non-Nanite mesh (stripped audio-visual data) or an unsupported/
// corrupt resource, in which case the returned mesh is simply empty.
RawNaniteMesh load_nanite_mesh(Reader& r, Package& pkg);

}  // namespace bl4::nanite
