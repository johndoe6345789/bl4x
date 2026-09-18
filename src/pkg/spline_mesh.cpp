#include "pkg/spline_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bl4 {
namespace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalized(V3 a, V3 fallback) {
    const double len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return len > 1e-9 ? a * (1.0 / len) : fallback;
}
double get(const V3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
V3 at(const std::vector<float>& p, size_t i) { return {p[i], p[i + 1], p[i + 2]}; }

// UE's SplineEvalPos / SplineEvalDir: cubic Hermite and its derivative.
V3 hermite(V3 p0, V3 t0, V3 p1, V3 t1, double a) {
    const double a2 = a * a, a3 = a2 * a;
    return p0 * (2 * a3 - 3 * a2 + 1) + t0 * (a3 - 2 * a2 + a) + p1 * (-2 * a3 + 3 * a2) +
           t1 * (a3 - a2);
}
V3 hermite_dir(V3 p0, V3 t0, V3 p1, V3 t1, double a) {
    const double a2 = a * a;
    return p0 * (6 * a2 - 6 * a) + t0 * (3 * a2 - 4 * a + 1) + p1 * (-6 * a2 + 6 * a) +
           t1 * (3 * a2 - 2 * a);
}

struct Slice {
    V3 origin, dir, x, y;  // spline point, forward, and the two cross axes
    double sx = 1, sy = 1;
};

Slice slice_at(const std::vector<float>& p, double alpha) {
    const V3 p0 = at(p, 0), t0 = at(p, 3), p1 = at(p, 6), t1 = at(p, 9);
    const bool smooth = p[28] != 0.f;
    const double h = smooth ? alpha * alpha * (3 - 2 * alpha) : alpha;
    Slice s;
    s.origin = hermite(p0, t0, p1, t1, alpha);
    s.dir = normalized(hermite_dir(p0, t0, p1, t1, alpha), normalized(p1 - p0, {1, 0, 0}));
    const V3 up = at(p, 23);
    const V3 base_x = normalized(cross(up, s.dir), {0, 1, 0});
    const V3 base_y = normalized(cross(s.dir, base_x), {0, 0, 1});
    const double off_x = p[16] + (p[18] - p[16]) * h, off_y = p[17] + (p[19] - p[17]) * h;
    s.origin = s.origin + base_x * off_x + base_y * off_y;
    const double roll = p[20] + (p[21] - p[20]) * h;
    const double c = std::cos(roll), sn = std::sin(roll);
    s.x = base_x * c - base_y * sn;
    s.y = base_y * c + base_x * sn;
    s.sx = p[12] + (p[14] - p[12]) * h;
    s.sy = p[13] + (p[15] - p[13]) * h;
    return s;
}

}  // namespace

void deform_spline_mesh(MeshData& mesh, const std::vector<float>& p) {
    if (p.size() < 29 || mesh.vertices.empty()) return;
    const int axis = std::clamp(static_cast<int>(p[22]), 0, 2);
    double lo = p[26], hi = p[27];
    if (hi - lo == 0.0) {  // unset: the mesh's own extent along the axis
        lo = 1e30; hi = -1e30;
        for (const auto& v : mesh.vertices) {
            const double f = get({v.px, v.py, v.pz}, axis);
            lo = std::min(lo, f); hi = std::max(hi, f);
        }
    }
    const double span = hi - lo == 0.0 ? 1.0 : hi - lo;
    for (auto& v : mesh.vertices) {
        const V3 pos{v.px, v.py, v.pz}, nrm{v.nx, v.ny, v.nz};
        const Slice s = slice_at(p, (get(pos, axis) - lo) / span);
        // The cross-section's two local axes, per ForwardAxis (X: Y,Z; Y:
        // X,Z; Z: X,Y), map onto the slice frame the way UE's switch does.
        V3 out, n;
        if (axis == 0) {
            out = s.origin + s.x * (pos.y * s.sx) + s.y * (pos.z * s.sy);
            n = s.dir * nrm.x + s.x * nrm.y + s.y * nrm.z;
        } else if (axis == 1) {
            out = s.origin + s.y * (pos.x * s.sy) + s.x * (pos.z * s.sx);
            n = s.y * nrm.x + s.dir * nrm.y + s.x * nrm.z;
        } else {
            out = s.origin + s.x * (pos.x * s.sx) + s.y * (pos.y * s.sy);
            n = s.x * nrm.x + s.y * nrm.y + s.dir * nrm.z;
        }
        n = normalized(n, {0, 0, 1});
        v.px = float(out.x); v.py = float(out.y); v.pz = float(out.z);
        v.nx = float(n.x); v.ny = float(n.y); v.nz = float(n.z);
    }
}

MeshData triangulate_lake(const std::vector<float>& outline) {
    MeshData mesh;
    const size_t count = outline.size() / 3;
    if (count < 3) return mesh;
    struct P { double x, y; };
    std::vector<P> pts(count);
    double area = 0;
    for (size_t i = 0; i < count; ++i) pts[i] = {outline[i * 3], outline[i * 3 + 1]};
    for (size_t i = 0; i < count; ++i) {
        const P& a = pts[i];
        const P& b = pts[(i + 1) % count];
        area += a.x * b.y - b.x * a.y;
    }
    // Ear clipping over a counter-clockwise loop (in UE's XY plane).
    std::vector<uint32_t> ring(count);
    for (size_t i = 0; i < count; ++i) ring[i] = static_cast<uint32_t>(area > 0 ? i : count - 1 - i);
    auto cross2 = [&](uint32_t a, uint32_t b, uint32_t c) {
        return (pts[b].x - pts[a].x) * (pts[c].y - pts[a].y) -
               (pts[b].y - pts[a].y) * (pts[c].x - pts[a].x);
    };
    auto inside = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t q) {
        return cross2(a, b, q) >= 0 && cross2(b, c, q) >= 0 && cross2(c, a, q) >= 0;
    };
    std::vector<uint32_t> tris;
    size_t guard = count * count;
    while (ring.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < ring.size(); ++i) {
            const uint32_t a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i],
                           c = ring[(i + 1) % ring.size()];
            if (cross2(a, b, c) <= 0) continue;  // reflex corner
            bool ear = true;
            for (uint32_t q : ring)
                if (q != a && q != b && q != c && inside(a, b, c, q)) { ear = false; break; }
            if (!ear) continue;
            tris.insert(tris.end(), {a, b, c});
            ring.erase(ring.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;  // self-intersecting outline: keep what we have
    }
    if (ring.size() == 3) tris.insert(tris.end(), {ring[0], ring[1], ring[2]});
    if (tris.empty()) return mesh;

    for (size_t i = 0; i < count; ++i) {
        MeshVertex v;
        v.px = outline[i * 3]; v.py = outline[i * 3 + 1]; v.pz = outline[i * 3 + 2];
        v.nx = 0; v.ny = 0; v.nz = 1;
        v.uv[0] = {v.px / 1000.f, v.py / 1000.f};  // one texture repeat every 10 m
        mesh.vertices.push_back(v);
    }
    mesh.indices = std::move(tris);
    mesh.sections.push_back({0, 0, static_cast<uint32_t>(mesh.indices.size() / 3)});
    mesh.num_tex_coords = 1;
    return mesh;
}

}  // namespace bl4
