#include "world/walker.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>

#include "pkg/object.hpp"
#include "pkg/package.hpp"
#include "pkg/usmap.hpp"

namespace bl4 {
namespace {

struct XForm { Vec3 t; Vec4 r{0, 0, 0, 1}; Vec3 s{1, 1, 1}; };

Vec4 quat_mul(const Vec4& a, const Vec4& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 quat_rotate(const Vec4& q, const Vec3& v) {
    Vec3 qv{q.x, q.y, q.z};
    Vec3 t{2 * (qv.y * v.z - qv.z * v.y), 2 * (qv.z * v.x - qv.x * v.z),
          2 * (qv.x * v.y - qv.y * v.x)};
    Vec3 cqt{qv.y * t.z - qv.z * t.y, qv.z * t.x - qv.x * t.z, qv.x * t.y - qv.y * t.x};
    return {v.x + q.w * t.x + cqt.x, v.y + q.w * t.y + cqt.y, v.z + q.w * t.z + cqt.z};
}
Vec4 rotator_to_quat(const Vec3& pyr) {  // x=pitch, y=yaw, z=roll, degrees
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    double sp = std::sin(pyr.x * 0.5 * kDeg2Rad), cp = std::cos(pyr.x * 0.5 * kDeg2Rad);
    double sy = std::sin(pyr.y * 0.5 * kDeg2Rad), cy = std::cos(pyr.y * 0.5 * kDeg2Rad);
    double sr = std::sin(pyr.z * 0.5 * kDeg2Rad), cr = std::cos(pyr.z * 0.5 * kDeg2Rad);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
            cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}
// world = local * parent (UE's FTransform::operator*, A=local, B=parent).
XForm combine(const XForm& local, const XForm& parent) {
    XForm w;
    w.r = quat_mul(parent.r, local.r);
    w.s = {local.s.x * parent.s.x, local.s.y * parent.s.y, local.s.z * parent.s.z};
    Vec3 scaled{local.t.x * parent.s.x, local.t.y * parent.s.y, local.t.z * parent.s.z};
    Vec3 rotated = quat_rotate(parent.r, scaled);
    w.t = {rotated.x + parent.t.x, rotated.y + parent.t.y, rotated.z + parent.t.z};
    return w;
}
Vec4 normalize_quat(Vec4 q) {
    double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-12) { q.x /= len; q.y /= len; q.z /= len; q.w /= len; }
    return q;
}
XForm normalize(XForm x) {
    x.r = normalize_quat(x.r);
    return x;
}
XForm from_instance(const Instance& i) { return {i.translation, i.rotation, i.scale}; }
XForm local_transform(const PropertyBag& props) {
    XForm x;
    x.t = props.get_vec3("RelativeLocation");
    x.r = normalize_quat(rotator_to_quat(props.get_vec3("RelativeRotation")));
    x.s = props.get_vec3("RelativeScale3D", {1, 1, 1});
    return x;
}
// Same conversion CUE4Parse's Gltf writer / the C# Exporter's
// Placement.AppendGltf use: swap Y/Z, cm -> m.
bool finite_xform(const XForm& x) {
    auto ok = [](double v) { return std::isfinite(v) && std::abs(v) < 1e7; };
    return ok(x.t.x) && ok(x.t.y) && ok(x.t.z) && std::isfinite(x.r.x) && std::isfinite(x.r.y) &&
          std::isfinite(x.r.z) && std::isfinite(x.r.w) && ok(x.s.x) && ok(x.s.y) && ok(x.s.z);
}
void append_gltf(std::vector<float>& out, const XForm& x) {
    out.insert(out.end(), {float(x.t.x * 0.01), float(x.t.z * 0.01), float(x.t.y * 0.01),
                           float(x.r.x), float(x.r.z), float(x.r.y), float(-x.r.w),
                           float(x.s.x), float(x.s.z), float(x.s.y)});
}

struct Node {
    Package* pkg = nullptr;
    uint32_t index = 0;
    std::string component_class;
    PropertyBag props;
    std::vector<Instance> instances;
    ObjIndex attach_parent;
    // Set when attached to a socket on the parent's mesh (e.g. a pipe
    // piece on the previous piece's "TL_End_01"): the socket's own
    // transform sits between this component and its parent.
    std::string attach_socket;
    bool visited = false;  // cycle guard while resolving world transform
    bool has_world = false;
    XForm world;
};

using Key = std::pair<Package*, uint32_t>;
struct KeyHash {
    size_t operator()(const Key& k) const {
        return std::hash<void*>()(k.first) ^ (std::hash<uint32_t>()(k.second) << 1);
    }
};

class Walker {
public:
    Walker(const Usmap& usmap, CellPlacements& out) : usmap_(usmap), out_(out) {}

