#include "pkg/object.hpp"

#include <cstdlib>
#include <cmath>
#include <iostream>

#include "core/reader.hpp"
#include "pkg/package.hpp"
#include "pkg/usmap.hpp"

namespace bl4 {
namespace {

// FMatrix::ToQuat's exact branching (UE4/Objects/Core/Math/FQuat.cs),
// applied to the pre-normalized (unit-row) rotation part of a matrix.
Vec4 quat_from_orthonormal_rows(const double m[3][3]) {
    double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0) {
        double inv_s = 1.0 / std::sqrt(tr + 1.0);
        double w = 0.5 / inv_s;
        double s = 0.5 * inv_s;
        return {(m[1][2] - m[2][1]) * s, (m[2][0] - m[0][2]) * s,
                (m[0][1] - m[1][0]) * s, w};
    }
    static constexpr int next[3] = {1, 2, 0};
    int i = 0;
    if (m[1][1] > m[0][0]) i = 1;
    if (m[2][2] > m[i][i]) i = 2;
    int j = next[i], k = next[j];
    double s = m[i][i] - m[j][j] - m[k][k] + 1.0;
    double inv_s = 1.0 / std::sqrt(s);
    double qt[4] = {0, 0, 0, 0};
    qt[i] = 0.5 / inv_s;
    s = 0.5 * inv_s;
    qt[3] = (m[j][k] - m[k][j]) * s;
    qt[j] = (m[i][j] + m[j][i]) * s;
    qt[k] = (m[i][k] + m[k][i]) * s;
    return {qt[0], qt[1], qt[2], qt[3]};
}

// FTransform::SetFromMatrix, given the matrix as 4 row vectors (rows
// 0-2 are the X/Y/Z basis, row 3 is the translation -- UE's row-vector
// convention, v' = v * M).
Instance decompose_matrix(const double m[4][4]) {
    double det =
        m[0][0] * (m[1][1] * (m[2][2] * m[3][3] - m[2][3] * m[3][2]) -
                   m[2][1] * (m[1][2] * m[3][3] - m[1][3] * m[3][2]) +
                   m[3][1] * (m[1][2] * m[2][3] - m[1][3] * m[2][2])) -
        m[1][0] * (m[0][1] * (m[2][2] * m[3][3] - m[2][3] * m[3][2]) -
                   m[2][1] * (m[0][2] * m[3][3] - m[0][3] * m[3][2]) +
                   m[3][1] * (m[0][2] * m[2][3] - m[0][3] * m[2][2])) +
        m[2][0] * (m[0][1] * (m[1][2] * m[3][3] - m[1][3] * m[3][2]) -
                   m[1][1] * (m[0][2] * m[3][3] - m[0][3] * m[3][2]) +
                   m[3][1] * (m[0][2] * m[1][3] - m[0][3] * m[1][2])) -
        m[3][0] * (m[0][1] * (m[1][2] * m[2][3] - m[1][3] * m[2][2]) -
                   m[1][1] * (m[0][2] * m[2][3] - m[0][3] * m[2][2]) +
                   m[2][1] * (m[0][2] * m[1][3] - m[0][3] * m[1][2]));

    double rows[3][3];
    Vec3 scale{};
    double* sc[3] = {&scale.x, &scale.y, &scale.z};
    for (int i = 0; i < 3; ++i) {
        double sq = m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2];
        if (sq > 1e-8) {
            double len = std::sqrt(sq);
            *sc[i] = len;
            rows[i][0] = m[i][0] / len;
            rows[i][1] = m[i][1] / len;
            rows[i][2] = m[i][2] / len;
        } else {
            *sc[i] = 0;
            rows[i][0] = m[i][0]; rows[i][1] = m[i][1]; rows[i][2] = m[i][2];
        }
    }
    if (det < 0.0) {
        scale.x *= -1.0;
        rows[0][0] = -rows[0][0]; rows[0][1] = -rows[0][1]; rows[0][2] = -rows[0][2];
    }

    Instance inst;
    inst.scale = scale;
    inst.rotation = quat_from_orthonormal_rows(rows);
    double len = std::sqrt(inst.rotation.x * inst.rotation.x + inst.rotation.y * inst.rotation.y +
                           inst.rotation.z * inst.rotation.z + inst.rotation.w * inst.rotation.w);
    if (len > 1e-12) {
        inst.rotation.x /= len; inst.rotation.y /= len;
        inst.rotation.z /= len; inst.rotation.w /= len;
    }
    inst.translation = {m[3][0], m[3][1], m[3][2]};
    return inst;
}

