#include "pkg/property.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cctype>
#include <cstdio>

#include "core/reader.hpp"
#include "pkg/package.hpp"

namespace bl4 {
namespace {

struct Fragment {
    uint8_t skip_num;
    bool has_zero;
    uint8_t value_num;
    bool is_last;
};

Fragment parse_fragment(uint16_t packed) {
    return {static_cast<uint8_t>(packed & 0x7Fu), (packed & 0x0080u) != 0,
           static_cast<uint8_t>(packed >> 9), (packed & 0x0100u) != 0};
}

struct UnversionedHeader {
    std::vector<Fragment> fragments;
    std::vector<bool> zero_mask;
    bool has_values = false;
};

UnversionedHeader read_header(Reader& r) {
    UnversionedHeader h;
    int zero_mask_num = 0, unmasked_num = 0;
    Fragment f{};
    do {
        f = parse_fragment(r.read<uint16_t>());
        h.fragments.push_back(f);
        (f.has_zero ? zero_mask_num : unmasked_num) += f.value_num;
    } while (!f.is_last);

    if (zero_mask_num > 0) {
        size_t nbytes = zero_mask_num <= 8 ? 1 : zero_mask_num <= 16 ? 2
                        : ((static_cast<size_t>(zero_mask_num) + 31) / 32) * 4;
        auto bytes = r.bytes(nbytes);
        h.zero_mask.resize(static_cast<size_t>(zero_mask_num));
        bool any_false = false;
        for (int i = 0; i < zero_mask_num; ++i) {
            bool bit = (bytes[static_cast<size_t>(i) / 8] >> (i % 8)) & 1;
            h.zero_mask[static_cast<size_t>(i)] = bit;
            any_false |= !bit;
        }
        h.has_values = unmasked_num > 0 || any_false;
    } else {
        h.has_values = unmasked_num > 0;
    }
    return h;
}

// Mirrors CUE4Parse's FIterator: walks fragment-encoded schema indices.
class FragIter {
public:
    explicit FragIter(const UnversionedHeader& h) : h_(h) { skip(); }

    int index() const { return schema_it_; }
    bool is_nonzero() const {
        const Fragment& f = h_.fragments[frag_idx_];
        return !f.has_zero || !h_.zero_mask.at(static_cast<size_t>(zero_mask_index_));
    }
    bool move_next() {
        const Fragment* f = &h_.fragments[frag_idx_];
        ++schema_it_;
        --remaining_;
        if (f->has_zero) ++zero_mask_index_;
        if (remaining_ == 0) {
            if (f->is_last) return false;
            ++frag_idx_;
            skip();
        }
        return true;
    }

private:
    void skip() {
        schema_it_ += h_.fragments[frag_idx_].skip_num;
        while (h_.fragments[frag_idx_].value_num == 0) {
            if (h_.fragments[frag_idx_].is_last)
                throw ParseError("unversioned header: last fragment has no values");
            ++frag_idx_;
            schema_it_ += h_.fragments[frag_idx_].skip_num;
        }
        remaining_ = h_.fragments[frag_idx_].value_num;
    }

