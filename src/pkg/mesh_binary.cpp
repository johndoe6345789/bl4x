#include "pkg/mesh_binary.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "pkg/mesh.hpp"

namespace bl4 {
namespace {

struct Part {
    std::string texture;
    std::vector<float> vertices;  // 10 per vertex
    std::vector<uint32_t> indices;
};

void put_u32(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void put_f32(std::ofstream& f, float v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void append_vertex(std::vector<float>& out, const MeshVertex& v) {
    const float fields[10] = {v.px, v.py, v.pz, v.uv[0].u, v.uv[0].v,
                              v.uv[1].u, v.uv[1].v, v.nx, v.ny, v.nz};
    out.insert(out.end(), fields, fields + 10);
}

void write_parts(const std::string& path, const std::vector<Part>& parts,
                 const float bounds[6]) {
    std::ofstream f(path, std::ios::binary);
    f.write("BL4M", 4);
    put_u32(f, 1);
    put_u32(f, static_cast<uint32_t>(parts.size()));
    for (int i = 0; i < 6; ++i) put_f32(f, bounds[i]);
    for (const Part& part : parts) {
        put_u32(f, static_cast<uint32_t>(part.vertices.size() / 10));
        put_u32(f, static_cast<uint32_t>(part.indices.size()));
        put_u32(f, static_cast<uint32_t>(part.texture.size()));
        f.write(part.texture.data(), static_cast<std::streamsize>(part.texture.size()));
        f.write(reinterpret_cast<const char*>(part.vertices.data()),
                static_cast<std::streamsize>(part.vertices.size() * sizeof(float)));
        f.write(reinterpret_cast<const char*>(part.indices.data()),
                static_cast<std::streamsize>(part.indices.size() * sizeof(uint32_t)));
    }
}

void grow_bounds(float bounds[6], bool& any, float x, float y, float z) {
    const float p[3] = {x, y, z};
    for (int a = 0; a < 3; ++a) {
        bounds[a] = any ? std::min(bounds[a], p[a]) : p[a];
        bounds[a + 3] = any ? std::max(bounds[a + 3], p[a]) : p[a];
    }
    any = true;
}

}  // namespace

void write_binary_mesh(const MeshData& mesh, const std::string& path,
                       const std::vector<std::string>& texture_files) {
    std::vector<Part> parts;
    float bounds[6] = {0, 0, 0, 0, 0, 0};
    bool any = false;
    for (const MeshSection& section : mesh.sections) {
        Part part;
        if (section.material_index < texture_files.size())
            part.texture = texture_files[section.material_index];
        // One vertex buffer per section, indices remapped into it.
        std::unordered_map<uint32_t, uint32_t> remap;
        for (uint32_t t = 0; t < section.num_triangles * 3; ++t) {
            const size_t at = section.first_index + t;
            if (at >= mesh.indices.size()) break;
            const uint32_t source = mesh.indices[at];
            if (source >= mesh.vertices.size()) continue;
            auto [it, inserted] = remap.emplace(source, static_cast<uint32_t>(remap.size()));
            if (inserted) {
                const MeshVertex& v = mesh.vertices[source];
                append_vertex(part.vertices, v);
                grow_bounds(bounds, any, v.px, v.py, v.pz);
            }
            part.indices.push_back(it->second);
        }
        if (!part.indices.empty()) parts.push_back(std::move(part));
    }
    write_parts(path, parts, bounds);
}

bool convert_obj_to_binary(const std::string& obj_path, const std::string& out_path) {
    std::ifstream in(obj_path);
    if (!in) return false;

    // The .mtl beside it holds one map_Kd per "mat_<n>" material.
    std::unordered_map<std::string, std::string> material_textures;
    {
        std::ifstream mtl(obj_path.substr(0, obj_path.size() - 4) + ".mtl");
        std::string line, current;
        while (std::getline(mtl, line)) {
            std::istringstream words(line);
            std::string tag;
            words >> tag;
            if (tag == "newmtl") words >> current;
            else if (tag == "map_Kd") words >> material_textures[current];
        }
    }

    std::vector<float> px, py, pz, tu, tv, nx, ny, nz;
    std::vector<Part> parts;
    std::unordered_map<std::string, uint32_t> remap;  // "v/vt/vn" -> part index
    float bounds[6] = {0, 0, 0, 0, 0, 0};
    bool any = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() < 2) continue;
        std::istringstream words(line);
        std::string tag;
        words >> tag;
        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            words >> x >> y >> z;
            px.push_back(x); py.push_back(y); pz.push_back(z);
        } else if (tag == "vt") {
            float u = 0, v = 0;
            words >> u >> v;
            tu.push_back(u); tv.push_back(v);
        } else if (tag == "vn") {
            float x = 0, y = 0, z = 0;
            words >> x >> y >> z;
            nx.push_back(x); ny.push_back(y); nz.push_back(z);
        } else if (tag == "usemtl") {
            std::string name;
            words >> name;
            parts.push_back({});
            parts.back().texture = material_textures.count(name) ? material_textures[name] : "";
            remap.clear();
        } else if (tag == "f") {
            if (parts.empty()) { parts.push_back({}); remap.clear(); }
            Part& part = parts.back();
            std::string vertex;
            int corner = 0;
            while (words >> vertex && corner < 3) {
                auto [it, inserted] = remap.emplace(vertex, static_cast<uint32_t>(remap.size()));
                if (inserted) {
                    // "p/t/n", all 1-based; bl4x's write_obj always emits all three.
                    int p = 0, t = 0, n = 0;
                    std::replace(vertex.begin(), vertex.end(), '/', ' ');
                    std::istringstream fields(vertex);
                    fields >> p >> t >> n;
                    const size_t vi = static_cast<size_t>(p - 1);
                    const size_t ti = static_cast<size_t>(t - 1);
                    const size_t ni = static_cast<size_t>(n - 1);
                    if (vi >= px.size()) return false;
                    const float row[10] = {px[vi], py[vi], pz[vi],
                                           ti < tu.size() ? tu[ti] : 0.f,
                                           ti < tv.size() ? tv[ti] : 0.f,
                                           0.f, 0.f,
                                           ni < nx.size() ? nx[ni] : 0.f,
                                           ni < ny.size() ? ny[ni] : 1.f,
                                           ni < nz.size() ? nz[ni] : 0.f};
                    part.vertices.insert(part.vertices.end(), row, row + 10);
                    grow_bounds(bounds, any, row[0], row[1], row[2]);
                }
                part.indices.push_back(it->second);
                ++corner;
            }
        }
    }
    std::vector<Part> kept;
    for (Part& part : parts)
        if (!part.indices.empty()) kept.push_back(std::move(part));
    write_parts(out_path, kept, bounds);
    return true;
}

}  // namespace bl4
