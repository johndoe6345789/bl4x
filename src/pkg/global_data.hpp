#pragma once
// The one global.utoc container: native (script) class/function names,
// resolved without touching any package (CUE4Parse's IoGlobalData).
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg/name_map.hpp"
#include "pkg/obj_index.hpp"

namespace bl4 {

class IoStore;

struct ScriptObjectEntry {
    MappedName object_name;
    ObjIndex global_index;
    ObjIndex outer_index;
    ObjIndex cdo_class_index;
};

class GlobalData {
public:
    explicit GlobalData(IoStore& global_container);

    std::string resolve(const MappedName& n) const {
        return resolve_mapped_name(n, names_);
    }
    // Full outer-qualified path, e.g. "/Script/Engine.StaticMeshComponent".
    std::string full_path(ObjIndex idx) const;
    const ScriptObjectEntry* find(ObjIndex idx) const;

private:
    std::vector<std::string> names_;
    std::unordered_map<uint64_t, ScriptObjectEntry> objects_;
};

}  // namespace bl4
