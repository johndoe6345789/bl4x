#include "pkg/material.hpp"

#include <algorithm>
#include <cctype>
#include <exception>

#include "pkg/object.hpp"
#include "pkg/package.hpp"
#include "pkg/property.hpp"

namespace bl4 {
namespace {

constexpr int kMaxParentHops = 8;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

bool ends_with(const std::string& s, const char* suffix) {
    std::string t(suffix);
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::optional<uint32_t> material_export(Package& pkg) {
    for (uint32_t i = 0; i < pkg.export_count(); ++i) {
        if (contains(pkg.resolve_object_name(pkg.export_at(i).class_index), "Material")) return i;
    }
    return std::nullopt;
}

// 0 = not a colour map. Parameter names are the strongest signal, then
// UE's texture naming convention (_D / _BC / _Diffuse suffixes).
int base_color_score(const MaterialTexture& t) {
    const std::string p = lower(t.parameter), n = lower(t.name);
    for (const char* bad : {"normal", "rough", "mask", "metal", "comp", "gwc", "emiss", "height",
                            "opacity", "noise", "lut", "spec", "bump", "orm", "grime", "wear"}) {
        if (contains(p, bad)) return 0;
    }
    if (contains(p, "basecolo") || contains(p, "base colo") || contains(p, "albedo") ||
        contains(p, "diffuse") || p == "color" || p == "colour") return 3;
    if (ends_with(n, "_d") || ends_with(n, "_bc") || ends_with(n, "_diffuse") ||
        ends_with(n, "_albedo") || ends_with(n, "_basecolor")) return 2;
    if (contains(p, "color") || contains(p, "colour")) return 1;
    return 0;
}

void append_parameters(Package& pkg, const PropertyBag& props, std::vector<MaterialTexture>& out) {
    const PropertyValue* values = props.find("TextureParameterValues");
    if (!values || values->kind != PropertyValue::Kind::Array) return;
    for (const PropertyValue& entry : values->arr) {
        if (entry.kind != PropertyValue::Kind::Bag || !entry.bag) continue;
        const PropertyValue* info = entry.bag->find("ParameterInfo");
        ObjIndex texture = entry.bag->get_object("ParameterValue");
        if (texture.is_null()) continue;
        auto target = pkg.resolve_export(texture);
        if (!target) continue;
        MaterialTexture t;
        t.pkg = target->first;
        t.export_index = target->second;
        t.name = pkg.resolve_object_name(texture);
        if (info && info->bag) t.parameter = info->bag->get_str("Name");
        out.push_back(std::move(t));
    }
}

}  // namespace

std::vector<MaterialTexture> list_texture_parameters(Package& start, const Usmap& usmap) {
    std::vector<MaterialTexture> out;
    Package* pkg = &start;
    for (int hop = 0; pkg && hop < kMaxParentHops; ++hop) {
        auto index = material_export(*pkg);
        if (!index) break;
        PropertyBag props;
        try {
            props = load_properties(*pkg, usmap, *index);
        } catch (const std::exception&) {
            break;  // base UMaterials carry no parameter array we can read
        }
        append_parameters(*pkg, props, out);
        ObjIndex parent = props.get_object("Parent");
        if (parent.is_null()) break;
        auto target = pkg->resolve_export(parent);
        pkg = target ? target->first : nullptr;
    }
    return out;
}

std::optional<MaterialTexture> find_base_color_texture(Package& pkg, const Usmap& usmap) {
    std::optional<MaterialTexture> best;
    int best_score = 0;
    for (MaterialTexture& t : list_texture_parameters(pkg, usmap)) {
        int score = base_color_score(t);
        if (score > best_score) {  // strict: the nearest instance wins ties
            best_score = score;
            best = std::move(t);
        }
    }
    return best;
}

}  // namespace bl4