    void add_actor(Package& pkg, uint32_t actor_index) {
        std::string cls = pkg.resolve_object_name(pkg.export_at(actor_index).class_index);
        PropertyBag props;
        try {
            props = load_properties(pkg, usmap_, actor_index);
        } catch (const std::exception& e) {
            ++out_.unsupported;
            if (std::getenv("BL4X_TRACE"))
                std::cerr << "  actor " << cls << ": " << e.what() << "\n";
            return;
        }

        std::vector<ObjIndex> refs;
        if (auto o = props.get_object("RootComponent"); !o.is_null()) refs.push_back(o);
        if (auto o = props.get_object("SplineComponent"); !o.is_null()) refs.push_back(o);
        for (auto o : props.get_object_array("InstanceComponents")) if (!o.is_null()) refs.push_back(o);
        for (auto o : props.get_object_array("BlueprintCreatedComponents")) if (!o.is_null()) refs.push_back(o);
        for (auto o : props.get_object_array("LandscapeComponents")) if (!o.is_null()) refs.push_back(o);
        // UE 5.5 landscapes keep a Nanite static mesh of the terrain
        // beside their heightfield components -- the only ground BL4
        // ships as geometry, and reachable from nowhere else.
        for (auto o : props.get_object_array("NaniteComponents")) if (!o.is_null()) refs.push_back(o);
        if (auto o = props.get_object("NaniteComponent"); !o.is_null()) refs.push_back(o);
        // Landscape spline actors hang their road/path meshes off their
        // own component lists.
        for (auto o : props.get_object_array("ControlPointMeshComponents")) if (!o.is_null()) refs.push_back(o);
        for (auto o : props.get_object_array("SegmentMeshComponents")) if (!o.is_null()) refs.push_back(o);

        for (ObjIndex ref : refs) visit_component(pkg, ref);
    }

    // Components the actor lists do not reach -- spline meshes hang off
    // landscape splines, river bodies and blueprint pipes by routes the
    // actor walk does not follow -- visited straight from the export
    // table. Their transforms still come from their AttachParent chain.
    void add_loose_components(Package& pkg) {
        for (uint32_t i = 0; i < pkg.export_count(); ++i) {
            const std::string cls = pkg.resolve_object_name(pkg.export_at(i).class_index);
            if (cls == "SplineMeshComponent" || cls == "WaterSplineComponent" ||
                cls == "WaterBodyCustomComponent") {
                ObjIndex idx;
                idx.raw = i;  // an export of this package
                visit_component(pkg, idx);
            }
        }
    }

    void finish() {
        // resolve_world can register new nodes (a parent reached only
        // via AttachParent, never listed on any actor) -- snapshot
        // keys before iterating so inserts never invalidate our loop,
        // and repeat until a pass adds nothing new.
        for (int guard = 0; guard < 8; ++guard) {
            std::vector<Key> keys;
            keys.reserve(nodes_.size());
            for (auto& [key, node] : nodes_) keys.push_back(key);
            size_t before = nodes_.size();
            for (auto& key : keys) resolve_world(key);
            if (nodes_.size() == before) break;
        }
        std::vector<Key> keys;
        keys.reserve(nodes_.size());
        for (auto& [key, node] : nodes_) keys.push_back(key);
        for (auto& key : keys) emit(nodes_.at(key));
    }

private:
    Node* visit_component(Package& owner_pkg, ObjIndex idx) {
        auto resolved = owner_pkg.resolve_export(idx);
        if (!resolved && idx.is_script_import()) return nullptr;  // native, not our export
        if (!resolved) { ++out_.unresolved; return nullptr; }
        Key key{resolved->first, resolved->second};
        auto it = nodes_.find(key);
        if (it != nodes_.end()) return &it->second;

        Package* pkg = resolved->first;
        uint32_t index = resolved->second;
        std::string cls = pkg->resolve_object_name(pkg->export_at(index).class_index);
        Node node;
        node.pkg = pkg;
        node.index = index;
        node.component_class = cls;
        try {
            ComponentData c = load_component(*pkg, usmap_, index, cls);
            node.props = std::move(c.props);
            node.instances = std::move(c.instances);
        } catch (const std::exception&) {
            ++out_.unsupported;
            return nullptr;  // don't register: unknown transform, can't place its mesh either
        }
        if (!node.props.get_bool("bVisible", true) || node.props.get_bool("bHiddenInGame")) {
            ++out_.hidden;
        }
        node.attach_parent = node.props.get_object("AttachParent");
        node.attach_socket = node.props.get_str("AttachSocketName");
        auto [ins, ok] = nodes_.emplace(key, std::move(node));
        return &ins->second;
    }

