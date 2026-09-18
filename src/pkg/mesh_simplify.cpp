#include "pkg/mesh_simplify.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bl4 {
namespace {

// 21 bits per axis: a cell key for up to ~2M cells a side, far beyond
// any mesh this is used on.
uint64_t cell_key(const MeshVertex& v, float cell) {
    auto axis = [cell](float p) {
        return static_cast<uint64_t>(static_cast<int64_t>(std::floor(p / cell)) + (1 << 20)) &
               0x1FFFFFull;
    };
    return (axis(v.px) << 42) | (axis(v.py) << 21) | axis(v.pz);
}

}  // namespace

void cluster_simplify(MeshData& mesh, float cell) {
    if (cell <= 0.f || mesh.vertices.empty()) return;

    std::unordered_map<uint64_t, uint32_t> cluster_of_key;
    std::vector<uint32_t> cluster_of_vertex(mesh.vertices.size());
    std::vector<MeshVertex> merged;
    std::vector<uint32_t> members;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const MeshVertex& v = mesh.vertices[i];
        auto [it, inserted] =
            cluster_of_key.emplace(cell_key(v, cell), static_cast<uint32_t>(merged.size()));
        if (inserted) {
            merged.push_back(v);  // first member's uv/tangent stand for the cell
            merged.back().px = merged.back().py = merged.back().pz = 0.f;
            merged.back().nx = merged.back().ny = merged.back().nz = 0.f;
            members.push_back(0);
        }
        MeshVertex& m = merged[it->second];
        m.px += v.px; m.py += v.py; m.pz += v.pz;
        m.nx += v.nx; m.ny += v.ny; m.nz += v.nz;
        ++members[it->second];
        cluster_of_vertex[i] = it->second;
    }
    for (size_t c = 0; c < merged.size(); ++c) {
        MeshVertex& m = merged[c];
        const float n = static_cast<float>(members[c]);
        m.px /= n; m.py /= n; m.pz /= n;
        const float len = std::sqrt(m.nx * m.nx + m.ny * m.ny + m.nz * m.nz);
        if (len > 1e-6f) { m.nx /= len; m.ny /= len; m.nz /= len; }
        else { m.nx = 0.f; m.ny = 1.f; m.nz = 0.f; }
    }

    // Sections keep their order and material, with their surviving
    // triangles packed contiguously.
    std::vector<uint32_t> indices;
    for (MeshSection& section : mesh.sections) {
        const uint32_t first = static_cast<uint32_t>(indices.size());
        for (uint32_t t = 0; t < section.num_triangles; ++t) {
            const size_t base = section.first_index + static_cast<size_t>(t) * 3;
            if (base + 2 >= mesh.indices.size()) break;
            const uint32_t a = cluster_of_vertex[mesh.indices[base]];
            const uint32_t b = cluster_of_vertex[mesh.indices[base + 1]];
            const uint32_t c = cluster_of_vertex[mesh.indices[base + 2]];
            if (a == b || b == c || a == c) continue;  // collapsed into a line or point
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
        }
        section.first_index = first;
        section.num_triangles = static_cast<uint32_t>((indices.size() - first) / 3);
    }
    mesh.vertices = std::move(merged);
    mesh.indices = std::move(indices);
}

}  // namespace bl4
