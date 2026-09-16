#include "pkg/provider.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

#include "io/iostore.hpp"
#include "pkg/package.hpp"

namespace bl4 {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Provider::Provider() = default;
Provider::~Provider() = default;

void Provider::mount(const std::string& paks_dir) {
    namespace fs = std::filesystem;
    std::vector<fs::path> utocs;
    for (auto& e : fs::directory_iterator(paks_dir))
        if (e.path().extension() == ".utoc") utocs.push_back(e.path());
    std::sort(utocs.begin(), utocs.end());

    for (auto& p : utocs) {
        auto store = std::make_unique<IoStore>(p.string());
        IoStore* raw = store.get();
        containers_.push_back(std::move(store));

        bool is_global = lower(p.filename().string()) == "global.utoc";
        for (auto& f : raw->files()) {
            std::string key = lower(f.path);
            auto it = files_.find(key);
            if (it == files_.end() || raw->order() >= it->second.container->order())
                files_[key] = {raw, f.chunk};

            const ChunkId& cid = raw->chunk_id(f.chunk);
            if (cid.type == static_cast<uint8_t>(ChunkType::ExportBundleData) &&
                cid.index() == 0) {
                by_package_id_[cid.id] = {raw, f.chunk};
                package_id_path_[cid.id] = f.path;
            }
        }
        if (is_global) global_ = std::make_unique<GlobalData>(*raw);
        raw->release_files();
    }
    if (!global_) throw std::runtime_error("no global.utoc found in " + paks_dir);
}

const ContainerHeader& Provider::header_of(IoStore& container) {
    auto it = headers_.find(&container);
    if (it != headers_.end()) return *it->second;
    ChunkId id = ChunkId::make(container.container_id(), 0, ChunkType::ContainerHeader);
    int64_t chunk = container.find(id);
    auto header = chunk >= 0
        ? std::make_unique<ContainerHeader>(container.read(static_cast<uint32_t>(chunk)))
        : std::make_unique<ContainerHeader>(std::span<const uint8_t>{});
    ContainerHeader* raw = header.get();
    headers_[&container] = std::move(header);
    return *raw;
}

Package* Provider::load_package(const std::string& path) {
    auto it = files_.find(lower(path));
    if (it == files_.end()) return nullptr;
    const ChunkId& cid = it->second.container->chunk_id(it->second.chunk);

    std::lock_guard<std::recursive_mutex> lock(package_cache_mutex_);
    auto cached = package_cache_.find(cid.id);
    if (cached != package_cache_.end()) return cached->second.get();

    auto data = it->second.container->read(it->second.chunk);
    auto pkg = std::make_unique<Package>(std::move(data), *this, *it->second.container,
                                         cid.id, path);
    Package* raw = pkg.get();
    package_cache_[cid.id] = std::move(pkg);
    return raw;
}

Package* Provider::load_package_by_id(uint64_t package_id) {
    std::lock_guard<std::recursive_mutex> lock(package_cache_mutex_);
    auto cached = package_cache_.find(package_id);
    if (cached != package_cache_.end()) return cached->second.get();

    auto it = by_package_id_.find(package_id);
    if (it == by_package_id_.end()) return nullptr;
    auto data = it->second.container->read(it->second.chunk);
    auto pit = package_id_path_.find(package_id);
    std::string path = pit != package_id_path_.end() ? pit->second : "<by-id>";
    auto pkg = std::make_unique<Package>(std::move(data), *this, *it->second.container,
                                         package_id, path);
    Package* raw = pkg.get();
    package_cache_[package_id] = std::move(pkg);
    return raw;
}

std::vector<std::string> Provider::find_paths(const std::string& needle_lower) const {
    std::vector<std::string> out;
    for (auto& [k, v] : files_)
        if (k.find(needle_lower) != std::string::npos) out.push_back(k);
    return out;
}

}  // namespace bl4
