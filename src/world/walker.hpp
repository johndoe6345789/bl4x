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

// Spline-mesh deformation inputs, in the component's own space (UE
// units): the mesh is bent along a cubic Hermite from start to end. See
// deform_spline_mesh in pkg/spline_mesh.hpp for the field order.
constexpr size_t kSplineParamCount = 30;

struct Placement {
    // "mesh"; "spline_mesh" (one instance, bent by `spline`); "water"
    // (a water body's swim plane); "lake" (a surface to triangulate from
    // `outline`, mesh is then only an identity key).
    std::string kind;
    std::string mesh;       // object path, e.g. "/Game/World/.../SM_X.SM_X"
    // The emitting component's class, e.g. "StaticMeshComponent",
    // "LandscapeNaniteComponent": what a mesh stored inside a cell
    // package is for, which its path cannot say.
    std::string component;
    std::vector<std::string> materials;
    std::vector<float> xforms;  // 10 floats/instance: tx,ty,tz,qx,qy,qz,qw,sx,sy,sz
    std::vector<float> spline;   // kSplineParamCount floats, kind "spline_mesh" only
    std::vector<float> outline;  // xyz per point, component space, kind "lake" only
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
