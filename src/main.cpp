// bl4x: a from-scratch, memory-lean reader for Borderlands 4's IoStore
// archives, built to replace the CUE4Parse-based C# exporter for the
// structural (container/package/import/export) layer. See README for
// what's implemented so far and what the next phase needs.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "io/oodle.hpp"
#include "pkg/material.hpp"
#include "pkg/mesh.hpp"
#include "pkg/mesh_binary.hpp"
#include "pkg/mesh_simplify.hpp"
#include "pkg/spline_mesh.hpp"
#include "pkg/object.hpp"
#include "pkg/package.hpp"
#include "pkg/provider.hpp"
#include "pkg/texture.hpp"
#include "pkg/texture_export.hpp"
#include "pkg/usmap.hpp"
#include "tools/coverage_raster.hpp"
#include "world/walker.hpp"

namespace {

constexpr const char* kPaksDir =
    "D:/SteamLibrary/steamapps/common/Borderlands 4/OakGame/Content/Paks";
constexpr const char* kUsmapPath = "D:/BL4Export/borderlands.usmap";

int cmd_index(bl4::Provider& p) {
    std::cout << "containers: " << p.container_count()
              << "  files: " << p.file_count() << "\n";
    return 0;
}

// Loads one package and prints its export classes, grouped, matching
// the shape of the earlier C# `census` dump for eyeball comparison.
int cmd_dump(bl4::Provider& p, const std::string& path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) {
        std::cerr << "not found: " << path << "\n";
        return 1;
    }
    std::cout << pkg->name() << "  exports=" << pkg->export_count()
              << "  bulkData=" << pkg->bulk_data_map().size() << "\n";
    std::map<std::string, int> counts;
    for (size_t i = 0; i < pkg->export_count(); ++i)
        counts[pkg->resolve_object_name(pkg->export_at(i).class_index)]++;
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
             [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [name, n] : sorted) std::cout << "  " << n << "\t" << name << "\n";
    return 0;
}

// Every World_P cell package, sorted -- the whole map, as `bake-all`
// and the census commands enumerate it.
std::vector<std::string> world_p_cells(bl4::Provider& p) {
    std::vector<std::string> umaps;
    for (auto& c : p.find_paths("world_p/_generated_/"))
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0) umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());
    return umaps;
}

// Walks every _Generated_ cell package under World_P and tallies export
// classes across all of them, to sanity-check package/import/export
// parsing against real data at scale (compare with class_census.txt).
int cmd_census(bl4::Provider& p, int limit) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cells = p.find_paths("world_p/_generated_/");
    std::vector<std::string> umaps;
    for (auto& c : cells)
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0)
            umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());
    if (limit > 0 && static_cast<size_t>(limit) < umaps.size())
        umaps.resize(static_cast<size_t>(limit));

    std::map<std::string, long long> counts;
    int failed = 0;
    for (auto& path : umaps) {
        try {
            bl4::Package* pkg = p.load_package(path);
            if (!pkg) { ++failed; continue; }
            for (size_t i = 0; i < pkg->export_count(); ++i)
                counts[pkg->resolve_object_name(pkg->export_at(i).class_index)]++;
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL " << path << ": " << e.what() << "\n";
        }
    }
    std::vector<std::pair<std::string, long long>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
             [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [name, n] : sorted) std::cout << n << "\t" << name << "\n";
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cerr << umaps.size() << " cells, " << failed << " failed, "
              << secs << "s\n";
    return 0;
}

// Prints every scene component's transform and (for ISM-family
// classes) raw per-instance transforms, for cross-checking against
// the C# exporter's already-produced placement JSON for the same cell.
int cmd_props(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path,
             int only_index = -1) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    for (uint32_t i = 0; i < pkg->export_count(); ++i) {
        if (only_index >= 0 && static_cast<int>(i) != only_index) continue;
        std::string cls = pkg->resolve_object_name(pkg->export_at(i).class_index);
        if (cls.find("Component") == std::string::npos) continue;
        try {
            if (std::getenv("BL4X_TRACE")) {
                bl4::PropertyBag pb = bl4::load_properties(*pkg, usmap, i);
                std::cerr << "  bComputeBoundsOnceForGame=" << pb.get_bool("bComputeBoundsOnceForGame")
                          << " bComputedBoundsOnceForGame=" << pb.get_bool("bComputedBoundsOnceForGame")
                          << "\n";
            }
            bl4::ComponentData c = bl4::load_component(*pkg, usmap, i, cls);
            bl4::ObjIndex mesh = c.props.get_object("StaticMesh");
            std::string mesh_name = mesh.is_null() ? "" : pkg->resolve_object_name(mesh);
            if (mesh_name.empty() && c.instances.empty()) continue;
            bl4::Vec3 loc = c.props.get_vec3("RelativeLocation");
            bl4::Vec3 rot = c.props.get_vec3("RelativeRotation");
            bl4::Vec3 scale = c.props.get_vec3("RelativeScale3D", {1, 1, 1});
            std::cerr << "  bComputeBoundsOnceForGame=" << c.props.get_bool("bComputeBoundsOnceForGame")
                      << " bComputedBoundsOnceForGame=" << c.props.get_bool("bComputedBoundsOnceForGame")
                      << "\n";
            std::cout << i << " " << cls << " mesh=" << mesh_name
                      << " loc=(" << loc.x << "," << loc.y << "," << loc.z << ")"
                      << " rot=(" << rot.x << "," << rot.y << "," << rot.z << ")"
                      << " scale=(" << scale.x << "," << scale.y << "," << scale.z << ")"
                      << " instances=" << c.instances.size() << "\n";
            for (size_t k = 0; k < c.instances.size() && k < 2; ++k) {
                const auto& inst = c.instances[k];
                std::cout << "  [" << k << "] t=(" << inst.translation.x << ","
                          << inst.translation.y << "," << inst.translation.z << ")"
                          << " r=(" << inst.rotation.x << "," << inst.rotation.y << ","
                          << inst.rotation.z << "," << inst.rotation.w << ")"
                          << " s=(" << inst.scale.x << "," << inst.scale.y << ","
                          << inst.scale.z << ")\n";
            }
        } catch (const std::exception& e) {
            std::cerr << i << " " << cls << ": FAIL " << e.what() << "\n";
        }
    }
    return 0;
}

// Walks every component export in `limit` cells, decoding properties
// (and instance transforms for ISM-family classes), to sanity-check
// the property system at scale: no crashes, no NaN/inf/huge values.
int cmd_census_props(bl4::Provider& p, const bl4::Usmap& usmap, int limit) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cells = p.find_paths("world_p/_generated_/");
    std::vector<std::string> umaps;
    for (auto& c : cells)
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0) umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());
    if (limit > 0 && static_cast<size_t>(limit) < umaps.size()) umaps.resize(static_cast<size_t>(limit));

    long long components = 0, instances = 0, failed = 0, bad_values = 0;
    for (auto& path : umaps) {
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) { ++failed; continue; }
        for (uint32_t i = 0; i < pkg->export_count(); ++i) {
            std::string cls = pkg->resolve_object_name(pkg->export_at(i).class_index);
            if (cls.find("Component") == std::string::npos) continue;
            try {
                bl4::ComponentData c = bl4::load_component(*pkg, usmap, i, cls);
                ++components;
                for (auto& inst : c.instances) {
                    ++instances;
                    bool ok = std::isfinite(inst.translation.x) && std::isfinite(inst.translation.y) &&
                             std::isfinite(inst.translation.z) &&
                             std::abs(inst.translation.x) < 1e7 && std::abs(inst.translation.y) < 1e7 &&
                             std::abs(inst.translation.z) < 1e7 && std::isfinite(inst.scale.x) &&
                             std::abs(inst.scale.x) < 1e4 && std::isfinite(inst.rotation.w);
                    if (!ok) {
                        ++bad_values;
                        if (bad_values <= 5)
                            std::cerr << "BAD " << path << " export " << i << " " << cls << "\n";
                    }
                }
            } catch (const std::exception& e) {
                ++failed;
                if (failed <= 20) std::cerr << "FAIL " << path << " " << i << " " << cls << ": " << e.what() << "\n";
            }
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "cells=" << umaps.size() << " components=" << components
              << " instances=" << instances << " failed=" << failed
              << " badValues=" << bad_values << " secs=" << secs << "\n";
    return 0;
}