    void resolve_world(const Key& key) {
        auto it = nodes_.find(key);
        if (it == nodes_.end() || it->second.has_world) return;
        Node& n = it->second;
        if (n.visited) { n.has_world = true; n.world = local_transform(n.props); return; }  // cycle: bail safely
        n.visited = true;
        XForm local = local_transform(n.props);
        if (n.attach_parent.is_null()) {
            n.world = local;
        } else {
            auto parent = n.pkg->resolve_export(n.attach_parent);
            Node* pnode = parent ? visit_component(*parent->first, n.attach_parent) : nullptr;
            if (pnode) {
                resolve_world({pnode->pkg, pnode->index});
                XForm parent_world = pnode->world;
                // Without the socket, a chain of socket-attached pieces all
                // lands on its root, and each piece's scale compounds --
                // the sockets carry the inverse scale (a 2.54x pipe piece
                // sits on a 0.3937 socket), so ignoring them grew pipe
                // chains to kilometres.
                if (!n.attach_socket.empty() && n.attach_socket != "None")
                    if (auto sock = socket_transform(*pnode, n.attach_socket))
                        parent_world = normalize(combine(*sock, parent_world));
                n.world = normalize(combine(local, parent_world));
            } else {
                n.world = local;  // parent outside our set (or native): treat as top-level
            }
        }
        n.has_world = true;
    }

    // A UStaticMeshSocket's transform relative to its mesh, found among
    // the mesh package's StaticMeshSocket exports by SocketName.
    std::optional<XForm> socket_transform(Node& parent, const std::string& socket) {
        ObjIndex mesh = parent.props.get_object("StaticMesh");
        if (mesh.is_null()) return std::nullopt;
        auto target = parent.pkg->resolve_export(mesh);
        if (!target) return std::nullopt;
        Package* mesh_pkg = target->first;
        const std::string key = mesh_pkg->path() + "|" + socket;
        auto cached = sockets_.find(key);
        if (cached != sockets_.end()) return cached->second;
        std::optional<XForm> found;
        for (uint32_t i = 0; i < mesh_pkg->export_count() && !found; ++i) {
            if (mesh_pkg->resolve_object_name(mesh_pkg->export_at(i).class_index) != "StaticMeshSocket")
                continue;
            try {
                PropertyBag sp = load_properties(*mesh_pkg, usmap_, i);
                if (sp.get_str("SocketName") != socket) continue;
                XForm x;
                x.t = sp.get_vec3("RelativeLocation");
                x.r = normalize_quat(rotator_to_quat(sp.get_vec3("RelativeRotation")));
                x.s = sp.get_vec3("RelativeScale", {1, 1, 1});
                found = x;
            } catch (const std::exception&) {
            }
        }
        sockets_[key] = found;
        return found;
    }

