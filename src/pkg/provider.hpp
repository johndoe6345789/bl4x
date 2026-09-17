#pragma once
// Owns every mounted IoStore container, the global name/script table,
// and every loaded Package (kept alive for the run: cross-package
// imports hold raw pointers into this cache).
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg/container_header.hpp"
#include "pkg/global_data.hpp"

namespace bl4 {

class IoStore;
class Package;

class Provider {
public:
    Provider();
    ~Provider();

    // Scans dir for *.utoc, opens each, and indexes global.utoc's
    // script objects table.
    void mount(const std::string& paks_dir);

    GlobalData& global_data() { return *global_; }
    const ContainerHeader& header_of(IoStore& container);

    // path is mount-relative, e.g. "OakGame/Content/Maps/Foo.umap".
    Package* load_package(const std::string& path);
    Package* load_package_by_id(uint64_t package_id);

    size_t container_count() const { return containers_.size(); }
    size_t file_count() const { return files_.size(); }
    // Every indexed path whose lowercase form contains needle.
    std::vector<std::string> find_paths(const std::string& needle_lower) const;

    // Raw chunk bytes for any indexed path (case-insensitive) -- used
    // for a package's sibling .ubulk/.uptnl bulk-data files, which
    // aren't packages themselves and need no structural parsing.
    bool read_raw_file(const std::string& path, std::vector<uint8_t>& out) const;

private:
    struct Loc { IoStore* container; uint32_t chunk; };

    std::vector<std::unique_ptr<IoStore>> containers_;
    std::unordered_map<IoStore*, std::unique_ptr<ContainerHeader>> headers_;
    std::unordered_map<std::string, Loc> files_;       // lowercased path
    std::unordered_map<uint64_t, Loc> by_package_id_;
    std::unordered_map<uint64_t, std::string> package_id_path_;  // original-case path
    std::unique_ptr<GlobalData> global_;

    std::recursive_mutex package_cache_mutex_;
    std::unordered_map<uint64_t, std::unique_ptr<Package>> package_cache_;  // keyed by chunk id
};

}  // namespace bl4