int cmd_walk(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path,
            const std::string& out_path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    bl4::CellPlacements cell = bl4::walk_cell(*pkg, usmap, path);
    bl4::write_placements_json(cell, out_path);
    std::cout << "entries=" << cell.entries.size() << " hidden=" << cell.hidden
              << " unresolved=" << cell.unresolved << " unsupported=" << cell.unsupported << "\n";
    return 0;
}

// Walks every cell (or the first `limit`) end to end, to sanity-check
// the full placement pipeline at scale: no crashes, no NaN/inf/huge
// transforms, total instance counts stay reasonable.
int cmd_walk_census(bl4::Provider& p, const bl4::Usmap& usmap, int limit) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cells = p.find_paths("world_p/_generated_/");
    std::vector<std::string> umaps;
    for (auto& c : cells)
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0) umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());
    if (limit > 0 && static_cast<size_t>(limit) < umaps.size()) umaps.resize(static_cast<size_t>(limit));

    long long placements = 0, instances = 0, failed = 0, bad = 0;
    for (auto& path : umaps) {
        try {
            bl4::Package* pkg = p.load_package(path);
            if (!pkg) { ++failed; continue; }
            bl4::CellPlacements cell = bl4::walk_cell(*pkg, usmap, path);
            placements += static_cast<long long>(cell.entries.size());
            for (auto& e : cell.entries) {
                for (size_t i = 0; i < e.xforms.size(); i += 10) {
                    ++instances;
                    bool ok = true;
                    for (int k = 0; k < 10; ++k)
                        ok &= std::isfinite(e.xforms[i + k]) && std::abs(e.xforms[i + k]) < 1e7f;
                    if (!ok) { ++bad; if (bad <= 5) std::cerr << "BAD " << path << " " << e.mesh << "\n"; }
                }
            }
        } catch (const std::exception& ex) {
            ++failed;
            if (failed <= 20) std::cerr << "FAIL " << path << ": " << ex.what() << "\n";
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "cells=" << umaps.size() << " placements=" << placements
              << " instances=" << instances << " failed=" << failed
              << " bad=" << bad << " secs=" << secs << "\n";
    return 0;
}

// Finds World_P cells holding an export of `class_name` -- how a cell
// with landscape (or any other class worth a look) gets located, given
// the cells' hashed names say nothing about their contents.
// One line per landscape-bearing cell: where its landscape components
// sit (engine-space metres, like the bake), how many there are, and
// whether a Nanite terrain mesh exists for them -- the bake's only source
// of ground. A cell with heightfield components but no Nanite mesh is a
// hole in the baked world.
int cmd_landscape_census(bl4::Provider& p, const bl4::Usmap& usmap) {
    std::cout << "cell,landscape,nanite,collision,water,min_x,min_z,max_x,max_z" << "\n";
    for (const auto& path : world_p_cells(p)) {
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) continue;
        int landscape = 0, nanite = 0, collision = 0, water = 0;
        double lo_x = 1e30, lo_z = 1e30, hi_x = -1e30, hi_z = -1e30;
        for (uint32_t i = 0; i < pkg->export_count(); ++i) {
            const std::string cls = pkg->resolve_object_name(pkg->export_at(i).class_index);
            if (cls == "LandscapeNaniteComponent") ++nanite;
            else if (cls == "LandscapeHeightfieldCollisionComponent") ++collision;
            else if (cls.rfind("WaterBody", 0) == 0 && cls.find("Component") != std::string::npos) ++water;
            if (cls != "LandscapeComponent") continue;
            ++landscape;
            try {
                bl4::ComponentData c = bl4::load_component(*pkg, usmap, i, cls);
                const bl4::Vec3 loc = c.props.get_vec3("RelativeLocation");
                // Components attach to their proxy at its origin; section
                // bases are baked into RelativeLocation for cooked data.
                const double x = loc.x * 0.01, z = loc.y * 0.01;
                lo_x = std::min(lo_x, x); lo_z = std::min(lo_z, z);
                hi_x = std::max(hi_x, x); hi_z = std::max(hi_z, z);
            } catch (const std::exception&) {
            }
        }
        if (landscape == 0 && nanite == 0) continue;
        std::cout << path << "," << landscape << "," << nanite << "," << collision << "," << water
                  << "," << lo_x << "," << lo_z << "," << hi_x << "," << hi_z << "\n";
    }
    return 0;
}

int cmd_find_class(bl4::Provider& p, const std::string& class_name, int max_hits) {
    int hits = 0;
    for (const auto& path : world_p_cells(p)) {
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) continue;
        for (uint32_t i = 0; i < pkg->export_count(); ++i) {
            if (pkg->resolve_object_name(pkg->export_at(i).class_index) != class_name) continue;
            std::cout << path << " [" << i << "] "
                      << pkg->resolve(pkg->export_at(i).object_name) << "\n";
            if (++hits >= max_hits && max_hits > 0) return 0;
            break;
        }
    }
    return 0;
}

int cmd_find_export(bl4::Provider& p, const std::string& path, const std::string& class_name) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    for (uint32_t i = 0; i < pkg->export_count(); ++i) {
        if (pkg->resolve_object_name(pkg->export_at(i).class_index) == class_name)
            std::cout << i << " " << pkg->resolve(pkg->export_at(i).object_name) << "\n";
    }
    return 0;
}

void print_value(bl4::Package& pkg, const bl4::PropertyValue& v, int depth) {
    using K = bl4::PropertyValue::Kind;
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    switch (v.kind) {
    case K::Bool: std::cout << v.b; break;
    case K::Int: std::cout << v.i; break;
    case K::Double: std::cout << v.d; break;
    case K::Name: case K::Str: std::cout << '"' << v.s << '"'; break;
    case K::Object: std::cout << (v.obj.is_null() ? "null" : pkg.resolve_asset_path(v.obj)); break;
    case K::Vec3: std::cout << "(" << v.v3.x << "," << v.v3.y << "," << v.v3.z << ")"; break;
    case K::Vec4: std::cout << "(" << v.v4.x << "," << v.v4.y << "," << v.v4.z << "," << v.v4.w << ")"; break;
    case K::Array:
        std::cout << "[" << v.arr.size() << "]";
        for (size_t i = 0; i < v.arr.size(); ++i) {
            std::cout << "\n" << pad << "  #" << i << " ";
            print_value(pkg, v.arr[i], depth + 2);
        }
        break;
    case K::Bag:
        std::cout << "{";
        for (auto& [k, sub] : v.bag->all()) {
            std::cout << "\n" << pad << "  " << k << " = ";
            print_value(pkg, sub, depth + 2);
        }
        std::cout << "}";
        break;
    default: std::cout << "?"; break;
    }
}

// Prints any export's full property tree -- for finding where a class
// keeps what we need (e.g. a water body's surface mesh).
int cmd_obj(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path, int index) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    const auto idx = static_cast<uint32_t>(index);
    std::cout << pkg->resolve_object_name(pkg->export_at(idx).class_index) << "\n";
    bl4::PropertyBag props = bl4::load_properties(*pkg, usmap, idx);
    for (auto& [k, v] : props.all()) {
        std::cout << "  " << k << " = ";
        print_value(*pkg, v, 1);
        std::cout << "\n";
    }
    return 0;
}

