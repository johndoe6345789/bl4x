#pragma once
// The per-container FIoContainerHeader chunk: which package a chunk id
// belongs to and which other packages it imports. We only keep what
// cross-package import resolution needs (CUE4Parse's FIoContainerHeader).
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace bl4 {

struct StoreEntry {
    std::vector<uint64_t> imported_packages;
};

class ContainerHeader {
public:
    explicit ContainerHeader(std::span<const uint8_t> data);

    const StoreEntry* find(uint64_t package_id) const {
        auto it = by_id_.find(package_id);
        return it == by_id_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint64_t, StoreEntry> by_id_;
};

}  // namespace bl4
