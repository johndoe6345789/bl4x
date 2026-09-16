#pragma once
// A Zen (IoStore) package: header, name map, import/export maps and
// bulk data map. Mirrors CUE4Parse's IoPackage, structural fields only
// -- no property values are parsed here (see pkg/object.hpp for that).
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pkg/container_header.hpp"
#include "pkg/name_map.hpp"
#include "pkg/obj_index.hpp"

namespace bl4 {

class IoStore;
class Provider;
struct BulkDataEntry {
    uint64_t serial_offset;
    uint64_t duplicate_serial_offset;
    uint64_t serial_size;
    uint32_t flags;
    uint8_t cooked_index;
};

struct ExportEntry {
    uint64_t cooked_serial_offset;
    uint64_t cooked_serial_size;
    MappedName object_name;
    ObjIndex outer_index, class_index, super_index, template_index;
    uint64_t public_export_hash;
    uint32_t object_flags;
    uint8_t filter_flags;
};

class Package {
public:
    Package(std::vector<uint8_t> uasset_data, Provider& provider,
            IoStore& container, uint64_t own_chunk_id, std::string path);
    ~Package();
    Package(const Package&) = delete;

    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }
    std::string resolve(const MappedName& n) const;

    size_t export_count() const { return exports_.size(); }
    const ExportEntry& export_at(size_t i) const { return exports_.at(i); }
    const std::vector<BulkDataEntry>& bulk_data_map() const { return bulk_data_map_; }
    uint32_t all_export_data_offset() const { return all_export_data_offset_; }
    uint32_t cooked_header_size() const { return cooked_header_size_; }
    bool unversioned() const { return unversioned_; }
    bool is_cooked() const { return (package_flags_ & 0x00000200u) != 0; }
    bool is_filter_editor_only() const { return (package_flags_ & 0x80000000u) != 0; }
    const std::vector<uint8_t>& data() const { return data_; }
    Provider& provider() { return provider_; }
    IoStore& container() { return container_; }

    // Human path for any index without deserializing the target's own
    // properties: script imports resolve instantly via global data;
    // package imports match by public export hash; local exports use
    // this package's own name map.
    std::string resolve_object_name(ObjIndex idx);
    // This package's UE object path ("/Game/World/.../SM_X", no leaf
    // object name), derived from its on-disk path.
    std::string virtual_path() const;
    // Full "/Game/.../Foo.Foo"-style path for an export reference,
    // following package imports -- what asset lookups need (unlike
    // resolve_object_name, which is just the leaf name).
    std::string resolve_asset_path(ObjIndex idx);
    // Resolves to the (package, export index) an index ultimately
    // points at, following package imports; nullopt for script
    // imports, null indices, or an import that can't be matched.
    std::optional<std::pair<Package*, uint32_t>> resolve_export(ObjIndex idx);

    // Legacy in-property FPackageIndex: 0=null, >0=export (value-1),
    // <0=import_map_[-value-1] (itself an ObjIndex to resolve further).
    ObjIndex resolve_legacy_index(int32_t v) const;
    // FName as read inline in a property stream: a plain index into
    // this package's own name map (no package/global tag), plus the
    // "_N" suffix number CUE4Parse calls the "extra" index.
    std::string resolve_local_name(int32_t name_index, int32_t extra) const;

    // TODO(next phase): deserialize export i's own properties on
    // demand (unversioned property reader + usmap-driven struct
    // dispatch). Every export's byte range is already known from the
    // export map, so one unparseable export need not corrupt others.

private:
    Package* imported_package(uint32_t i);

    Provider& provider_;
    IoStore& container_;
    uint64_t chunk_id_;
    std::string path_;
    std::string name_;
    std::vector<uint8_t> data_;
    std::vector<std::string> name_map_;
    std::vector<ObjIndex> import_map_;
    std::vector<ExportEntry> exports_;
    std::vector<uint64_t> imported_public_export_hashes_;
    std::vector<BulkDataEntry> bulk_data_map_;
    std::vector<uint64_t> imported_package_ids_;
    std::vector<Package*> imported_packages_;  // lazily populated
    std::vector<bool> imported_loaded_;
    uint32_t all_export_data_offset_ = 0;
    uint32_t cooked_header_size_ = 0;
    uint32_t package_flags_ = 0;
    bool unversioned_ = true;
};

}  // namespace bl4