// Prints a material (instance) export's properties, then its Parent's,
// up the chain -- a probe for where BL4 keeps its texture parameters.
int cmd_material(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    for (int hop = 0; pkg && hop < 8; ++hop) {
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < pkg->export_count(); ++i) {
            std::string cls = pkg->resolve_object_name(pkg->export_at(i).class_index);
            if (cls.find("Material") != std::string::npos) { idx = i; break; }
        }
        if (idx == UINT32_MAX) { std::cout << "no material export in " << pkg->path() << "\n"; return 1; }
        std::string cls = pkg->resolve_object_name(pkg->export_at(idx).class_index);
        std::cout << "== " << pkg->path() << " [" << idx << "] " << cls << "\n";
        bl4::PropertyBag props;
        try { props = bl4::load_properties(*pkg, usmap, idx); }
        catch (const std::exception& e) { std::cout << "  FAIL " << e.what() << "\n"; return 1; }
        for (auto& [k, v] : props.all()) {
            std::cout << "  " << k << " = ";
            print_value(*pkg, v, 1);
            std::cout << "\n";
        }
        bl4::ObjIndex parent = props.get_object("Parent");
        if (parent.is_null()) break;
        auto target = pkg->resolve_export(parent);
        pkg = target ? target->first : nullptr;
        if (!pkg) std::cout << "(parent unresolved: " << props.get_object("Parent").raw << ")\n";
    }
    return 0;
}

int cmd_raw_props(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path, int index) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    bl4::PropertyBag props = bl4::load_properties(*pkg, usmap, static_cast<uint32_t>(index));
    (void)props;
    return 0;
}

int cmd_texture(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path,
                int index, const std::string& out_path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    bl4::TextureData tex = bl4::load_texture(*pkg, usmap, static_cast<uint32_t>(index));
    std::cout << tex.width << "x" << tex.height << " " << tex.pixel_format
              << " mips=" << tex.mips.size() << " srgb=" << tex.srgb << "\n";
    bl4::write_dds(tex, out_path);
    std::cout << "wrote " << out_path << "\n";
    return 0;
}

// Loads every Texture2D export in `limit` cells (plus their imported
// texture packages, reached via the mesh materials scan), to sanity-
// check texture decode at scale.
int cmd_texture_census(bl4::Provider& p, const bl4::Usmap& usmap, int limit) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> cells = p.find_paths("world_p/_generated_/");
    std::vector<std::string> umaps;
    for (auto& c : cells)
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0) umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());
    if (limit > 0 && static_cast<size_t>(limit) < umaps.size()) umaps.resize(static_cast<size_t>(limit));

    long long ok = 0, failed = 0, bytes = 0;
    std::map<std::string, int> failure_reasons;
    for (auto& path : umaps) {
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) continue;
        for (uint32_t i = 0; i < pkg->export_count(); ++i) {
            if (pkg->resolve_object_name(pkg->export_at(i).class_index) != "Texture2D") continue;
            try {
                bl4::TextureData tex = bl4::load_texture(*pkg, usmap, i);
                ++ok;
                for (auto& m : tex.mips) bytes += static_cast<long long>(m.data.size());
            } catch (const std::exception& e) {
                ++failed;
                std::string reason = e.what();
                size_t paren = reason.find('(');
                failure_reasons[paren != std::string::npos ? reason.substr(0, paren) : reason]++;
            }
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "cells=" << umaps.size() << " ok=" << ok << " failed=" << failed
              << " bytes=" << bytes << " secs=" << secs << "\n";
    for (auto& [reason, n] : failure_reasons) std::cout << "  " << n << "x " << reason << "\n";
    return 0;
}

// Loads export 0 of every path listed in a file (one per line) as a
// texture -- for sampling across many standalone texture packages
// rather than just what a handful of cells happen to embed.
int cmd_texture_batch(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& list_path) {
    std::ifstream in(list_path);
    std::string line;
    long long ok = 0, failed = 0, bytes = 0;
    std::map<std::string, int> failure_reasons;
    auto t0 = std::chrono::steady_clock::now();
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        bl4::Package* pkg = p.load_package(line);
        if (!pkg) { ++failed; failure_reasons["package not found"]++; continue; }
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < pkg->export_count(); ++i)
            if (pkg->resolve_object_name(pkg->export_at(i).class_index) == "Texture2D") { idx = i; break; }
        if (idx == UINT32_MAX) { ++failed; failure_reasons["no Texture2D export"]++; continue; }
        try {
            bl4::TextureData tex = bl4::load_texture(*pkg, usmap, idx);
            ++ok;
            for (auto& m : tex.mips) bytes += static_cast<long long>(m.data.size());
        } catch (const std::exception& e) {
            ++failed;
            std::string reason = e.what();
            size_t paren = reason.find('(');
            failure_reasons[paren != std::string::npos ? reason.substr(0, paren) : reason]++;
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "ok=" << ok << " failed=" << failed << " bytes=" << bytes << " secs=" << secs << "\n";
    for (auto& [reason, n] : failure_reasons) std::cout << "  " << n << "x " << reason << "\n";
    return 0;
}

// Loads export 0's StaticMesh (or the first one found) of every path
// listed in a file (one per line) -- for sampling geometry decode
// robustness across many standalone mesh packages.
int cmd_mesh_batch(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& list_path) {
    std::ifstream in(list_path);
    std::string line;
    long long ok = 0, failed = 0, tris = 0, nanite_ok = 0;
    std::map<std::string, int> failure_reasons;
    auto t0 = std::chrono::steady_clock::now();
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        bl4::Package* pkg = p.load_package(line);
        if (!pkg) { ++failed; failure_reasons["package not found"]++; continue; }
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < pkg->export_count(); ++i)
            if (pkg->resolve_object_name(pkg->export_at(i).class_index) == "StaticMesh") { idx = i; break; }
        if (idx == UINT32_MAX) { ++failed; failure_reasons["no StaticMesh export"]++; continue; }
        try {
            bl4::MeshData mesh = bl4::load_static_mesh(*pkg, usmap, idx);
            if (mesh.vertices.empty()) { ++failed; failure_reasons["empty mesh"]++; continue; }
            ++ok;
            if (mesh.is_nanite) ++nanite_ok;
            tris += static_cast<long long>(mesh.indices.size() / 3);
        } catch (const std::exception& e) {
            ++failed;
            std::string reason = e.what();
            size_t paren = reason.find('(');
            failure_reasons[paren != std::string::npos ? reason.substr(0, paren) : reason]++;
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "ok=" << ok << " (nanite=" << nanite_ok << ") failed=" << failed
              << " tris=" << tris << " secs=" << secs << "\n";
    for (auto& [reason, n] : failure_reasons) std::cout << "  " << n << "x " << reason << "\n";
    return 0;
}

// texture_files (parallel to mesh.material_paths, "" = none) adds a
// sibling .mtl binding each section's group to its base-colour map.
void write_obj(const bl4::MeshData& mesh, const std::string& path,
               const std::vector<std::string>* texture_files = nullptr) {
    std::ofstream out(path);
    if (texture_files) {
        std::filesystem::path obj(path);
        std::string mtl_name = obj.stem().string() + ".mtl";
        std::ofstream mtl(obj.parent_path() / mtl_name);
        for (size_t i = 0; i < texture_files->size(); ++i) {
            mtl << "newmtl mat_" << i << "\nKd 1 1 1\n";
            if (!(*texture_files)[i].empty()) mtl << "map_Kd " << (*texture_files)[i] << "\n";
        }
        out << "mtllib " << mtl_name << "\n";
    }
    for (auto& v : mesh.vertices) out << "v " << v.px << " " << v.py << " " << v.pz << "\n";
    for (auto& v : mesh.vertices) out << "vn " << v.nx << " " << v.ny << " " << v.nz << "\n";
    for (auto& v : mesh.vertices) out << "vt " << v.uv[0].u << " " << v.uv[0].v << "\n";
    for (auto& sec : mesh.sections) {
        out << "g section_" << sec.material_index << "\n";
        if (texture_files) out << "usemtl mat_" << sec.material_index << "\n";
        for (uint32_t t = 0; t < sec.num_triangles; ++t) {
            uint32_t base = sec.first_index + t * 3;
            if (base + 2 >= mesh.indices.size()) break;
            uint32_t a = mesh.indices[base] + 1, b = mesh.indices[base + 1] + 1, c = mesh.indices[base + 2] + 1;
            out << "f " << a << "/" << a << "/" << a << " " << b << "/" << b << "/" << b
                << " " << c << "/" << c << "/" << c << "\n";
        }
    }
}

