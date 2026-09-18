#pragma once
// Vertex-clustering simplification: snap every vertex to a grid of
// `cell` units (in the mesh's own space), merge each cell's vertices into
// their average, and drop the triangles that collapse. Crude next to
// quadric decimation, but exactly right for what it is used for -- the
// overview bake's terrain, a heightfield seen from kilometres up, where
// 84 M triangles of landscape become a few hundred thousand.
#include "pkg/mesh.hpp"

namespace bl4 {

void cluster_simplify(MeshData& mesh, float cell);

}  // namespace bl4