void read_matrix(Reader& r, double m[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = r.read<double>();
}

size_t export_start(Package& pkg, uint32_t export_index) {
    const ExportEntry& e = pkg.export_at(export_index);
    return static_cast<size_t>(pkg.all_export_data_offset()) +
           static_cast<size_t>(e.cooked_serial_offset);
}
size_t export_end(Package& pkg, uint32_t export_index) {
    const ExportEntry& e = pkg.export_at(export_index);
    return export_start(pkg, export_index) + static_cast<size_t>(e.cooked_serial_size);
}

// UActorComponent's own Deserialize tail (UCSModifiedProperties), read
// only to advance the cursor correctly for classes whose own tail
// (e.g. InstancedStaticMeshComponent's PerInstanceSMData) follows it.
void skip_actor_component_tail(Reader& r, size_t valid_pos) {
    if (r.pos() >= valid_pos) return;
    int32_t count = r.read_count();  // FFortniteReleaseBranchCustomObjectVersion
                                     // ActorComponentUCSModifiedPropertiesSparseStorage(3) <= 15: always present for BL4
    for (int32_t i = 0; i < count; ++i) {
        r.skip(4);   // FPackageIndex MemberParent
        r.skip(8);   // FName MemberName (index + extra)
        r.skip(16);  // FGuid MemberGuid
    }
}

// USceneComponent's tail: an optional cached-bounds block, present
// only if the component opted into static bounds caching.
void skip_scene_component_tail(Reader& r, const PropertyBag& props) {
    bool compute_once = props.get_bool("bComputeBoundsOnceForGame") ||
                        props.get_bool("bComputedBoundsOnceForGame");
    if (!compute_once) return;  // version gate (SerializeSceneComponentStaticBounds) always holds for BL4
    bool is_cooked = r.read<int32_t>() != 0;
    if (is_cooked) r.skip(24 + 24 + 8);  // FBoxSphereBounds: Origin+BoxExtent (FVector, LWC) + SphereRadius (FReal)
}

// One FStaticMeshComponentLODInfo entry.
void skip_lod_info(Reader& r) {
    uint8_t global_strip = r.read<uint8_t>();
    r.skip(1);  // class strip flags
    bool av_stripped = (global_strip & 2) != 0;
    if (!av_stripped) {
        // BL4 is >= UE5.5 and cooked: MapBuildDataId (+OriginalMapBuildDataId).
        r.skip(16);
        r.skip(16);
    }
    bool editor_stripped = (global_strip & 1) != 0;
    uint8_t load_vertex_colors = r.read<uint8_t>();
    if (load_vertex_colors == 1) {
        // FColorVertexBuffer(Ar): its own strip flags, then Stride and
        // NumVertices (informational only -- the FColor data below is
        // a self-describing bulk array, sized by its own header).
        uint8_t vc_global = r.read<uint8_t>();
        r.skip(1);
        r.skip(4);  // Stride
        int32_t num_vertices = r.read<int32_t>();
        bool vc_av_stripped = (vc_global & 2) != 0;
        if (!vc_av_stripped && num_vertices > 0) {
            int32_t elem_size = r.read<int32_t>();
            int32_t elem_count = r.read<int32_t>();
            r.skip(static_cast<size_t>(elem_size) * static_cast<size_t>(elem_count));
        }
    }
    if (!editor_stripped) {
        int32_t n = r.read_count();
        for (int32_t i = 0; i < n; ++i) {
            r.skip(24);  // Position (FVector, LWC)
            r.skip(32);  // Normal (FVector4, LWC)
            r.skip(4);   // Color (FColor)
        }
    }
}

}  // namespace

// UObject::Deserialize reads an optional per-instance GUID right after
// the property block, for every export except class default objects
// (RF_ClassDefaultObject) -- easy to miss since it's not itself a
// tagged property, and every class-specific tail comes after it.
constexpr uint32_t kClassDefaultObject = 0x10;
void skip_object_guid_tail(Reader& r, const ExportEntry& e) {
    if (e.object_flags & kClassDefaultObject) return;
    if (r.read<int32_t>() != 0) r.skip(16);
}

PropertyBag load_properties(Package& pkg, const Usmap& usmap, uint32_t export_index) {
    Reader r(pkg.data());
    r.seek(export_start(pkg, export_index));
    std::string class_name = pkg.resolve_object_name(pkg.export_at(export_index).class_index);
    PropertyBag bag = read_unversioned_properties(r, usmap, pkg, class_name);
    skip_object_guid_tail(r, pkg.export_at(export_index));
    return bag;
}

