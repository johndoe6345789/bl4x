#pragma once
// FPackageObjectIndex: a tagged 64-bit reference used throughout Zen
// packages -- an export in this package, a native (script) class/
// function, or an export in another package (matched by hash, since
// Zen packages carry no by-name import table).
#include <cstdint>

namespace bl4 {

enum class ObjIndexType : uint8_t { Export = 0, ScriptImport = 1, PackageImport = 2, Null = 3 };

struct PackageImportRef {
    uint32_t imported_package_index;
    uint32_t imported_public_export_hash_index;
};

struct ObjIndex {
    uint64_t raw = ~0ull;

    static constexpr int kIndexBits = 62;
    static constexpr uint64_t kIndexMask = (1ull << kIndexBits) - 1ull;

    bool is_null() const { return raw == ~0ull; }
    ObjIndexType type() const { return static_cast<ObjIndexType>(raw >> kIndexBits); }
    bool is_export() const { return !is_null() && type() == ObjIndexType::Export; }
    bool is_script_import() const { return !is_null() && type() == ObjIndexType::ScriptImport; }
    bool is_package_import() const { return !is_null() && type() == ObjIndexType::PackageImport; }
    uint64_t value() const { return raw & kIndexMask; }

    PackageImportRef package_import_ref() const {
        return {static_cast<uint32_t>((raw & kIndexMask) >> 32),
                static_cast<uint32_t>(raw)};
    }
};

}  // namespace bl4
