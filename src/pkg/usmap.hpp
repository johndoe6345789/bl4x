#pragma once
// Parses a .usmap mappings file into per-class/struct property lists.
// Needed because BL4 ships unversioned property serialization: the
// property list isn't self-describing, so we need the same external
// type info FModel/CUE4Parse use to read it.
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bl4 {

enum class EPropType : uint8_t {
    Byte, Bool, Int, Float, Object, Name, Delegate, Double, Array, Struct,
    Str, Text, Interface, MulticastDelegate, WeakObject, LazyObject,
    AssetObject, SoftObject, UInt64, UInt32, UInt16, Int64, Int16, Int8,
    Map, Set, Enum, FieldPath, Optional, Utf8Str, AnsiStr,
    Class = 31, MulticastInlineDelegate, SoftClass, VerseString,
    VerseDynamic, VerseFunction,
    CustomFD = 0xFD, CustomFE = 0xFE, Unknown = 0xFF,
};

struct UsmapPropertyType {
    EPropType type;
    std::string struct_type;                    // StructProperty
    std::string enum_name;                       // (Enum|Byte)Property
    std::unique_ptr<UsmapPropertyType> inner;     // Array/Set/Optional
    std::unique_ptr<UsmapPropertyType> key;       // Map
    std::unique_ptr<UsmapPropertyType> value;     // Map
};

struct UsmapProperty {
    std::string name;
    int local_index;   // sub-index within a fixed C array (usually 0)
    UsmapPropertyType type;
};

struct UsmapStruct {
    std::string name;
    std::string super_type;
    std::unordered_map<int, UsmapProperty> properties;  // by schema index
    int own_property_count = 0;
};

class Usmap {
public:
    explicit Usmap(const std::string& path);

    // nullptr if not mapped. Case-insensitive, matching CUE4Parse.
    const UsmapStruct* find(const std::string& name) const;

private:
    std::unordered_map<std::string, UsmapStruct> structs_;  // lowercased key
};

}  // namespace bl4