int cmd_mesh(bl4::Provider& p, const bl4::Usmap& usmap, const std::string& path,
            int index, const std::string& out_path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) { std::cerr << "not found: " << path << "\n"; return 1; }
    bl4::MeshData mesh = bl4::load_static_mesh(*pkg, usmap, static_cast<uint32_t>(index));
    std::cout << "nanite=" << mesh.is_nanite << " verts=" << mesh.vertices.size()
              << " tris=" << mesh.indices.size() / 3 << " sections=" << mesh.sections.size()
              << " texcoords=" << mesh.num_tex_coords << "\n";
    for (size_t i = 0; i < mesh.material_paths.size(); ++i)
        std::cout << "  material[" << i << "]=" << mesh.material_paths[i] << "\n";
    for (auto& s : mesh.sections)
        std::cout << "  section mat=" << s.material_index << " first=" << s.first_index
                  << " tris=" << s.num_triangles << "\n";
    if (!out_path.empty()) {
        write_obj(mesh, out_path);
        std::cerr << "wrote " << out_path << "\n";
    }
    return 0;
}

// Converts a virtual UE object path ("/Game/..."/"/Engine/...") back to
// the on-disk package path this provider mounts, so a placement's mesh
// reference can be loaded without a reverse path index.
std::string virtual_to_disk_path(const std::string& virtual_path) {
    size_t dot = virtual_path.find_last_of('.');
    std::string pkg_part = dot != std::string::npos ? virtual_path.substr(0, dot) : virtual_path;
    if (pkg_part.rfind("/Game/", 0) == 0) return "OakGame/Content/" + pkg_part.substr(6) + ".uasset";
    // Engine plugin content keeps its own layout ("/Engine/Plugins/X/
    // Content/..." is on disk at exactly that path, no extra Content/).
    if (pkg_part.rfind("/Engine/Plugins/", 0) == 0) return pkg_part.substr(1) + ".uasset";
    if (pkg_part.rfind("/Engine/", 0) == 0) return "Engine/Content/" + pkg_part.substr(8) + ".uasset";
    // Anything else is an object inside a World_P cell itself (its HLOD
    // proxy meshes, their materials and baked textures), so the package
    // is that cell's .umap, already in on-disk form bar the leading '/'.
    if (!pkg_part.empty() && pkg_part.front() == '/') pkg_part.erase(0, 1);
    return pkg_part + ".umap";
}

std::string leaf_object_name(const std::string& virtual_path) {
    size_t dot = virtual_path.find_last_of('.');
    return dot != std::string::npos ? virtual_path.substr(dot + 1) : virtual_path;
}

// Leaf names alone collide across the full map (many packages hold an
// "SM_Door"), so every baked file carries 8 hex digits of its source
// path's FNV-1a hash.
std::string path_tag(const std::string& virtual_path) {
    uint32_t h = 2166136261u;
    for (char c : virtual_path) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return buf;
}

std::string sanitize_filename(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return out;
}

// Unreal (centimetres, Z-up, left-handed) -> engine/glTF space (metres,
// Y-up, right-handed): the same Y/Z swap and cm->m scale walker.cpp's
// append_gltf applies to placements, so a mesh and the transform placing
// it agree. UE's front faces are clockwise; the axis swap is a mirror, so
// it alone turns them counter-clockwise -- the index order stays as is.
void to_engine_space(bl4::MeshData& mesh) {
    for (auto& v : mesh.vertices) {
        float px = v.px, py = v.py, pz = v.pz;
        v.px = px * 0.01f; v.py = pz * 0.01f; v.pz = py * 0.01f;
        float nx = v.nx, ny = v.ny, nz = v.nz;
        v.nx = nx; v.ny = nz; v.nz = ny;
        float tx = v.tx, ty = v.ty, tz = v.tz;
        v.tx = tx; v.ty = tz; v.tz = ty;
        v.tw = -v.tw;
    }
}

// A model's axis-aligned extent in engine space, written beside its OBJ
// as "<name>.bnd" so a resumed bake (which does not re-decode the mesh)
// can still bucket its instances by the space they cover.
struct ModelBounds {
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    bool known = false;
};

ModelBounds mesh_bounds(const bl4::MeshData& mesh) {
    ModelBounds b;
    if (mesh.vertices.empty()) return b;
    b.min[0] = b.max[0] = mesh.vertices[0].px;
    b.min[1] = b.max[1] = mesh.vertices[0].py;
    b.min[2] = b.max[2] = mesh.vertices[0].pz;
    for (const auto& v : mesh.vertices) {
        const float p[3] = {v.px, v.py, v.pz};
        for (int a = 0; a < 3; ++a) {
            b.min[a] = std::min(b.min[a], p[a]);
            b.max[a] = std::max(b.max[a], p[a]);
        }
    }
    b.known = true;
    return b;
}

std::string bounds_path(const std::string& obj_path) {
    return obj_path.substr(0, obj_path.size() - 4) + ".bnd";
}

void write_bounds(const std::string& obj_path, const ModelBounds& b) {
    std::ofstream f(bounds_path(obj_path));
    f << b.min[0] << " " << b.min[1] << " " << b.min[2] << " "
      << b.max[0] << " " << b.max[1] << " " << b.max[2] << "\n";
}

ModelBounds read_bounds(const std::string& obj_path) {
    ModelBounds b;
    std::ifstream f(bounds_path(obj_path));
    if (f >> b.min[0] >> b.min[1] >> b.min[2] >> b.max[0] >> b.max[1] >> b.max[2]) b.known = true;
    return b;
}

struct BakedPlacement {
    std::string archetype, model;
    float px, py, pz, qx, qy, qz, qw, sx, sy, sz;
};

