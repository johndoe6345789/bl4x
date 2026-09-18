#include "tools/coverage_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace bl4 {
namespace {

struct Instance {
    uint32_t model;
    float t[3], q[4], s[3];
};

struct Grid {
    float x0, z0, cell;
    int w, h;
    std::vector<uint8_t> bits;
};

// Every triangle of a .bmesh, 9 floats each.
std::vector<float> read_triangles(const std::string& path) {
    std::vector<float> out;
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    if (!f.read(magic, 4) || std::memcmp(magic, "BL4M", 4) != 0) return out;
    uint32_t version = 0, parts = 0;
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&parts), 4);
    f.seekg(24, std::ios::cur);  // bounds
    for (uint32_t p = 0; p < parts && f; ++p) {
        uint32_t vc = 0, ic = 0, tl = 0;
        f.read(reinterpret_cast<char*>(&vc), 4);
        f.read(reinterpret_cast<char*>(&ic), 4);
        f.read(reinterpret_cast<char*>(&tl), 4);
        f.seekg(tl, std::ios::cur);
        std::vector<float> verts(static_cast<size_t>(vc) * 10);
        std::vector<uint32_t> idx(ic);
        f.read(reinterpret_cast<char*>(verts.data()), static_cast<std::streamsize>(verts.size() * 4));
        f.read(reinterpret_cast<char*>(idx.data()), static_cast<std::streamsize>(idx.size() * 4));
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = idx[i + k];
                if (v >= vc) continue;
                out.insert(out.end(), &verts[v * 10], &verts[v * 10] + 3);
            }
    }
    return out;
}

void transform(const Instance& in, const float* p, float* o) {
    const float v[3] = {p[0] * in.s[0], p[1] * in.s[1], p[2] * in.s[2]};
    const float qx = in.q[0], qy = in.q[1], qz = in.q[2], qw = in.q[3];
    const float t[3] = {2 * (qy * v[2] - qz * v[1]), 2 * (qz * v[0] - qx * v[2]),
                        2 * (qx * v[1] - qy * v[0])};
    o[0] = v[0] + qw * t[0] + (qy * t[2] - qz * t[1]) + in.t[0];
    o[1] = v[1] + qw * t[1] + (qz * t[0] - qx * t[2]) + in.t[1];
    o[2] = v[2] + qw * t[2] + (qx * t[1] - qy * t[0]) + in.t[2];
}

// Marks cells whose centre lies inside the triangle's XZ projection,
// plus its three corners so slivers still register.
void rasterize(Grid& g, const float* a, const float* b, const float* c, uint8_t bit) {
    const float ax = (a[0] - g.x0) / g.cell, az = (a[2] - g.z0) / g.cell;
    const float bx = (b[0] - g.x0) / g.cell, bz = (b[2] - g.z0) / g.cell;
    const float cx = (c[0] - g.x0) / g.cell, cz = (c[2] - g.z0) / g.cell;
    auto mark = [&](int x, int z) {
        if (x >= 0 && z >= 0 && x < g.w && z < g.h) g.bits[static_cast<size_t>(z) * g.w + x] |= bit;
    };
    mark(static_cast<int>(std::floor(ax)), static_cast<int>(std::floor(az)));
    mark(static_cast<int>(std::floor(bx)), static_cast<int>(std::floor(bz)));
    mark(static_cast<int>(std::floor(cx)), static_cast<int>(std::floor(cz)));
    const float d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz);
    if (std::fabs(d) < 1e-9f) return;
    const int x_lo = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int x_hi = std::min(g.w - 1, static_cast<int>(std::floor(std::max({ax, bx, cx}))));
    const int z_lo = std::max(0, static_cast<int>(std::floor(std::min({az, bz, cz}))));
    const int z_hi = std::min(g.h - 1, static_cast<int>(std::floor(std::max({az, bz, cz}))));
    for (int z = z_lo; z <= z_hi; ++z) {
        const float pz = z + 0.5f;
        for (int x = x_lo; x <= x_hi; ++x) {
            const float px = x + 0.5f;
            const float w0 = ((bz - cz) * (px - cx) + (cx - bx) * (pz - cz)) / d;
            const float w1 = ((cz - az) * (px - cx) + (ax - cx) * (pz - cz)) / d;
            if (w0 >= -1e-4f && w1 >= -1e-4f && 1.f - w0 - w1 >= -1e-4f)
                g.bits[static_cast<size_t>(z) * g.w + x] |= bit;
        }
    }
}

}  // namespace

int run_coverage_raster(const std::vector<std::string>& args) {
    if (args.size() < 9) {
        std::cerr << "coverage-raster instances.bin models.txt bake_dir cell x0 z0 w h out.raw\n";
        return 1;
    }
    std::vector<std::string> models;
    std::vector<uint8_t> is_terrain;
    {
        std::ifstream f(args[1]);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            is_terrain.push_back(line[0] == 'T' ? 1 : 0);
            models.push_back(line.substr(2));
        }
    }
    std::vector<Instance> instances;
    {
        std::ifstream f(args[0], std::ios::binary);
        Instance in{};
        while (f.read(reinterpret_cast<char*>(&in), sizeof(in))) instances.push_back(in);
    }
    std::sort(instances.begin(), instances.end(),
              [](const Instance& a, const Instance& b) { return a.model < b.model; });

    Grid g;
    g.cell = std::stof(args[3]);
    g.x0 = std::stof(args[4]);
    g.z0 = std::stof(args[5]);
    g.w = std::stoi(args[6]);
    g.h = std::stoi(args[7]);
    g.bits.assign(static_cast<size_t>(g.w) * g.h, 0);

    std::vector<float> tris;
    uint32_t loaded = UINT32_MAX;
    long long done = 0, triangles = 0;
    for (const Instance& in : instances) {
        if (in.model != loaded) {
            tris = read_triangles(args[2] + "/" + models[in.model]);
            loaded = in.model;
        }
        const uint8_t bit = is_terrain[in.model] ? 1 : 2;
        for (size_t i = 0; i + 8 < tris.size(); i += 9) {
            float a[3], b[3], c[3];
            transform(in, &tris[i], a);
            transform(in, &tris[i + 3], b);
            transform(in, &tris[i + 6], c);
            // Upward-facing only: walls and ceilings hold nobody up.
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float nx = e1[1] * e2[2] - e1[2] * e2[1];
            const float ny = e1[2] * e2[0] - e1[0] * e2[2];
            const float nz = e1[0] * e2[1] - e1[1] * e2[0];
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len <= 0.f || ny / len < 0.5f) continue;
            rasterize(g, a, b, c, bit);
            ++triangles;
        }
        if (++done % 100000 == 0)
            std::cout << "  " << done << "/" << instances.size() << " instances\n" << std::flush;
    }
    std::ofstream out(args[8], std::ios::binary);
    out.write(reinterpret_cast<const char*>(g.bits.data()), static_cast<std::streamsize>(g.bits.size()));
    std::cout << "rasterized " << done << " instances, " << triangles << " walkable triangles\n";
    return 0;
}

}  // namespace bl4