ComponentData load_component(Package& pkg, const Usmap& usmap, uint32_t export_index,
                             const std::string& class_name) {
    ComponentData out;
    Reader r(pkg.data());
    size_t start = export_start(pkg, export_index);
    size_t end = export_end(pkg, export_index);
    r.seek(start);
    out.props = read_unversioned_properties(r, usmap, pkg, class_name);
    skip_object_guid_tail(r, pkg.export_at(export_index));

    bool is_ism = class_name == "InstancedStaticMeshComponent" ||
                 class_name == "HLODInstancedStaticMeshComponent" ||
                 class_name == "HierarchicalInstancedStaticMeshComponent" ||
                 class_name == "FoliageInstancedStaticMeshComponent" ||
                 class_name == "GrassInstancedStaticMeshComponent";
    if (!is_ism) return out;
    bool trace = std::getenv("BL4X_TRACE") != nullptr;
    auto log = [&](const char* label) {
        if (trace) std::cerr << "  [" << label << "] pos=" << r.pos()
                             << " (start=" << start << " end=" << end << ")\n";
    };

    // ActorComponent -> SceneComponent -> StaticMeshComponent tails,
    // each parsed only far enough to reach the next; see README.
    log("props-end");
    skip_actor_component_tail(r, end);
    log("actorcomp-end");
    if (r.pos() >= end) return out;
    skip_scene_component_tail(r, out.props);
    log("scenecomp-end");
    if (r.pos() >= end) return out;
    int32_t lod_count = r.read_count();
    if (trace) std::cerr << "  lod_count=" << lod_count << "\n";
    for (int32_t i = 0; i < lod_count; ++i) skip_lod_info(r);
    log("lods-end");
    bool mesh_paint_cooked = r.read<int32_t>() != 0;
    if (mesh_paint_cooked) r.skip(4);
    log("meshpaint-end");

    // UInstancedStaticMeshComponent's own tail.
    bool cooked = r.read<int32_t>() != 0;
    bool has_skip_data = r.read<int32_t>() != 0;
    if (trace) std::cerr << "  cooked=" << cooked << " has_skip_data=" << has_skip_data << "\n";
    if (has_skip_data) {
        int32_t elem_size = r.read<int32_t>();
        int32_t count = r.read<int32_t>();
        if (trace) std::cerr << "  elem_size=" << elem_size << " count=" << count << "\n";
        (void)elem_size;
        out.instances.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            double m[4][4];
            read_matrix(r, m);
            out.instances.push_back(decompose_matrix(m));
        }
        int32_t cd_size = r.read<int32_t>();
        int32_t cd_count = r.read<int32_t>();
        r.skip(static_cast<size_t>(cd_size) * static_cast<size_t>(cd_count));
    }
    if (cooked) {
        bool has_cooked_data = r.read<int32_t>() != 0;
        // else: nothing more to read for this export.
        (void)has_cooked_data;
    }
    return out;
}

std::vector<ObjIndex> read_level_actors(Package& pkg, const Usmap& usmap,
                                        uint32_t level_export_index) {
    Reader r(pkg.data());
    size_t start = export_start(pkg, level_export_index);
    size_t end = export_end(pkg, level_export_index);
    r.seek(start);
    const ExportEntry& e = pkg.export_at(level_export_index);
    read_unversioned_properties(r, usmap, pkg, pkg.resolve_object_name(e.class_index));
    skip_object_guid_tail(r, e);
    if ((e.object_flags & kClassDefaultObject) || r.pos() >= end) return {};
    // FReleaseObjectVersion.LevelTransArrayConvertedToTArray(2) <= 44
    // for BL4: no legacy padding before the array.
    int32_t count = r.read_count();
    std::vector<ObjIndex> actors;
    actors.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) actors.push_back(pkg.resolve_legacy_index(r.read<int32_t>()));
    return actors;
}

ObjIndex read_world_persistent_level(Package& pkg, const Usmap& usmap,
                                     uint32_t world_export_index) {
    Reader r(pkg.data());
    r.seek(export_start(pkg, world_export_index));
    const ExportEntry& e = pkg.export_at(world_export_index);
    read_unversioned_properties(r, usmap, pkg, pkg.resolve_object_name(e.class_index));
    skip_object_guid_tail(r, e);
    return pkg.resolve_legacy_index(r.read<int32_t>());
}

}  // namespace bl4
