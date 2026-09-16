// bl4x: a from-scratch, memory-lean reader for Borderlands 4's IoStore
// archives, built to replace the CUE4Parse-based C# exporter for the
#include <cmath>
#include <cstdlib>
// structural (container/package/import/export) layer. See README for
// what's implemented so far and what the next phase needs.
#include <algorithm>
#include <fstream>
#include <chrono>
#include <iostream>
#include <map>
#include <string>

#include "io/oodle.hpp"
#include "pkg/object.hpp"
#include "pkg/package.hpp"
#include "pkg/provider.hpp"
#include "pkg/usmap.hpp"
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

        if (args[0] == "walk" && args.size() > 2) {
            bl4::Usmap usmap(kUsmapPath);
            return cmd_walk(provider, usmap, args[1], args[2]);
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