// Bakes a set of World_P cells into an engine-consumable region: one OBJ
// per unique mesh referenced (rendering.hpp/nanite geometry, decoded
// once and cached across cells), and placements grouped into fixed-size
// grid tiles by world position -- not by BL4's own opaque per-cell
// naming, since cells carry no grid coordinate in their filename. This
// mirrors packages/gta5's `assets/tiles/<x>_<z>.json` placement format
// in the SDL3CPlusPlus engine, so its existing tile-streaming machinery
// can load the result directly (see bl4x's README "Engine bridge").
// Walks every cell looking for placements whose mesh path contains any of
// `patterns` (case-insensitive substring match), reporting the cell path
// plus the matching instances' centroid -- for picking a bake region by
// content (e.g. "find me a cell with buildings and a road") rather than
// guessing from opaque cell filenames.
// What is at a world point? Walks every cell, and for each whose
// placements come within `radius` metres of (x, z) (engine space, the
// bake's coordinates) prints the nearest placements and the cell's export
// class histogram -- which is how a hole in the baked ground gets traced
// to what the game actually puts there (water, splines, something the
// walker skips).
int cmd_probe(bl4::Provider& p, const bl4::Usmap& usmap, double x, double z, double radius) {
    for (const auto& path : world_p_cells(p)) {
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) continue;
        bl4::CellPlacements cell;
        try { cell = bl4::walk_cell(*pkg, usmap, path); } catch (const std::exception&) { continue; }
        double best = 1e30;
        std::vector<std::pair<double, std::string>> near;
        for (const auto& e : cell.entries) {
            for (size_t i = 0; i + 10 <= e.xforms.size(); i += 10) {
                const double dx = e.xforms[i] - x, dz = e.xforms[i + 2] - z;
                const double d = std::sqrt(dx * dx + dz * dz);
                best = std::min(best, d);
                if (d <= radius)
                    near.push_back({d, e.component + " " + leaf_object_name(e.mesh) + " y=" +
                                           std::to_string(e.xforms[i + 1])});
            }
        }
        if (best > radius) continue;
        std::sort(near.begin(), near.end());
        std::cout << "== " << path << "  (nearest " << best << " m, unsupported "
                  << cell.unsupported << ")" << "\n";
        for (size_t i = 0; i < near.size() && i < 8; ++i)
            std::cout << "   " << static_cast<int>(near[i].first) << " m  " << near[i].second << "\n";
        std::map<std::string, int> classes;
        for (uint32_t i = 0; i < pkg->export_count(); ++i)
            classes[pkg->resolve_object_name(pkg->export_at(i).class_index)]++;
        std::cout << "   classes:";
        for (auto& [name, n] : classes)
            if (name.find("Water") != std::string::npos || name.find("Landscape") != std::string::npos ||
                name.find("Spline") != std::string::npos || name.find("Foliage") != std::string::npos ||
                name.find("Instanced") != std::string::npos || name.find("Volume") != std::string::npos)
                std::cout << " " << name << "=" << n;
        std::cout << "\n";
    }
    return 0;
}

int cmd_find_cells(bl4::Provider& p, const bl4::Usmap& usmap,
                   const std::vector<std::string>& patterns, int max_hits) {
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::vector<std::string> needles;
    for (auto& p2 : patterns) needles.push_back(lower(p2));

    std::vector<std::string> cells = p.find_paths("world_p/_generated_/");
    std::vector<std::string> umaps;
    for (auto& c : cells)
        if (c.size() > 5 && c.compare(c.size() - 5, 5, ".umap") == 0) umaps.push_back(c);
    std::sort(umaps.begin(), umaps.end());

    int hits = 0;
    for (auto& path : umaps) {
        if (hits >= max_hits) break;
        bl4::Package* pkg = p.load_package(path);
        if (!pkg) continue;
        try {
            bl4::CellPlacements cell = bl4::walk_cell(*pkg, usmap, path);
            std::map<std::string, std::pair<double, int>> matched;  // needle -> (sum stub, count)
            double cx = 0, cy = 0, cz = 0;
            int n = 0;
            for (auto& e : cell.entries) {
                std::string mesh_lower = lower(e.mesh);
                bool any = false;
                for (auto& needle : needles)
                    if (mesh_lower.find(needle) != std::string::npos) { any = true; break; }
                if (!any) continue;
                for (size_t i = 0; i + 10 <= e.xforms.size(); i += 10) {
                    cx += e.xforms[i]; cy += e.xforms[i + 1]; cz += e.xforms[i + 2];
                    ++n;
                }
            }
            if (n > 0) {
                ++hits;
                std::cout << path << " matches=" << n << " centroid=(" << (cx / n) << ","
                          << (cy / n) << "," << (cz / n) << ")\n";
            }
        } catch (const std::exception&) {
        }
    }
    return 0;
}

// Resolves each material slot to a base-colour TGA under
// <out_dir>/textures, once per material and once per texture.
class TextureBaker {
public:
    TextureBaker(bl4::Provider& p, const bl4::Usmap& usmap, std::string out_dir,
                 int max_texture_size)
        : p_(p), usmap_(usmap), dir_(std::move(out_dir) + "/textures"),
          max_size_(max_texture_size) {
        std::filesystem::create_directories(dir_);
    }

    // Path relative to models/ ("../textures/X.tga"), or "" for none.
    std::string for_material(const std::string& material_path) {
        auto it = by_material_.find(material_path);
        if (it != by_material_.end()) return it->second;
        std::string rel;
        ++materials_;
        if (bl4::Package* pkg = p_.load_package(virtual_to_disk_path(material_path))) {
            if (auto tex = bl4::find_base_color_texture(*pkg, usmap_)) rel = for_texture(*tex);
            else report_unresolved(material_path, *pkg);
        }
        if (!rel.empty()) ++materials_textured_;
        return by_material_[material_path] = rel;
    }

