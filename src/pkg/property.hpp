#pragma once
// Unversioned property values: BL4's packages carry no per-property
// size/type tags (CUE4Parse calls this "unversioned properties"), so
// reading them needs the .usmap-described layout of each class/struct
// (fragment header + zero mask, then one FPropertyTag per non-default
// slot) -- see D:\BL4Export\src\cpp\README.md for the format notes.
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg/obj_index.hpp"
#include "pkg/usmap.hpp"

namespace bl4 {

class Reader;
class Package;

struct Vec3 { double x = 0, y = 0, z = 0; };
struct Vec4 { double x = 0, y = 0, z = 0, w = 0; };

class PropertyBag;

struct PropertyValue {
    enum class Kind {
        None, Bool, Int, Double, Name, Str, Object, Vec3, Vec4, Guid,
        Array, Bag,
    } kind = Kind::None;

    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    ObjIndex obj;   // legacy int32 FPackageIndex, already resolved to an ObjIndex
    Vec3 v3;
    Vec4 v4;        // also Quat(x,y,z,w), Rotator(pitch,yaw,roll,0), Color/LinearColor(r,g,b,a)
    std::vector<PropertyValue> arr;
    std::shared_ptr<PropertyBag> bag;
};

// One object's (or one struct instance's) decoded properties, by name.
// Only non-default-valued properties are stored; callers ask with a
// default the same way CUE4Parse's GetOrDefault does.
class PropertyBag {
public:
    void set(std::string name, PropertyValue v) { props_[std::move(name)] = std::move(v); }
    const PropertyValue* find(const std::string& name) const {
        auto it = props_.find(name);
        return it == props_.end() ? nullptr : &it->second;
    }

    bool get_bool(const std::string& name, bool def = false) const;
    double get_double(const std::string& name, double def = 0) const;
    Vec3 get_vec3(const std::string& name, Vec3 def = {}) const;
    Vec4 get_vec4(const std::string& name, Vec4 def = {}) const;
    ObjIndex get_object(const std::string& name) const;
    std::vector<ObjIndex> get_object_array(const std::string& name) const;
    std::string get_str(const std::string& name, std::string def = "") const;

private:
    std::unordered_map<std::string, PropertyValue> props_;
};

// Reads one object's (or embedded struct's) full unversioned property
// block for class/struct `type_name`, using `usmap` for layout and
// `pkg` to resolve FName text and legacy FPackageIndex refs. Throws
// ParseError if `type_name` (or a struct it references) isn't mapped,
// or on any other format mismatch -- callers should treat that as
// "this object can't be parsed" and move on to independent siblings.
PropertyBag read_unversioned_properties(Reader& r, const Usmap& usmap,
                                        Package& pkg, const std::string& type_name);

}  // namespace bl4
