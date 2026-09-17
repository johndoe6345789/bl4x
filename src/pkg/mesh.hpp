#pragma once
// UStaticMesh geometry: classic (position/normal/tangent/UV/color vertex
// buffers + a raw index buffer) and Nanite (bit-packed clusters, decoded
// to the same flat vertex/index representation) both funnel into one
// MeshData -- callers don't need to know which path produced it.
#include <cstdint>
#include <string>
#include <vector>

namespace bl4 {

class Package;
class Usmap;

struct MeshUv { float u = 0, v = 0; };

struct MeshVertex {
    float px = 0, py = 0, pz = 0;
    float nx = 0, ny = 0, nz = 0;
    float tx = 0, ty = 0, tz = 0, tw = 1;  // tangent, w = bitangent sign
    uint8_t r = 255, g = 255, b = 255, a = 255;
    MeshUv uv[4];
};

// One contiguous run of triangles (into MeshData::indices) using one
// material slot (index into MeshData::material_paths).
struct MeshSection {
    uint32_t material_index = 0;
    uint32_t first_index = 0;
    uint32_t num_triangles = 0;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;  // triangle list, 3 per triangle
    std::vector<MeshSection> sections;
    std::vector<std::string> material_paths;  // parallel to section material_index
    uint32_t num_tex_coords = 0;
    bool is_nanite = false;
};

// Reads a UStaticMesh export's full geometry: Nanite clusters if present
// (the common case for BL4), otherwise the classic per-vertex buffers.
// Throws ParseError/runtime_error on anything this tool doesn't decode
// (see pkg/nanite.hpp's scope notes) -- callers should treat a failed
// mesh independently, the same way a failed component doesn't corrupt
// its siblings elsewhere in this tool.
MeshData load_static_mesh(Package& pkg, const Usmap& usmap, uint32_t export_index);

}  // namespace bl4
