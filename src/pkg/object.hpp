#pragma once
// Deserializes one export's properties, and -- for the handful of
// classes the world walker needs -- the class-specific binary tail
// that follows them (CUE4Parse's per-class Deserialize overrides).
//
// Every other class is left alone: we only need its export's class
// name (already resolvable without touching its data at all -- see
// Package::resolve_object_name), so an unsupported class is simply
// never parsed rather than approximated.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pkg/obj_index.hpp"
#include "pkg/property.hpp"

namespace bl4 {

class Package;
class Usmap;

struct Instance {
    Vec3 translation;
    Vec4 rotation;  // quaternion x,y,z,w
    Vec3 scale;
};

struct ComponentData {
    PropertyBag props;
    // Present only for Instanced/HLODInstanced/Hierarchical/Foliage
    // static mesh components (the raw per-instance transform array;
    // everything else uses just Relative{Location,Rotation,Scale3D}
    // from props).
    std::vector<Instance> instances;
};

// Reads export_index's unversioned properties (throws ParseError if
// its class or any struct it uses isn't in usmap).
PropertyBag load_properties(Package& pkg, const Usmap& usmap, uint32_t export_index);

// Like load_properties, but also decodes the per-instance transform
// array for the instanced-mesh component classes (see ComponentData).
// class_name is the export's already-resolved class (avoids resolving
// it twice); pass the exact native class name.
ComponentData load_component(Package& pkg, const Usmap& usmap, uint32_t export_index,
                             const std::string& class_name);

// ULevel::Actors is raw binary (a plain TArray<FPackageIndex>), not a
// tagged property -- read right after the usual property+GUID tail.
std::vector<ObjIndex> read_level_actors(Package& pkg, const Usmap& usmap,
                                        uint32_t level_export_index);

// UWorld::PersistentLevel is likewise raw binary, the first field
// after the property+GUID tail.
ObjIndex read_world_persistent_level(Package& pkg, const Usmap& usmap,
                                     uint32_t world_export_index);

}  // namespace bl4
