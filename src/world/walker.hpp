#pragma once
// Flattens one World_P cell's actor/component tree into world-space
// mesh placements, mirroring the C# Exporter's CellWalker.cs: same
// coordinate convention (glTF space -- Y up, metres, quaternion xyzw)
// and the same placement grouping (kind/mesh/materials/xforms).
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg/obj_index.hpp"
#include "pkg/property.hpp"

namespace bl4 {

class Package;
class Usmap;

struct Placement {
    std::string kind;       // "mesh" | "spline" | "landscape" (spline/landscape: not yet baked)
    std::string mesh;       // object path, e.g. "/Game/World/.../SM_X.SM_X"
    std::vector<std::string> materials;
    std::vector<float> xforms;  // 10 floats/instance: tx,ty,tz,qx,qy,qz,qw,sx,sy,sz
};

struct CellPlacements {
    std::string cell;
    std::vector<Placement> entries;
    int hidden = 0;
    int unresolved = 0;
    int unsupported = 0;  // components whose class we can't deserialize at all
};

// Walks package's World export (found by class name "World") and
// returns its placements. On any per-component failure the component
// is skipped (counted in unsupported/unresolved) rather than aborting
// the whole cell.
CellPlacements walk_cell(Package& pkg, const Usmap& usmap, const std::string& cell_name);

void write_placements_json(const CellPlacements& cell, const std::string& path);

}  // namespace bl4
