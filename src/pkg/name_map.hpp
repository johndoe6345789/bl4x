#pragma once
// FMappedName + the UE5 name-batch format used both for a Zen package's
// own name map and the global name map in global.utoc's script objects
// chunk (FNameEntrySerialized::LoadNameBatch(Ar), no explicit count).
#include <cstdint>
#include <string>
#include <vector>

#include "core/reader.hpp"

namespace bl4 {

struct MappedName {
    uint32_t index = 0;
    uint32_t extra = 0;
    uint8_t type = 0;  // 0=Package, 1=Container, 2=Global

    bool is_global() const { return type != 0; }
};

inline MappedName read_mapped_name(Reader& r) {
    uint32_t a = r.read<uint32_t>();
    uint32_t extra = r.read<uint32_t>();
    return {a & 0x3FFFFFFFu, extra, static_cast<uint8_t>(a >> 30)};
}

std::vector<std::string> load_name_batch(Reader& r);

// name_map[mapped.index] with the "_N" suffix UE appends when extra != 0.
std::string resolve_mapped_name(const MappedName& m,
                                const std::vector<std::string>& name_map);

}  // namespace bl4