    // One flat water colour for every water surface: the game's water
    // materials are shader graphs (depth colour, caustics, foam) with no
    // single base-colour map to stand in for them.
    std::string water() {
        const std::string file = "_water.tga";
        if (!std::filesystem::exists(dir_ + "/" + file)) {
            uint8_t header[18] = {};
            header[2] = 2; header[12] = 8; header[14] = 8; header[16] = 32; header[17] = 0x28;
            std::ofstream out(dir_ + "/" + file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            const uint8_t bgra[4] = {112, 96, 40, 255};  // a murky teal
            for (int i = 0; i < 64; ++i) out.write(reinterpret_cast<const char*>(bgra), 4);
        }
        return "../textures/" + file;
    }

    void print_stats() const {
        std::cout << "materials=" << materials_ << " textured=" << materials_textured_
                  << " textures_written=" << written_ << " textures_failed=" << failed_ << "\n";
    }

private:
    std::string for_texture(const bl4::MaterialTexture& tex) {
        std::string key = tex.pkg->path() + "#" + std::to_string(tex.export_index);
        auto it = by_texture_.find(key);
        if (it != by_texture_.end()) return it->second;
        std::string file = sanitize_filename(tex.name) + "_" + path_tag(key) + ".tga";
        std::string rel;
        if (std::filesystem::exists(dir_ + "/" + file)) {  // resumes an interrupted bake
            ++written_;
            return by_texture_[key] = "../textures/" + file;
        }
        try {
            bl4::TextureData data = bl4::load_texture(*tex.pkg, usmap_, tex.export_index);
            if (bl4::write_texture_tga(data, dir_ + "/" + file, max_size_)) {
                rel = "../textures/" + file;
                ++written_;
            } else {
                ++failed_;
                std::cerr << "  texture " << tex.name << ": can't decode " << data.pixel_format << "\n";
            }
        } catch (const std::exception& e) {
            ++failed_;
            std::cerr << "  texture " << tex.name << " failed: " << e.what() << "\n";
        }
        return by_texture_[key] = rel;
    }

    void report_unresolved(const std::string& material_path, bl4::Package& pkg) {
        std::cerr << "  no base colour in " << leaf_object_name(material_path) << ":";
        for (auto& t : bl4::list_texture_parameters(pkg, usmap_))
            std::cerr << " " << t.parameter << "=" << t.name;
        std::cerr << "\n";
    }

    bl4::Provider& p_;
    const bl4::Usmap& usmap_;
    std::string dir_;
    int max_size_;
    std::unordered_map<std::string, std::string> by_material_, by_texture_;
    long long materials_ = 0, materials_textured_ = 0, written_ = 0, failed_ = 0;
};

// Which placements a bake takes. BL4's World Partition keeps, beside
// the real props, its own merged HLOD proxy meshes inside each cell
// package: low-detail stand-ins for a whole cell, at several levels
// (the coarsest, "HL1_Merged", covers a 511 m block in ~11k triangles).
// Detail and Overview are two different maps of the same world, and
// drawing both at once would just make them fight for depth.
enum class BakeMode { Detail, Overview };

bool wanted_placement(BakeMode mode, const std::string& mesh_path,
                      const std::string& component) {
    const bool cell_local = mesh_path.rfind("/Game/", 0) != 0 &&
                            mesh_path.rfind("/Engine/", 0) != 0;
    // The terrain is a cell-local mesh too -- UE5 landscapes carry a
    // Nanite mesh of themselves, which is the only ground BL4 has and
    // belongs in the detail bake, not the overview.
    const bool terrain = component.find("Landscape") != std::string::npos;
    // World Partition's instanced HLOD: low-detail copies of props that
    // are already placed for real, which only doubled the drawing.
    if (component == "HLODInstancedStaticMeshComponent") return false;
    // Collision-only stand-ins (SM_Road_Invisible_*): the game never
    // draws them, and the visible road sits on the same spot.
    if (mesh_path.find("/Invisible") != std::string::npos) return false;
    // Water surfaces are ground to land on in both bakes: the terrain is
    // cut away wherever water sits.
    const bool water = component.rfind("WaterBody", 0) == 0 || component == "WaterSplineComponent";
    if (mode == BakeMode::Detail) return !cell_local || terrain || water;
    // The overview: the coarsest structure proxies, plus the terrain --
    // the proxies cover buildings and rock, not ground, and alone they
    // float over nothing.
    if (component == "SplineMeshComponent") return false;  // too fine for kilometres up
    return terrain || water ||
           (cell_local && leaf_object_name(mesh_path).find("HL1_Merged") != std::string::npos);
}

// Which tiles an instance belongs in. A placement used to go only in
// the tile holding its pivot, which is fine for a 2 m crate and wrong
// for a 250 m landscape mesh: standing anywhere but its pivot's tile,
// the ground was not resident and the player fell through the world.
// Rotation is handled by transforming all eight corners.
std::vector<std::pair<int64_t, int64_t>> tiles_covered(const ModelBounds& b,
                                                       const float* xform,
                                                       double tile_size) {
    const float tx = xform[0], ty = xform[1], tz = xform[2];
    std::vector<std::pair<int64_t, int64_t>> out;
    auto tile_of = [tile_size](double v) {
        return static_cast<int64_t>(std::floor(v / tile_size));
    };
    if (!b.known) {
        out.push_back({tile_of(tx), tile_of(tz)});
        return out;
    }
    const float qx = xform[3], qy = xform[4], qz = xform[5], qw = xform[6];
    const float sx = xform[7], sy = xform[8], sz = xform[9];
    double lo_x = 0, hi_x = 0, lo_z = 0, hi_z = 0;
    for (int corner = 0; corner < 8; ++corner) {
        const float local[3] = {(corner & 1 ? b.max[0] : b.min[0]) * sx,
                                (corner & 2 ? b.max[1] : b.min[1]) * sy,
                                (corner & 4 ? b.max[2] : b.min[2]) * sz};
        // q * v * q^-1, written out: the same rotation walker.cpp applies.
        const float t[3] = {2 * (qy * local[2] - qz * local[1]),
                            2 * (qz * local[0] - qx * local[2]),
                            2 * (qx * local[1] - qy * local[0])};
        const double wx = local[0] + qw * t[0] + (qy * t[2] - qz * t[1]);
        const double wz = local[2] + qw * t[2] + (qx * t[1] - qy * t[0]);
        if (corner == 0) { lo_x = hi_x = wx; lo_z = hi_z = wz; }
        lo_x = std::min(lo_x, wx); hi_x = std::max(hi_x, wx);
        lo_z = std::min(lo_z, wz); hi_z = std::max(hi_z, wz);
    }
    const int64_t x0 = tile_of(tx + lo_x), x1 = tile_of(tx + hi_x);
    const int64_t z0 = tile_of(tz + lo_z), z1 = tile_of(tz + hi_z);
    // A degenerate transform could ask for millions of tiles; the pivot
    // alone is a safer answer than filling the disk.
    if ((x1 - x0 + 1) * (z1 - z0 + 1) > 4096) {
        out.push_back({tile_of(tx), tile_of(tz)});
        return out;
    }
    for (int64_t x = x0; x <= x1; ++x)
        for (int64_t z = z0; z <= z1; ++z) out.push_back({x, z});
    return out;
}

// Writes the "<name>.bnd" extent beside every models/*.obj that lacks
// one -- for a bake made before bounds existed, so the next run can
// bucket big instances by the tiles they cover without re-decoding
// 22 GB of meshes.
int cmd_bake_bounds(const std::string& out_dir) {
    namespace fs = std::filesystem;
    long long written = 0, skipped = 0;
    for (const auto& entry : fs::directory_iterator(out_dir + "/models")) {
        const std::string path = entry.path().generic_string();
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".obj") != 0) continue;
        if (fs::exists(bounds_path(path))) { ++skipped; continue; }
        ModelBounds b;
        std::ifstream in(path);
        std::string tag;
        float x = 0, y = 0, z = 0;
        while (in >> tag) {
            if (tag != "v") { std::getline(in, tag); continue; }
            if (!(in >> x >> y >> z)) break;
            const float p[3] = {x, y, z};
            if (!b.known) {
                for (int a = 0; a < 3; ++a) { b.min[a] = p[a]; b.max[a] = p[a]; }
                b.known = true;
            } else {
                for (int a = 0; a < 3; ++a) {
                    b.min[a] = std::min(b.min[a], p[a]);
                    b.max[a] = std::max(b.max[a], p[a]);
                }
            }
        }
        if (b.known) { write_bounds(path, b); ++written; }
        if ((written + skipped) % 500 == 0)
            std::cout << "  ... " << written << " written, " << skipped
                      << " already had one\n" << std::flush;
    }
    std::cout << "bounds written=" << written << " skipped=" << skipped << "\n";
    return 0;
}

// Converts every models/*.obj in an existing bake to the binary form,
// so streaming reads meshes instead of parsing them.
int cmd_bake_binary(const std::string& out_dir) {
    namespace fs = std::filesystem;
    long long written = 0, skipped = 0, failed = 0;
    for (const auto& entry : fs::directory_iterator(out_dir + "/models")) {
        const std::string path = entry.path().generic_string();
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".obj") != 0) continue;
        const std::string out = path.substr(0, path.size() - 4) + ".bmesh";
        if (fs::exists(out)) { ++skipped; continue; }
        if (bl4::convert_obj_to_binary(path, out)) ++written; else ++failed;
        if ((written + skipped + failed) % 500 == 0)
            std::cout << "  ... " << written << " converted\n" << std::flush;
    }
    std::cout << "binary meshes written=" << written << " skipped=" << skipped
              << " failed=" << failed << "\n";
    return 0;
}

