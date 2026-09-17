#include "pkg/package.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "core/reader.hpp"
#include "io/iostore.hpp"
#include "pkg/provider.hpp"

namespace bl4 {
namespace {
constexpr size_t kExportMapEntrySize = 72;
constexpr size_t kBulkDataMapEntrySize = 32;
}  // namespace

Package::Package(std::vector<uint8_t> uasset_data, Provider& provider,
                 IoStore& container, uint64_t own_chunk_id, std::string path)
    : provider_(provider),
      container_(container),
      chunk_id_(own_chunk_id),
      path_(std::move(path)),
      data_(std::move(uasset_data)) {
    Reader r(data_);

    uint32_t has_versioning = r.read<uint32_t>();
    uint32_t header_size = r.read<uint32_t>();
    MappedName pkg_name = read_mapped_name(r);
    size_t flags_pos = r.pos();
    r.skip(4);
    uint32_t cooked_header_size = r.read<uint32_t>();
    int32_t hashes_off = r.read<int32_t>();
    int32_t import_off = r.read<int32_t>();
    int32_t export_off = r.read<int32_t>();
    int32_t bundle_off = r.read<int32_t>();
    r.skip(4);  // dependency bundle headers offset
    r.skip(4);  // dependency bundle entries offset
    r.skip(4);  // imported package names offset

    unversioned_ = (has_versioning == 0);
    if (has_versioning != 0) {
        r.skip(4);       // zen version
        r.skip(8);       // package file version (2x int32)
        r.skip(4);       // licensee version
        int32_t n = r.read_count();
        r.skip(static_cast<size_t>(n) * 20);  // FCustomVersion[]: guid+int32
    }

    name_map_ = load_name_batch(r);
    name_ = resolve(pkg_name);

    if (r.remaining() >= 8) {
        uint64_t pad = r.read<uint64_t>();
        r.skip(static_cast<size_t>(pad));
        int64_t map_bytes = r.read<int64_t>();
        size_t count = static_cast<size_t>(map_bytes) / kBulkDataMapEntrySize;
        bulk_data_map_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            BulkDataEntry e;
            e.serial_offset = r.read<uint64_t>();
            e.duplicate_serial_offset = r.read<uint64_t>();
            e.serial_size = r.read<uint64_t>();
            e.flags = r.read<uint32_t>();
            e.cooked_index = r.read<uint8_t>();
            r.skip(3);  // pad
            bulk_data_map_.push_back(e);
        }
    }

    r.seek(static_cast<size_t>(hashes_off));
    imported_public_export_hashes_ =
        r.read_array<uint64_t>((static_cast<size_t>(import_off) -
                                static_cast<size_t>(hashes_off)) / 8);

    r.seek(static_cast<size_t>(import_off));
    size_t import_count = (static_cast<size_t>(export_off) -
                           static_cast<size_t>(import_off)) / 8;
    import_map_.resize(import_count);
    for (auto& idx : import_map_) idx.raw = r.read<uint64_t>();

    r.seek(static_cast<size_t>(export_off));
    size_t export_count = (static_cast<size_t>(bundle_off) -
                           static_cast<size_t>(export_off)) / kExportMapEntrySize;
    exports_.reserve(export_count);
    for (size_t i = 0; i < export_count; ++i) {
        size_t start = r.pos();
        ExportEntry e;
        e.cooked_serial_offset = r.read<uint64_t>();
        e.cooked_serial_size = r.read<uint64_t>();
        e.object_name = read_mapped_name(r);
        e.outer_index.raw = r.read<uint64_t>();
        e.class_index.raw = r.read<uint64_t>();
        e.super_index.raw = r.read<uint64_t>();
        e.template_index.raw = r.read<uint64_t>();
        e.public_export_hash = r.read<uint64_t>();
        e.object_flags = r.read<uint32_t>();
        e.filter_flags = r.read<uint8_t>();
        exports_.push_back(e);
        r.seek(start + kExportMapEntrySize);
    }

    all_export_data_offset_ = header_size;
    cooked_header_size_ = cooked_header_size;
    {
        Reader fr(data_);
        fr.seek(flags_pos);
        package_flags_ = fr.read<uint32_t>();
    }

    if (const StoreEntry* se = provider_.header_of(container_).find(own_chunk_id))
        imported_package_ids_ = se->imported_packages;
    imported_packages_.assign(imported_package_ids_.size(), nullptr);
    imported_loaded_.assign(imported_package_ids_.size(), false);
}

Package::~Package() = default;

std::string Package::resolve(const MappedName& n) const {
    if (n.is_global()) return provider_.global_data().resolve(n);
    return resolve_mapped_name(n, name_map_);
}

Package* Package::imported_package(uint32_t i) {
    if (i >= imported_package_ids_.size()) return nullptr;
    if (!imported_loaded_[i]) {
        imported_loaded_[i] = true;
        imported_packages_[i] = provider_.load_package_by_id(imported_package_ids_[i]);
    }
    return imported_packages_[i];
}