    void emit(Node& n) {
        if (!n.has_world) return;
        bool hidden = !n.props.get_bool("bVisible", true) || n.props.get_bool("bHiddenInGame");
        if (hidden) return;
        if (n.component_class == "WaterSplineComponent") { emit_lake(n); return; }
        ObjIndex mesh = n.props.get_object("StaticMesh");
        // A custom water body draws WaterMeshOverride (BL4: a Gearbox swim
        // plane) scaled to the water's extent, at the water's height.
        if (mesh.is_null() && n.component_class == "WaterBodyCustomComponent")
            mesh = n.props.get_object("WaterMeshOverride");
        if (mesh.is_null()) return;
        if (n.component_class == "SplineMeshComponent") { emit_spline_mesh(n, mesh); return; }
        std::string mesh_path = n.pkg->resolve_asset_path(mesh);
        std::vector<std::string> materials;
        for (auto m : n.props.get_object_array("OverrideMaterials"))
            materials.push_back(m.is_null() ? "" : n.pkg->resolve_asset_path(m));

        std::string key = mesh_path + "#" + n.component_class;
        for (auto& m : materials) key += "|" + m;
        auto pit = placement_index_.find(key);
        Placement* p;
        if (pit == placement_index_.end()) {
            const char* kind = n.component_class == "WaterBodyCustomComponent" ? "water" : "mesh";
            out_.entries.push_back({kind, mesh_path, n.component_class, materials, {}, {}, {}});
            placement_index_[key] = out_.entries.size() - 1;
            p = &out_.entries.back();
        } else {
            p = &out_.entries[pit->second];
        }
        if (n.instances.empty()) {
            if (finite_xform(n.world)) append_gltf(p->xforms, n.world);
        } else {
            for (auto& inst : n.instances) {
                XForm w = normalize(combine(from_instance(inst), n.world));
                // A rare handful of instances (observed ~1 in 30,000)
                // decode to a non-finite or absurd transform; skip
                // rather than emit a placement that would corrupt
                // downstream JSON/glTF. Root cause not yet isolated --
                // see README.
                if (finite_xform(w)) append_gltf(p->xforms, w);
            }
        }
    }

    // One entry per spline mesh: each is bent differently, so none share.
    void emit_spline_mesh(Node& n, ObjIndex mesh) {
        if (!finite_xform(n.world)) return;
        Placement p{"spline_mesh", n.pkg->resolve_asset_path(mesh), n.component_class, {}, {}, {}, {}};
        for (auto m : n.props.get_object_array("OverrideMaterials"))
            p.materials.push_back(m.is_null() ? "" : n.pkg->resolve_asset_path(m));
        const PropertyValue* params = n.props.find("SplineParams");
        const PropertyBag empty;
        const PropertyBag& sp = (params && params->bag) ? *params->bag : empty;
        auto push3 = [&](const Vec3& v) {
            p.spline.insert(p.spline.end(), {float(v.x), float(v.y), float(v.z)});
        };
        auto push2 = [&](const Vec3& v) { p.spline.insert(p.spline.end(), {float(v.x), float(v.y)}); };
        push3(sp.get_vec3("StartPos"));
        push3(sp.get_vec3("StartTangent"));
        push3(sp.get_vec3("EndPos"));
        push3(sp.get_vec3("EndTangent"));
        push2(sp.get_vec3("StartScale", {1, 1, 0}));
        push2(sp.get_vec3("EndScale", {1, 1, 0}));
        push2(sp.get_vec3("StartOffset"));
        push2(sp.get_vec3("EndOffset"));
        p.spline.push_back(float(sp.get_double("StartRoll")));
        p.spline.push_back(float(sp.get_double("EndRoll")));
        const PropertyValue* axis = n.props.find("ForwardAxis");
        p.spline.push_back(axis ? float(axis->i) : 0.f);
        push3(n.props.get_vec3("SplineUpDir", {0, 0, 1}));
        p.spline.push_back(float(n.props.get_double("SplineBoundaryMin")));
        p.spline.push_back(float(n.props.get_double("SplineBoundaryMax")));
        p.spline.push_back(n.props.get_bool("bSmoothInterpRollScale") ? 1.f : 0.f);
        p.spline.resize(kSplineParamCount, 0.f);
        append_gltf(p.xforms, n.world);
        out_.entries.push_back(std::move(p));
    }