int cmd_bake_region(bl4::Provider& p, const bl4::Usmap& usmap,
                    const std::vector<std::string>& cell_paths,
                    const std::string& out_dir, double tile_size,
                    BakeMode mode = BakeMode::Detail, int max_texture_size = 1024) {
    namespace fs = std::filesystem;
    fs::create_directories(out_dir + "/models");
    fs::create_directories(out_dir + "/tiles");

    std::map<std::pair<int64_t, int64_t>, std::vector<BakedPlacement>> tiles;
    std::unordered_map<std::string, std::string> mesh_to_model;  // "" cached = export failed, don't retry
    std::unordered_map<std::string, ModelBounds> bounds;          // by model_rel
    long long total_instances = 0, meshes_exported = 0, meshes_failed = 0;
    long long hlod_skipped = 0;
    double spawn_x = 0, spawn_y = 0, spawn_z = 0;
    bool have_spawn = false;
    TextureBaker textures(p, usmap, out_dir, max_texture_size);

    // A model's identity: a water body's plane is tinted as water, and
    // every spline mesh is bent differently, so neither may share the
    // plain mesh's cache entry.
    auto model_key = [](const bl4::Placement& pl) {
        if (pl.kind == "water") return pl.mesh + "#water";
        if (pl.kind == "spline_mesh") {
            std::string key = pl.mesh + "#spline";
            for (float f : pl.spline) key += "," + std::to_string(f);
            return key;
        }
        return pl.mesh;
    };
    // A base mesh for bending, in UE space, loaded once however many
    // spline segments reuse it (a road is hundreds of the same segment).
    std::unordered_map<std::string, std::shared_ptr<bl4::MeshData>> base_meshes;
    auto load_base = [&](const std::string& mesh_path) -> std::shared_ptr<bl4::MeshData> {
        auto it = base_meshes.find(mesh_path);
        if (it != base_meshes.end()) return it->second;
        std::shared_ptr<bl4::MeshData> out;
        const std::string leaf = leaf_object_name(mesh_path);
        if (bl4::Package* pkg = p.load_package(virtual_to_disk_path(mesh_path))) {
            for (uint32_t i = 0; i < pkg->export_count() && !out; ++i) {
                const auto& exp = pkg->export_at(i);
                if (pkg->resolve_object_name(exp.class_index) != "StaticMesh" ||
                    pkg->resolve(exp.object_name) != leaf) continue;
                try {
                    auto mesh = std::make_shared<bl4::MeshData>(bl4::load_static_mesh(*pkg, usmap, i));
                    if (!mesh->vertices.empty() && !mesh->indices.empty()) out = mesh;
                } catch (const std::exception& e) {
                    std::cerr << "  mesh failed " << mesh_path << ": " << e.what() << "\n";
                }
            }
        }
        if (base_meshes.size() > 512) base_meshes.clear();  // bounded: bases are small but many
        return base_meshes[mesh_path] = out;
    };
    // Spline meshes (bent per instance) and lakes (triangulated from their
    // outline): geometry UE builds at runtime. Binary form only -- the
    // engine loads .bmesh first, and there are tens of thousands.
    auto bake_generated = [&](const bl4::Placement& pl) -> std::string {
        const bool lake = pl.kind == "lake";
        const std::string stem =
            lake ? "Lake_" + path_tag(pl.mesh)
                 : sanitize_filename(leaf_object_name(pl.mesh)) + "_s" + path_tag(model_key(pl));
        const std::string rel = "models/" + stem + ".obj";
        const std::string bmesh = out_dir + "/models/" + stem + ".bmesh";
        if (std::filesystem::exists(bmesh)) {
            bounds[rel] = read_bounds(out_dir + "/" + rel);
            ++meshes_exported;
            return rel;
        }
        bl4::MeshData mesh;
        std::vector<std::string> texture_files;
        if (lake) {
            mesh = bl4::triangulate_lake(pl.outline);
            if (mesh.indices.empty()) return "";
            texture_files.push_back(textures.water());
        } else {
            auto base = load_base(pl.mesh);
            if (!base) return "";
            mesh = *base;
            bl4::deform_spline_mesh(mesh, pl.spline);
            for (auto& material : mesh.material_paths)
                texture_files.push_back(textures.for_material(material));
        }
        to_engine_space(mesh);
        if (lake) {
            // Face up: the outline's winding decides which way the
            // triangulation came out, and a downward lake is invisible.
            const auto& a = mesh.vertices[mesh.indices[0]];
            const auto& b = mesh.vertices[mesh.indices[1]];
            const auto& c = mesh.vertices[mesh.indices[2]];
            const float ny = (b.pz - a.pz) * (c.px - a.px) - (b.px - a.px) * (c.pz - a.pz);
            if (ny < 0)
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
                    std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
            for (auto& v : mesh.vertices) { v.nx = 0; v.ny = 1; v.nz = 0; }
        }
        bl4::write_binary_mesh(mesh, bmesh, texture_files);
        bounds[rel] = mesh_bounds(mesh);
        write_bounds(out_dir + "/" + rel, bounds[rel]);
        ++meshes_exported;
        return rel;
    };

    long long cells_done = 0;
    for (const auto& cell_path : cell_paths) {
        if (++cells_done % 200 == 0) {
            std::cout << "  ... " << cells_done << "/" << cell_paths.size() << " cells, "
                      << total_instances << " instances, " << meshes_exported << " meshes\n"
                      << std::flush;
        }
        bl4::Package* cell_pkg = p.load_package(cell_path);
        if (!cell_pkg) { std::cerr << "cell not found: " << cell_path << "\n"; continue; }
        bl4::CellPlacements cell = bl4::walk_cell(*cell_pkg, usmap, cell_path);

        for (const auto& placement : cell.entries) {
            const bool known = placement.kind == "mesh" || placement.kind == "water" ||
                               placement.kind == "spline_mesh" || placement.kind == "lake";
            if (!known || placement.mesh.empty()) continue;
            if (!wanted_placement(mode, placement.mesh, placement.component)) {
                ++hlod_skipped;
                continue;
            }

            auto cached = mesh_to_model.find(model_key(placement));
            std::string model_rel;
            if (cached != mesh_to_model.end()) {
                model_rel = cached->second;
            } else if (placement.kind == "spline_mesh" || placement.kind == "lake") {
                model_rel = bake_generated(placement);
                if (model_rel.empty()) ++meshes_failed;
                mesh_to_model[model_key(placement)] = model_rel;
            } else {
                std::string leaf = leaf_object_name(placement.mesh);
                bl4::Package* mesh_pkg = p.load_package(virtual_to_disk_path(placement.mesh));
                uint32_t export_idx = UINT32_MAX;
                if (mesh_pkg) {
                    for (uint32_t i = 0; i < mesh_pkg->export_count(); ++i) {
                        const auto& exp = mesh_pkg->export_at(i);
                        if (mesh_pkg->resolve_object_name(exp.class_index) == "StaticMesh" &&
                            mesh_pkg->resolve(exp.object_name) == leaf) {
                            export_idx = i;
                            break;
                        }
                    }
                }
                const bool water = placement.kind == "water";
                std::string filename = sanitize_filename(leaf) + "_" +
                                       path_tag(water ? placement.mesh + "#water" : placement.mesh) +
                                       ".obj";
                if (std::filesystem::exists(out_dir + "/models/" + filename)) {
                    model_rel = "models/" + filename;  // resumes an interrupted bake
                    bounds[model_rel] = read_bounds(out_dir + "/" + model_rel);
                    ++meshes_exported;
                } else if (mesh_pkg && export_idx != UINT32_MAX) {
                    try {
                        bl4::MeshData mesh = bl4::load_static_mesh(*mesh_pkg, usmap, export_idx);
                        if (!mesh.vertices.empty() && !mesh.indices.empty()) {
                            to_engine_space(mesh);
                            // Overview terrain: 84 M triangles map-wide at full
                            // detail, seen from kilometres up. ~10 m cells in
                            // world space, via the placement's own scale.
                            if (mode == BakeMode::Overview &&
                                placement.component.find("Landscape") != std::string::npos &&
                                placement.xforms.size() >= 10) {
                                const float scale = std::max({std::abs(placement.xforms[7]),
                                                              std::abs(placement.xforms[8]),
                                                              std::abs(placement.xforms[9]), 1e-3f});
                                bl4::cluster_simplify(mesh, 10.f / scale);
                            }
                            std::vector<std::string> texture_files;
                            for (auto& material : mesh.material_paths)
                                texture_files.push_back(water ? textures.water()
                                                              : textures.for_material(material));
                            write_obj(mesh, out_dir + "/models/" + filename, &texture_files);
                            bl4::write_binary_mesh(
                                mesh, out_dir + "/models/" + filename.substr(0, filename.size() - 4) +
                                          ".bmesh",
                                texture_files);
                            model_rel = "models/" + filename;
                            bounds[model_rel] = mesh_bounds(mesh);
                            write_bounds(out_dir + "/" + model_rel, bounds[model_rel]);
                            ++meshes_exported;
                        } else {
                            std::cerr << "  no geometry " << placement.mesh << "\n";
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "  mesh failed " << placement.mesh << ": " << e.what() << "\n";
                    }
                } else {
                    std::cerr << (mesh_pkg ? "  no StaticMesh export " : "  no package ")
                              << placement.mesh << "\n";
                }
                if (model_rel.empty()) ++meshes_failed;
                mesh_to_model[model_key(placement)] = model_rel;
            }
            if (model_rel.empty()) continue;

            std::string archetype = sanitize_filename(leaf_object_name(placement.mesh));
            for (size_t i = 0; i + 10 <= placement.xforms.size(); i += 10) {
                float tx = placement.xforms[i], ty = placement.xforms[i + 1], tz = placement.xforms[i + 2];
                if (!have_spawn) { spawn_x = tx; spawn_y = ty; spawn_z = tz; have_spawn = true; }
                const BakedPlacement baked{archetype, model_rel, tx, ty, tz,
                                           placement.xforms[i + 3], placement.xforms[i + 4],
                                           placement.xforms[i + 5], placement.xforms[i + 6],
                                           placement.xforms[i + 7], placement.xforms[i + 8],
                                           placement.xforms[i + 9]};
                for (const auto& key :
                     tiles_covered(bounds[model_rel], &placement.xforms[i], tile_size)) {
                    tiles[key].push_back(baked);
                }
                ++total_instances;
            }
        }
    }

    for (auto& [key, list] : tiles) {
        std::string dir = out_dir + "/tiles/" + std::to_string(key.first) + "_" + std::to_string(key.second);
        fs::create_directories(dir);
        std::ofstream f(dir + "/placements.json", std::ios::binary);
        f << "{\"tile\":[" << key.first << "," << key.second << "],\"tile_size\":" << tile_size
          << ",\"placements\":[";
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& tp = list[i];
            if (i) f << ",";
            f << "{\"archetype\":\"" << tp.archetype << "\",\"model\":\"" << tp.model << "\""
              << ",\"position\":[" << tp.px << "," << tp.py << "," << tp.pz << "]"
              << ",\"rotation\":[" << tp.qx << "," << tp.qy << "," << tp.qz << "," << tp.qw << "]"
              << ",\"scale\":[" << tp.sx << "," << tp.sy << "," << tp.sz << "]}";
        }
        f << "]}";
    }

    {
        std::ofstream f(out_dir + "/world.json", std::ios::binary);
        f << "{\"tile_size\":" << tile_size << ",\"spawn\":{\"x\":" << spawn_x
          << ",\"y\":" << (spawn_y + 3.0) << ",\"z\":" << spawn_z << ",\"heading\":0.0}}";
    }

    std::cout << "tiles=" << tiles.size() << " instances=" << total_instances
              << " meshes_exported=" << meshes_exported << " meshes_failed=" << meshes_failed
              << " hlod_placements_skipped=" << hlod_skipped << "\n";
    textures.print_stats();
    return 0;
}

