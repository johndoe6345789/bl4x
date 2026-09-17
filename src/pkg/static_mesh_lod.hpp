#pragma once
// Classic (non-Nanite) FStaticMeshLODResources decode: the fallback
// geometry a minority of BL4 meshes still carry (most are Nanite-only,
// see pkg/nanite.hpp). BL4-pinned byte layout (UE5.5, UseNewCookedFormat,
// HasRayTracingGeometry, no adjacency/wireframe buffers) -- see
// D:\BL4Export\src\cpp\README.md for the full byte-map.
#include <cstdint>
#include <vector>

namespace bl4 {
class Reader;
class Package;
}  // namespace bl4

namespace bl4::classic_mesh {

struct Section {
    uint32_t material_index = 0, first_index = 0, num_triangles = 0;
};

// One LOD's decoded buffers, already resolved to plain float/uint8
// vertex attributes -- SkipLod-equivalent: has_buffers is false when
// this LOD was cooked out (Sections/MaxDeviation only, no geometry).
struct Lod {
    std::vector<Section> sections;
    std::vector<float> positions;      // 3 floats/vertex
    std::vector<float> normals;        // 3 floats/vertex
    std::vector<float> tangents;       // 4 floats/vertex (xyz + bitangent sign)
    std::vector<uint8_t> colors;       // 4 bytes/vertex, RGBA order (source is BGRA)
    std::vector<std::vector<float>> uv_channels;  // NumTexCoords x (2 floats/vertex)
    std::vector<uint32_t> indices;     // triangle list
    uint32_t num_vertices = 0;
    uint32_t num_tex_coords = 0;
    bool has_buffers = false;
};

// Parses one FStaticMeshLODResources starting at its FStripDataFlags.
Lod parse_lod(Reader& r, Package& pkg);

}  // namespace bl4::classic_mesh