std::optional<std::pair<Package*, uint32_t>> Package::resolve_export(ObjIndex idx) {
    if (idx.is_null()) return std::nullopt;
    if (idx.is_export()) {
        uint32_t i = static_cast<uint32_t>(idx.value());
        if (i >= exports_.size()) return std::nullopt;
        return std::make_pair(this, i);
    }
    if (!idx.is_package_import()) return std::nullopt;  // script import: no export
    if (imported_public_export_hashes_.empty()) return std::nullopt;

    PackageImportRef ref = idx.package_import_ref();
    if (ref.imported_public_export_hash_index >= imported_public_export_hashes_.size())
        return std::nullopt;
    uint64_t target = imported_public_export_hashes_[ref.imported_public_export_hash_index];

    Package* pkg = imported_package(ref.imported_package_index);
    if (!pkg) return std::nullopt;
    for (uint32_t i = 0; i < pkg->exports_.size(); ++i)
        if (pkg->exports_[i].public_export_hash == target) return std::make_pair(pkg, i);
    return std::nullopt;
}

ObjIndex Package::resolve_legacy_index(int32_t v) const {
    if (v == 0) return ObjIndex{};
    if (v > 0) {
        ObjIndex idx;
        idx.raw = static_cast<uint64_t>(v - 1);  // Export type bits are 0
        return idx;
    }
    size_t i = static_cast<size_t>(-v - 1);
    return i < import_map_.size() ? import_map_[i] : ObjIndex{};
}

std::string Package::resolve_local_name(int32_t name_index, int32_t extra) const {
    if (name_index < 0 || static_cast<size_t>(name_index) >= name_map_.size())
        return "<bad-name>";
    if (extra == 0) return name_map_[static_cast<size_t>(name_index)];
    return name_map_[static_cast<size_t>(name_index)] + "_" + std::to_string(extra - 1);
}

std::string Package::resolve_object_name(ObjIndex idx) {
    if (idx.is_null()) return "None";
    if (idx.is_script_import()) {
        const ScriptObjectEntry* e = provider_.global_data().find(idx);
        return e ? provider_.global_data().resolve(e->object_name) : "<missing script import>";
    }
    auto r = resolve_export(idx);
    if (!r) return "<unresolved>";
    return r->first->resolve(r->first->exports_[r->second].object_name);
}

namespace {
constexpr uint32_t kBulkUnused = 1u << 5;
constexpr uint32_t kBulkPayloadInSeparateFile = 1u << 8;
constexpr uint32_t kBulkOptionalPayload = 1u << 11;
constexpr uint32_t kBulkMemoryMapped = 1u << 12;

std::string without_extension(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of('/');
    return (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        ? path.substr(0, dot) : path;
}
}  // namespace

std::vector<uint8_t> Package::read_bulk_data(Reader& r, int32_t data_index) {
    if (data_index < 0 || static_cast<size_t>(data_index) >= bulk_data_map_.size())
        throw std::runtime_error("bulk data index out of range");
    const BulkDataEntry& e = bulk_data_map_[static_cast<size_t>(data_index)];
    if (e.serial_size == 0 || (e.flags & kBulkUnused)) return {};

    const char* ext = nullptr;
    if ((e.flags & kBulkPayloadInSeparateFile) && (e.flags & kBulkMemoryMapped)) ext = "m.ubulk";
    else if (e.flags & kBulkPayloadInSeparateFile) ext = "ubulk";
    else if (e.flags & kBulkOptionalPayload) ext = "uptnl";

    if (!ext) {
        // ForceInlinePayload (or LazyLoadable/None): the data is the
        // next serial_size bytes of the CURRENT stream, right where
        // the index was read -- SerialOffset plays no part here.
        auto bytes = r.bytes(static_cast<size_t>(e.serial_size));
        return {bytes.begin(), bytes.end()};
    }

    std::string base = without_extension(path_);
    std::string sibling = e.cooked_index == 0
        ? base + "." + ext
        : base + "." + [i = e.cooked_index] {
              char buf[8]; std::snprintf(buf, sizeof(buf), "%03u", i); return std::string(buf);
          }() + "." + ext;
    std::vector<uint8_t> file;
    if (!provider_.read_raw_file(sibling, file))
        throw std::runtime_error("missing bulk sibling file " + sibling);
    uint64_t offset = file.size() == e.serial_size ? 0 : e.serial_offset;
    if (offset + e.serial_size > file.size())
        throw std::runtime_error("bulk data range out of bounds in " + sibling);
    return {file.begin() + static_cast<ptrdiff_t>(offset),
           file.begin() + static_cast<ptrdiff_t>(offset + e.serial_size)};
}

std::string Package::virtual_path() const {
    // Mirrors CUE4Parse's FixPath for the common cases: the project's
    // own content and Engine content. Plugin content (a third mount
    // root) isn't remapped -- rare for mesh references, and the raw
    // path is still a usable, unique lookup key.
    static constexpr const char* kProject = "OakGame/Content/";
    static constexpr const char* kEngine = "Engine/Content/";
    std::string p = path_;
    if (p.compare(0, std::strlen(kProject), kProject) == 0)
        p = "/Game/" + p.substr(std::strlen(kProject));
    else if (p.compare(0, std::strlen(kEngine), kEngine) == 0)
        p = "/Engine/" + p.substr(std::strlen(kEngine));
    else if (!p.empty() && p[0] != '/')
        p = "/" + p;
    size_t dot = p.find_last_of('.');
    size_t slash = p.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        p = p.substr(0, dot);
    return p;
}

std::string Package::resolve_asset_path(ObjIndex idx) {
    auto r = resolve_export(idx);
    if (!r) return "";
    std::string name = r->first->resolve(r->first->exports_[r->second].object_name);
    return r->first->virtual_path() + "." + name;
}

}  // namespace bl4