    const UnversionedHeader& h_;
    size_t frag_idx_ = 0;
    int schema_it_ = 0;
    int zero_mask_index_ = 0;
    int remaining_ = 0;
};

// Walks struct_name's own usmap Struct, then its SuperType chain,
// matching Struct.TryGetValue in CUE4Parse's MappingsSchema.cs.
bool find_property(const Usmap& usmap, const UsmapStruct* s, int index,
                   const UsmapProperty*& out) {
    while (s) {
        auto it = s->properties.find(index);
        if (it != s->properties.end()) { out = &it->second; return true; }
        if (index < s->own_property_count) return false;
        index -= s->own_property_count;
        s = s->super_type.empty() ? nullptr : usmap.find(s->super_type);
    }
    return false;
}

PropertyValue read_value(Reader& r, const Usmap& usmap, Package& pkg,
                         const UsmapPropertyType& type, bool zero);

Vec3 read_vec3(Reader& r) {
    return {r.read<double>(), r.read<double>(), r.read<double>()};
}
Vec4 read_vec4(Reader& r) {
    return {r.read<double>(), r.read<double>(), r.read<double>(), r.read<double>()};
}
// Quat/Vector/Vector2D/Rotator/Vector4/Plane all read one FReal (a
// double, since BL4 always has LARGE_WORLD_COORDINATES) per component
// via their own ctor; only order differs (Quat is xyzw, Rotator is
// pitch/yaw/roll).
Vec4 read_quat(Reader& r) { return read_vec4(r); }

PropertyValue read_struct(Reader& r, const Usmap& usmap, Package& pkg,
                          const std::string& struct_type, bool zero) {
    PropertyValue v;
    const std::string& t = struct_type;
    // CUE4Parse's hardcoded-layout structs (FScriptStruct.cs); anything
    // else recurses into the generic unversioned struct reader below.
    if (t == "Vector" || t == "Rotator") {
        v.kind = PropertyValue::Kind::Vec3;
        v.v3 = zero ? Vec3{} : read_vec3(r);
        return v;
    }
    if (t == "Vector2D") {
        v.kind = PropertyValue::Kind::Vec3;
        v.v3 = zero ? Vec3{} : Vec3{r.read<double>(), r.read<double>(), 0};
        return v;
    }
    if (t == "Quat" || t == "Vector4") {
        v.kind = PropertyValue::Kind::Vec4;
        v.v4 = zero ? Vec4{} : read_quat(r);
        return v;
    }
    if (t == "Color") {
        v.kind = PropertyValue::Kind::Vec4;
        if (!zero) {
            uint8_t b = r.read<uint8_t>(), g = r.read<uint8_t>(),
                    rr = r.read<uint8_t>(), a = r.read<uint8_t>();
            v.v4 = {rr / 255.0, g / 255.0, b / 255.0, a / 255.0};
        }
        return v;
    }
    if (t == "LinearColor") {
        v.kind = PropertyValue::Kind::Vec4;
        v.v4 = zero ? Vec4{}
                    : Vec4{r.read<float>(), r.read<float>(), r.read<float>(), r.read<float>()};
        return v;
    }
    if (t == "Guid") {
        v.kind = PropertyValue::Kind::Guid;
        if (!zero) {
            char buf[33];
            uint32_t a = r.read<uint32_t>(), b = r.read<uint32_t>(),
                     c = r.read<uint32_t>(), d = r.read<uint32_t>();
            std::snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", a, b, c, d);
            v.s.assign(buf, 32);
        }
        return v;
    }
    if (t == "Box") {
        v.kind = PropertyValue::Kind::Bag;
        v.bag = std::make_shared<PropertyBag>();
        if (!zero) {
            PropertyValue mn, mx;
            mn.kind = mx.kind = PropertyValue::Kind::Vec3;
            mn.v3 = read_vec3(r);
            mx.v3 = read_vec3(r);
            v.bag->set("Min", std::move(mn));
            v.bag->set("Max", std::move(mx));
            r.read<uint8_t>();  // IsValid
        }
        return v;
    }
    if (t == "IntPoint") {
        v.kind = PropertyValue::Kind::Vec3;
        v.v3 = zero ? Vec3{} : Vec3{double(r.read<int32_t>()), double(r.read<int32_t>()), 0};
        return v;
    }
    if (t == "PerPlatformInt" || t == "PerPlatformFloat" || t == "PerPlatformBool") {
        // TPerPlatformProperty<T> has a custom Serialize(), not a tagged
        // property list: bool bCooked, T Default, then (only if !bCooked
        // and this package keeps editor-only data) a Map<FName,T> of
        // per-platform overrides we don't need and just skip past.
        if (zero) {
            v.kind = t == "PerPlatformBool" ? PropertyValue::Kind::Bool
                    : t == "PerPlatformFloat" ? PropertyValue::Kind::Double : PropertyValue::Kind::Int;
            return v;
        }
        bool cooked = r.read_bool();
        if (t == "PerPlatformInt") { v.kind = PropertyValue::Kind::Int; v.i = r.read<int32_t>(); }
        else if (t == "PerPlatformFloat") { v.kind = PropertyValue::Kind::Double; v.d = r.read<float>(); }
        else { v.kind = PropertyValue::Kind::Bool; v.b = r.read_bool(); }
        if (!cooked && pkg.is_filter_editor_only()) {
            int32_t count = r.read_count();
            for (int32_t i = 0; i < count; ++i) {
                r.skip(8);  // FName key (name index + extra)
                if (t == "PerPlatformInt") r.read<int32_t>();
                else if (t == "PerPlatformFloat") r.read<float>();
                else r.read_bool();
            }
        }
        return v;
    }

    // Generic (usmap-defined) struct: recurse using the exact same
    // unversioned reader, keyed by struct name instead of class name.
    v.kind = PropertyValue::Kind::Bag;
    if (zero) { v.bag = std::make_shared<PropertyBag>(); return v; }
    v.bag = std::make_shared<PropertyBag>(read_unversioned_properties(r, usmap, pkg, t));
    return v;
}

PropertyValue read_value(Reader& r, const Usmap& usmap, Package& pkg,
                         const UsmapPropertyType& type, bool zero) {
    PropertyValue v;
    switch (type.type) {
        case EPropType::Bool:
            v.kind = PropertyValue::Kind::Bool;
            v.b = zero ? false : r.read<uint8_t>() != 0;
            return v;
        case EPropType::Byte:
            v.kind = PropertyValue::Kind::Int;
            v.i = zero ? 0 : r.read<uint8_t>();
            return v;
        case EPropType::Int8:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<int8_t>(); return v;
        case EPropType::Int16:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<int16_t>(); return v;
        case EPropType::UInt16:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<uint16_t>(); return v;
        case EPropType::Int:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<int32_t>(); return v;
        case EPropType::UInt32:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<uint32_t>(); return v;
        case EPropType::Int64:
            v.kind = PropertyValue::Kind::Int; v.i = zero ? 0 : r.read<int64_t>(); return v;
        case EPropType::UInt64:
            v.kind = PropertyValue::Kind::Int;
            v.i = zero ? 0 : static_cast<int64_t>(r.read<uint64_t>());
            return v;
        case EPropType::Float:
            v.kind = PropertyValue::Kind::Double; v.d = zero ? 0 : r.read<float>(); return v;
        case EPropType::Double:
            v.kind = PropertyValue::Kind::Double; v.d = zero ? 0 : r.read<double>(); return v;
        case EPropType::Name: {
            v.kind = PropertyValue::Kind::Name;
            if (!zero) {
                int32_t idx = r.read<int32_t>();
                int32_t extra = r.read<int32_t>();
                v.s = pkg.resolve_local_name(idx, extra);
            }
            return v;
        }
        case EPropType::Str:
        case EPropType::AnsiStr:
        case EPropType::Utf8Str:
            v.kind = PropertyValue::Kind::Str;
            v.s = zero ? std::string() : r.read_fstring();
            return v;
        case EPropType::Object:
        case EPropType::Class: {
            v.kind = PropertyValue::Kind::Object;
            v.obj = zero ? ObjIndex{} : pkg.resolve_legacy_index(r.read<int32_t>());
            return v;
        }
        case EPropType::SoftObject:
        case EPropType::AssetObject: {
            // FSoftObjectPath (BL4 is past FSOFTOBJECTPATH_REMOVE_ASSET_PATH_FNAMES
            // and before SoftObjectPathUtf8SubPaths): FTopLevelAssetPath
            // (2 FNames) then a regular FString sub-path.
            v.kind = PropertyValue::Kind::Str;
            if (!zero) {
                int32_t pkg_idx = r.read<int32_t>(), pkg_extra = r.read<int32_t>();
                int32_t asset_idx = r.read<int32_t>(), asset_extra = r.read<int32_t>();
                std::string sub = r.read_fstring();
                v.s = pkg.resolve_local_name(pkg_idx, pkg_extra) + "." +
                      pkg.resolve_local_name(asset_idx, asset_extra);
                if (!sub.empty()) v.s += ":" + sub;
            }
            return v;
        }
        case EPropType::Enum: {
            // Index only; we don't need the enum's textual value for
            // any placement-relevant property today.
            v.kind = PropertyValue::Kind::Int;
            if (!zero) {
                if (type.inner)
                    v.i = read_value(r, usmap, pkg, *type.inner, false).i;
                else
                    v.i = r.read<uint8_t>();
            }
            return v;
        }
        case EPropType::Struct:
            return read_struct(r, usmap, pkg, type.struct_type, zero);
        case EPropType::Array: {
            v.kind = PropertyValue::Kind::Array;
            if (zero || !type.inner) return v;
            int32_t count = r.read_count();
            v.arr.reserve(static_cast<size_t>(count));
            bool as_enum = type.inner->type == EPropType::Byte &&
                          !type.inner->enum_name.empty() &&
                          type.inner->enum_name != "None";
            for (int32_t i = 0; i < count; ++i) {
                if (as_enum) {
                    PropertyValue e;
                    e.kind = PropertyValue::Kind::Int;
                    e.i = r.read<uint8_t>();
                    v.arr.push_back(std::move(e));
                } else {
                    v.arr.push_back(read_value(r, usmap, pkg, *type.inner, false));
                }
            }
            return v;
        }
        default:
            throw ParseError("unsupported property type " +
                             std::to_string(static_cast<int>(type.type)));
    }
}

}  // namespace

PropertyBag read_unversioned_properties(Reader& r, const Usmap& usmap,
                                        Package& pkg, const std::string& type_name) {
    PropertyBag bag;
    const UsmapStruct* s = usmap.find(type_name);
    if (!s) throw ParseError("no usmap mapping for '" + type_name + "'");

    UnversionedHeader header = read_header(r);
    if (!header.has_values) return bag;

    FragIter it(header);
    for (;;) {
        int index = it.index();
        bool nonzero = it.is_nonzero();
        const UsmapProperty* prop = nullptr;
        bool found = find_property(usmap, s, index, prop);
        bool trace = std::getenv("BL4X_TRACE") != nullptr;
        size_t p0 = r.pos();
        if (nonzero) {
            if (!found)
                throw ParseError("unknown property index " + std::to_string(index) +
                                 " in '" + type_name + "'");
            PropertyValue v = read_value(r, usmap, pkg, prop->type, false);
            if (trace)
                std::cerr << "    " << type_name << "." << prop->name << " idx=" << index
                          << " size=" << (r.pos() - p0) << " pos=" << r.pos() << "\n";
            bag.set(prop->name, std::move(v));
        } else if (found) {
            bag.set(prop->name, read_value(r, usmap, pkg, prop->type, true));
        }
        if (!it.move_next()) break;
    }
    return bag;
}

double PropertyBag::get_double(const std::string& name, double def) const {
    const PropertyValue* v = find(name);
    if (!v) return def;
    if (v->kind == PropertyValue::Kind::Double) return v->d;
    if (v->kind == PropertyValue::Kind::Int) return static_cast<double>(v->i);
    return def;
}
bool PropertyBag::get_bool(const std::string& name, bool def) const {
    const PropertyValue* v = find(name);
    return v && v->kind == PropertyValue::Kind::Bool ? v->b : def;
}
Vec3 PropertyBag::get_vec3(const std::string& name, Vec3 def) const {
    const PropertyValue* v = find(name);
    return v && v->kind == PropertyValue::Kind::Vec3 ? v->v3 : def;
}
Vec4 PropertyBag::get_vec4(const std::string& name, Vec4 def) const {
    const PropertyValue* v = find(name);
    return v && v->kind == PropertyValue::Kind::Vec4 ? v->v4 : def;
}
ObjIndex PropertyBag::get_object(const std::string& name) const {
    const PropertyValue* v = find(name);
    return v && v->kind == PropertyValue::Kind::Object ? v->obj : ObjIndex{};
}
std::string PropertyBag::get_str(const std::string& name, std::string def) const {
    const PropertyValue* v = find(name);
    return v && (v->kind == PropertyValue::Kind::Str || v->kind == PropertyValue::Kind::Name)
        ? v->s : def;
}
std::vector<ObjIndex> PropertyBag::get_object_array(const std::string& name) const {
    std::vector<ObjIndex> out;
    const PropertyValue* v = find(name);
    if (!v || v->kind != PropertyValue::Kind::Array) return out;
    out.reserve(v->arr.size());
    for (auto& e : v->arr)
        out.push_back(e.kind == PropertyValue::Kind::Object ? e.obj : ObjIndex{});
    return out;
}

}  // namespace bl4
