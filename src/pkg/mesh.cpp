#include "pkg/mesh.hpp"

#include <algorithm>
#include <cmath>

#include "core/reader.hpp"
#include "pkg/nanite.hpp"
#include "pkg/package.hpp"
#include "pkg/property.hpp"
#include "pkg/static_mesh_lod.hpp"
#include "pkg/usmap.hpp"

namespace bl4 {
namespace {

size_t export_start(Package& pkg, uint32_t i) {
    return static_cast<size_t>(pkg.all_export_data_offset()) +
           static_cast<size_t>(pkg.export_at(i).cooked_serial_offset);
}

// See pkg/object.cpp for the fuller writeup; duplicated locally like
// pkg/texture.cpp does, rather than shared, to keep each pkg/*.cpp file
// independent.
void skip_object_guid_tail(Reader& r, const ExportEntry& e) {
    constexpr uint32_t kClassDefaultObject = 0x10;
    if (e.object_flags & kClassDefaultObject) return;
    if (r.read<int32_t>() != 0) r.skip(16);
}

// MeshVertex.TangentFor (CUE4Parse-Conversion): synthesizes a plausible
// tangent for vertices that don't carry an explicit one (the common case
// -- most Nanite meshes are built without "explicit tangents").
void synthesize_tangent(float nx, float ny, float nz, float out[4]) {
    float ax = 1.0f, ay = 0.0f, az = 0.0f;
    if (std::fabs(nz) < 0.9f) { ax = 0.0f; ay = 0.0f; az = 1.0f; }
    // t = normalize(cross(axis, n))
    float tx = ay * nz - az * ny, ty = az * nx - ax * nz, tz = ax * ny - ay * nx;
    float len = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (len > 1e-8f) { tx /= len; ty /= len; tz /= len; } else { tx = 1.0f; ty = 0.0f; tz = 0.0f; }
    out[0] = tx; out[1] = ty; out[2] = tz; out[3] = 1.0f;
}

MeshData assemble_from_nanite(const nanite::RawNaniteMesh& raw, uint32_t section_count) {
    MeshData out;
    out.is_nanite = true;
    out.num_tex_coords = raw.num_tex_coords;
    out.vertices.reserve(raw.vertices.size());
    for (const auto& v : raw.vertices) {
        MeshVertex mv;
        mv.px = v.pos[0]; mv.py = v.pos[1]; mv.pz = v.pos[2];
        mv.nx = v.normal[0]; mv.ny = v.normal[1]; mv.nz = v.normal[2];
        if (v.has_tangent) {
            mv.tx = v.tangent[0]; mv.ty = v.tangent[1]; mv.tz = v.tangent[2]; mv.tw = v.tangent[3];
        } else {
            float t[4];
            synthesize_tangent(mv.nx, mv.ny, mv.nz, t);
            mv.tx = t[0]; mv.ty = t[1]; mv.tz = t[2]; mv.tw = t[3];
        }
        mv.r = v.color[0]; mv.g = v.color[1]; mv.b = v.color[2]; mv.a = v.color[3];
        for (int k = 0; k < 4; ++k) { mv.uv[k].u = v.uv[k][0]; mv.uv[k].v = v.uv[k][1]; }
        out.vertices.push_back(mv);
    }

    uint32_t sc = section_count == 0 ? 1 : section_count;
    std::vector<uint32_t> face_counts(sc, 0), first_index(sc, 0);
    for (const auto& t : raw.triangles) ++face_counts[std::min(t.material_index, sc - 1)];
    uint32_t running = 0;
    for (uint32_t s = 0; s < sc; ++s) { first_index[s] = running; running += face_counts[s] * 3; }
    out.indices.assign(running, 0);
    std::vector<uint32_t> cursor = first_index;
    for (const auto& t : raw.triangles) {
        uint32_t mi = std::min(t.material_index, sc - 1);
        uint32_t& w = cursor[mi];
        out.indices[w + 0] = t.v0; out.indices[w + 1] = t.v1; out.indices[w + 2] = t.v2;
        w += 3;
    }
    for (uint32_t s = 0; s < sc; ++s)
        out.sections.push_back({s, first_index[s], face_counts[s]});
    return out;
}

MeshData assemble_from_classic(const classic_mesh::Lod& lod) {
    MeshData out;
    out.is_nanite = false;
    out.num_tex_coords = lod.num_tex_coords;
    out.vertices.resize(lod.num_vertices);
    for (uint32_t i = 0; i < lod.num_vertices; ++i) {
        MeshVertex& mv = out.vertices[i];
        if (lod.positions.size() >= static_cast<size_t>(i) * 3 + 3) {
            mv.px = lod.positions[i * 3 + 0]; mv.py = lod.positions[i * 3 + 1]; mv.pz = lod.positions[i * 3 + 2];
        }
        if (lod.normals.size() >= static_cast<size_t>(i) * 3 + 3) {
            mv.nx = lod.normals[i * 3 + 0]; mv.ny = lod.normals[i * 3 + 1]; mv.nz = lod.normals[i * 3 + 2];
        }
        if (lod.tangents.size() >= static_cast<size_t>(i) * 4 + 4) {
            mv.tx = lod.tangents[i * 4 + 0]; mv.ty = lod.tangents[i * 4 + 1];
            mv.tz = lod.tangents[i * 4 + 2]; mv.tw = lod.tangents[i * 4 + 3];
        }
        if (lod.colors.size() >= static_cast<size_t>(i) * 4 + 4) {
            mv.r = lod.colors[i * 4 + 0]; mv.g = lod.colors[i * 4 + 1];
            mv.b = lod.colors[i * 4 + 2]; mv.a = lod.colors[i * 4 + 3];
        }
        for (uint32_t ch = 0; ch < lod.uv_channels.size() && ch < 4; ++ch) {
            const auto& uvs = lod.uv_channels[ch];
            if (uvs.size() >= static_cast<size_t>(i) * 2 + 2) {
                mv.uv[ch].u = uvs[i * 2]; mv.uv[ch].v = uvs[i * 2 + 1];
            }
        }
    }
    out.indices = lod.indices;
    for (const auto& s : lod.sections)
        out.sections.push_back({s.material_index, s.first_index, s.num_triangles});
    return out;
}

std::vector<std::string> read_static_materials(Package& pkg, const PropertyBag& props) {
    std::vector<std::string> out;
    const PropertyValue* v = props.find("StaticMaterials");
    if (!v || v->kind != PropertyValue::Kind::Array) return out;
    for (const auto& e : v->arr) {
        std::string path;
        if (e.kind == PropertyValue::Kind::Bag && e.bag) {
            if (const PropertyValue* mi = e.bag->find("MaterialInterface");
                mi && mi->kind == PropertyValue::Kind::Object) {
                path = pkg.resolve_asset_path(mi->obj);
            }
        }
        out.push_back(std::move(path));
    }
    return out;
}

}  // namespace

MeshData load_static_mesh(Package& pkg, const Usmap& usmap, uint32_t export_index) {
    Reader r(pkg.data());
    r.seek(export_start(pkg, export_index));
    const ExportEntry& e = pkg.export_at(export_index);
    std::string class_name = pkg.resolve_object_name(e.class_index);

    PropertyBag props = read_unversioned_properties(r, usmap, pkg, class_name);
    skip_object_guid_tail(r, e);

    uint8_t strip_global = r.read<uint8_t>();
    r.read<uint8_t>();  // strip class flags, unused here
    bool cooked = r.read<int32_t>() != 0;
    // Editor-only bounds (Ver < STATIC_MESH_REFACTOR) never applies to
    // BL4's engine version -- nothing to skip here.

    r.read<int32_t>();  // BodySetup (FPackageIndex), not needed for geometry
    // Old CollisionModel field (Ver < REMOVE_STATICMESH_COLLISIONMODEL)
    // never applies to BL4.
    r.read<int32_t>();  // NavCollision (StaticMesh.HasNavCollision is true for BL4)
    // The entire `Ar.Game < GAME_UE4_0` legacy branch, and the editor-only
    // thumbnail block right after it, are both unreachable for BL4's
    // engine version regardless of strip flags -- see README's mesh
    // section for why. LightingGuid, VertexPositionVersionNumber,
    // CachedStreamingTextureFactors, and bRemoveDegenerates are likewise
    // either always-16-bytes or never-present for BL4.
    r.skip(16);  // LightingGuid

    int32_t num_sockets = r.read_count();
    r.skip(static_cast<size_t>(num_sockets) * 4);  // Sockets: TArray<FPackageIndex>

    MeshData out;
    if (!pkg.is_filter_editor_only()) return out;  // matches UStaticMesh's own early-out

    classic_mesh::Lod classic_lod;
    nanite::RawNaniteMesh raw_nanite;
    if (cooked) {
        // FStaticMeshRenderData: LODs, then a byte, then Nanite data.
        // StaticMesh.KeepMobileMinLODSettingOnDesktop is false for BL4
        // (no minMobileLODIdx read).
        int32_t num_lods = r.read_count();
        std::vector<classic_mesh::Lod> lods(static_cast<size_t>(num_lods));
        for (auto& lod : lods) lod = classic_mesh::parse_lod(r, pkg);
        r.read<uint8_t>();  // numInlinedLODs (Game >= UE4.23)
        raw_nanite = nanite::load_nanite_mesh(r, pkg);
        // Nothing after Nanite data is needed: StaticMaterials (below)
        // comes from the tagged property bag, not this binary tail.

        for (auto& lod : lods) {
            if (lod.has_buffers && !lod.indices.empty()) { classic_lod = std::move(lod); break; }
        }
    }
    (void)strip_global;

    std::vector<std::string> materials = read_static_materials(pkg, props);
    uint32_t section_count = static_cast<uint32_t>(materials.size());

    if (!raw_nanite.vertices.empty())
        out = assemble_from_nanite(raw_nanite, section_count);
    else if (classic_lod.has_buffers)
        out = assemble_from_classic(classic_lod);
    else
        return out;  // no usable geometry (fully cooked-out, non-Nanite mesh)

    out.material_paths = std::move(materials);
    return out;
}

}  // namespace bl4
