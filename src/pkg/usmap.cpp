#include "pkg/usmap.hpp"

#include <algorithm>
#include <cctype>

#include "core/reader.hpp"
#include "io/file.hpp"
#include "io/oodle.hpp"

namespace bl4 {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Reads a length-prefixed name from the usmap string table; -1 means
// "no name" (used for e.g. a struct with no super).
std::string read_name(Reader& r, const std::vector<std::string>& lut) {
    int32_t idx = r.read<int32_t>();
    return idx < 0 ? std::string() : lut.at(static_cast<size_t>(idx));
}

UsmapPropertyType clone(const UsmapPropertyType& t) {
    UsmapPropertyType c;
    c.type = t.type;
    c.struct_type = t.struct_type;
    c.enum_name = t.enum_name;
    if (t.inner) c.inner = std::make_unique<UsmapPropertyType>(clone(*t.inner));
    if (t.key) c.key = std::make_unique<UsmapPropertyType>(clone(*t.key));
    if (t.value) c.value = std::make_unique<UsmapPropertyType>(clone(*t.value));
    return c;
}

UsmapPropertyType read_prop_type(Reader& r, const std::vector<std::string>& lut) {
    UsmapPropertyType t;
    t.type = static_cast<EPropType>(r.read<uint8_t>());
    switch (t.type) {
        case EPropType::Enum:
            t.inner = std::make_unique<UsmapPropertyType>(read_prop_type(r, lut));
            t.enum_name = read_name(r, lut);
            break;
        case EPropType::Struct:
            t.struct_type = read_name(r, lut);
            break;
        case EPropType::Set:
        case EPropType::Array:
        case EPropType::Optional:
            t.inner = std::make_unique<UsmapPropertyType>(read_prop_type(r, lut));
            break;
        case EPropType::Map:
            t.key = std::make_unique<UsmapPropertyType>(read_prop_type(r, lut));
            t.value = std::make_unique<UsmapPropertyType>(read_prop_type(r, lut));
            break;
        default:
            break;
    }
    return t;
}

}  // namespace

Usmap::Usmap(const std::string& path) {
    auto file = File::read_all(path);
    Reader r(file);

    if (r.read<uint16_t>() != 0x30C4) r.fail("bad usmap magic");
    uint8_t version = r.read<uint8_t>();
    bool has_versioning = version >= 1 && r.read_bool();
    if (has_versioning) {
        if (version >= 5) {                     // EngineVersioning
            r.skip(2 + 2 + 2 + 4);               // major/minor/patch(u16 each)+changelist(u32)
            r.read_fstring();                    // branch
        }
        r.skip(8);                              // package version (2x int32)
        r.skip(4);                              // licensee version
        int32_t n = r.read_count();
        r.skip(static_cast<size_t>(n) * 20);    // FCustomVersion[]: guid+int32
        r.skip(4);                              // net CL
    }

    uint8_t compression = r.read<uint8_t>();
    uint32_t comp_size = r.read<uint32_t>();
    uint32_t decomp_size = r.read<uint32_t>();
    std::vector<uint8_t> body(decomp_size);
    if (compression == 0) {
        if (comp_size != decomp_size) r.fail("usmap: None compression size mismatch");
        auto b = r.bytes(comp_size);
        std::copy(b.begin(), b.end(), body.begin());
    } else if (compression == 1) {  // Oodle
        auto b = r.bytes(comp_size);
        oodle::decompress(b.data(), b.size(), body.data(), body.size());
    } else {
        throw std::runtime_error("usmap: unsupported compression method " +
                                 std::to_string(compression));
    }

    Reader br(body);
    uint32_t name_count = br.read<uint32_t>();
    std::vector<std::string> names(name_count);
    for (auto& n : names) {
        uint32_t len = version >= 2 ? br.read<uint16_t>() : br.read<uint8_t>();
        auto b = br.bytes(len);
        n.assign(reinterpret_cast<const char*>(b.data()), len);
    }

    uint32_t enum_count = br.read<uint32_t>();
    for (uint32_t i = 0; i < enum_count; ++i) {
        read_name(br, names);
        uint32_t n = version >= 3 ? br.read<uint16_t>() : br.read<uint8_t>();
        for (uint32_t j = 0; j < n; ++j) {
            if (version >= 4) { br.skip(8); read_name(br, names); }
            else read_name(br, names);
        }
    }

    uint32_t struct_count = br.read<uint32_t>();
    structs_.reserve(struct_count * 2);
    for (uint32_t i = 0; i < struct_count; ++i) {
        UsmapStruct s;
        s.name = read_name(br, names);
        s.super_type = read_name(br, names);
        uint16_t prop_count = br.read<uint16_t>();
        uint16_t serializable_count = br.read<uint16_t>();
        s.own_property_count = prop_count;
        for (uint16_t j = 0; j < serializable_count; ++j) {
            uint16_t index = br.read<uint16_t>();
            uint8_t array_dim = br.read<uint8_t>();
            std::string name = read_name(br, names);
            UsmapPropertyType type = read_prop_type(br, names);
            for (uint8_t k = 0; k < array_dim; ++k) {
                UsmapProperty p;
                p.name = name;
                p.local_index = k;
                p.type = clone(type);
                s.properties.emplace(index + k, std::move(p));
            }
        }
        structs_[lower(s.name)] = std::move(s);
    }
}

const UsmapStruct* Usmap::find(const std::string& name) const {
    auto it = structs_.find(lower(name));
    return it == structs_.end() ? nullptr : &it->second;
}

}  // namespace bl4