    // A lake's surface is built at runtime from its spline outline
    // (WaterBodyLakeComponent.LakeMeshComp is null in cooked data); only
    // the spline under a lake body is a lake -- rivers use spline meshes.
    void emit_lake(Node& n) {
        if (n.attach_parent.is_null() || !finite_xform(n.world)) return;
        auto parent = n.pkg->resolve_export(n.attach_parent);
        if (!parent) return;
        const std::string parent_class =
            parent->first->resolve_object_name(parent->first->export_at(parent->second).class_index);
        if (parent_class != "WaterBodyLakeComponent") return;
        const PropertyValue* curves = n.props.find("SplineCurves");
        if (!curves || !curves->bag) return;
        const PropertyValue* position = curves->bag->find("position");
        if (!position) position = curves->bag->find("Position");
        if (!position || !position->bag) return;
        const PropertyValue* points = position->bag->find("points");
        if (!points) points = position->bag->find("Points");
        if (!points || points->arr.size() < 3) return;

        Placement p{"lake", "lake:" + n.pkg->path() + "#" + std::to_string(n.index),
                    n.component_class, {}, {}, {}, {}};
        const size_t count = points->arr.size();
        for (size_t i = 0; i < count; ++i) {
            const PropertyValue& a = points->arr[i];
            const PropertyValue& b = points->arr[(i + 1) % count];  // lakes are closed loops
            if (!a.bag || !b.bag) return;
            const Vec3 p0 = a.bag->get_vec3("OutVal"), t0 = a.bag->get_vec3("LeaveTangent");
            const Vec3 p1 = b.bag->get_vec3("OutVal"), t1 = b.bag->get_vec3("ArriveTangent");
            const bool linear = a.bag->find("InterpMode") && a.bag->find("InterpMode")->i == 0;
            const int steps = linear ? 1 : 8;
            for (int s = 0; s < steps; ++s) {  // the segment's end is the next one's start
                const double t = double(s) / steps, t2 = t * t, t3 = t2 * t;
                const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
                const double h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
                p.outline.push_back(float(h00 * p0.x + h10 * t0.x + h01 * p1.x + h11 * t1.x));
                p.outline.push_back(float(h00 * p0.y + h10 * t0.y + h01 * p1.y + h11 * t1.y));
                p.outline.push_back(float(h00 * p0.z + h10 * t0.z + h01 * p1.z + h11 * t1.z));
            }
        }
        append_gltf(p.xforms, n.world);
        out_.entries.push_back(std::move(p));
    }

    const Usmap& usmap_;
    CellPlacements& out_;
    std::unordered_map<Key, Node, KeyHash> nodes_;
    std::unordered_map<std::string, size_t> placement_index_;
    std::unordered_map<std::string, std::optional<XForm>> sockets_;
};

}  // namespace

CellPlacements walk_cell(Package& pkg, const Usmap& usmap, const std::string& cell_name) {
    CellPlacements out;
    out.cell = cell_name;
    Walker walker(usmap, out);

    uint32_t world_index = UINT32_MAX, level_index = UINT32_MAX;
    for (uint32_t i = 0; i < pkg.export_count(); ++i) {
        std::string cls = pkg.resolve_object_name(pkg.export_at(i).class_index);
        if (cls == "World") world_index = i;
        else if (cls == "Level") level_index = i;
    }
    if (world_index == UINT32_MAX) return out;
    uint32_t actual_level = level_index;
    try {
        ObjIndex persistent = read_world_persistent_level(pkg, usmap, world_index);
        if (auto level = persistent.is_null() ? std::nullopt : pkg.resolve_export(persistent))
            actual_level = level->second;
    } catch (const std::exception& e) {
        std::cerr << "walk_cell: PersistentLevel read failed: " << e.what() << "\n";
    }
    if (actual_level == UINT32_MAX) return out;

    std::vector<ObjIndex> actors;
    try {
        actors = read_level_actors(pkg, usmap, actual_level);
    } catch (const std::exception& e) {
        std::cerr << "walk_cell: Level.Actors read failed: " << e.what() << "\n";
        return out;
    }
    for (ObjIndex actor : actors)
        if (!actor.is_null())
            if (auto resolved = pkg.resolve_export(actor)) walker.add_actor(*resolved->first, resolved->second);
    walker.add_loose_components(pkg);
    walker.finish();
    return out;
}

void write_placements_json(const CellPlacements& cell, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
        return o;
    };
    f << "{\"cell\":\"" << esc(cell.cell) << "\",\"hidden\":" << cell.hidden
      << ",\"unresolved\":" << cell.unresolved << ",\"unsupported\":" << cell.unsupported
      << ",\"entries\":[";
    for (size_t i = 0; i < cell.entries.size(); ++i) {
        const Placement& p = cell.entries[i];
        if (i) f << ",";
        f << "{\"kind\":\"" << esc(p.kind) << "\",\"mesh\":\"" << esc(p.mesh)
          << "\",\"component\":\"" << esc(p.component) << "\",\"materials\":[";
        for (size_t j = 0; j < p.materials.size(); ++j)
            f << (j ? "," : "") << "\"" << esc(p.materials[j]) << "\"";
        f << "],\"xforms\":[";
        for (size_t j = 0; j < p.xforms.size(); ++j) f << (j ? "," : "") << p.xforms[j];
        f << "]}";
    }
    f << "]}";
}

}  // namespace bl4