int cmd_dump_bytes(bl4::Provider& p, const std::string& path, int index,
                   const std::string& out_path) {
    bl4::Package* pkg = p.load_package(path);
    if (!pkg) return 1;
    const auto& e = pkg->export_at(static_cast<size_t>(index));
    size_t start = pkg->all_export_data_offset() + e.cooked_serial_offset;
    size_t size = e.cooked_serial_size;
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pkg->data().data() + start),
             static_cast<std::streamsize>(size));
    std::cerr << "wrote " << size << " bytes to " << out_path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    // Needs no game files: run before mounting the paks.
    if (!args.empty() && args[0] == "coverage-raster")
        return bl4::run_coverage_raster(std::vector<std::string>(args.begin() + 1, args.end()));
    try {
        bl4::oodle::init("D:/BL4Export/oodle.dll");
        bl4::Provider provider;
        provider.mount(kPaksDir);

        if (args.empty() || args[0] == "index") return cmd_index(provider);
        if (args[0] == "dump" && args.size() > 1) return cmd_dump(provider, args[1]);
        if (args[0] == "census")
            return cmd_census(provider, args.size() > 1 ? std::stoi(args[1]) : 0);
        if (args[0] == "props" && args.size() > 1) {
            bl4::Usmap usmap(kUsmapPath);
            int only = args.size() > 2 ? std::stoi(args[2]) : -1;
            return cmd_props(provider, usmap, args[1], only);
        }
        if (args[0] == "obj" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_obj(provider, usmap, args[1], std::stoi(args[2]));
        }
        if (args[0] == "material" && args.size() > 1) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_material(provider, usmap, args[1]);
        }
        if (args[0] == "raw-props" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_raw_props(provider, usmap, args[1], std::stoi(args[2]));
        }
        if (args[0] == "mesh" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            std::string out = args.size() > 3 ? args[3] : "";
            return cmd_mesh(provider, usmap, args[1], std::stoi(args[2]), out);
        }
        if (args[0] == "mesh-batch" && args.size() > 1) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_mesh_batch(provider, usmap, args[1]);
        }
        if (args[0] == "find-cells" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            int max_hits = std::stoi(args[1]);
            std::vector<std::string> patterns(args.begin() + 2, args.end());
            return cmd_find_cells(provider, usmap, patterns, max_hits);
        }
        if (args[0] == "bake" && args.size() > 3) {
            bl4::Usmap usmap(kUsmapPath);
            std::vector<std::string> cells(args.begin() + 3, args.end());
            return cmd_bake_region(provider, usmap, cells, args[1], std::stod(args[2]));
        }
        if (args[0] == "bake-binary" && args.size() > 1) return cmd_bake_binary(args[1]);
        if (args[0] == "bake-bounds" && args.size() > 1) return cmd_bake_bounds(args[1]);
        if (args[0] == "bake-overview" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            // 256 px maps: an overview holds the whole map resident, and
            // one baked colour map per 511 m block is never seen close up.
            return cmd_bake_region(provider, usmap, world_p_cells(provider), args[1],
                                   std::stod(args[2]), BakeMode::Overview, 256);
        }
        if (args[0] == "bake-all" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_bake_region(provider, usmap, world_p_cells(provider), args[1],
                                   std::stod(args[2]));
        }

        if (args[0] == "walk" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_walk(provider, usmap, args[1], args[2]);
        }
        if (args[0] == "landscape-census") {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_landscape_census(provider, usmap);
        }
        if (args[0] == "probe" && args.size() > 3) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_probe(provider, usmap, std::stod(args[1]), std::stod(args[2]),
                             std::stod(args[3]));
        }
        if (args[0] == "find-class" && args.size() > 1) {
            return cmd_find_class(provider, args[1], args.size() > 2 ? std::stoi(args[2]) : 5);
        }
        if (args[0] == "find-export" && args.size() > 2)
            return cmd_find_export(provider, args[1], args[2]);
        if (args[0] == "texture-batch" && args.size() > 1) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_texture_batch(provider, usmap, args[1]);
        }
        if (args[0] == "texture-census") {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_texture_census(provider, usmap, args.size() > 1 ? std::stoi(args[1]) : 0);
        }
        if (args[0] == "texture" && args.size() > 3) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_texture(provider, usmap, args[1], std::stoi(args[2]), args[3]);
        }
        if (args[0] == "walk-census") {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_walk_census(provider, usmap, args.size() > 1 ? std::stoi(args[1]) : 0);
        }
        if (args[0] == "census-props") {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_census_props(provider, usmap, args.size() > 1 ? std::stoi(args[1]) : 0);
        }
        if (args[0] == "dumpbytes" && args.size() > 3)
            return cmd_dump_bytes(provider, args[1], std::stoi(args[2]), args[3]);

        std::cerr << "usage: bl4x index | dump <path> | census [limit] | props <path>\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
