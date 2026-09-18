#pragma once
// Material instance -> base-colour texture: follows a
// UMaterialInstanceConstant's TextureParameterValues, then its Parent's,
// up the chain until one names a colour map. BL4's materials are layered
// (MaterialLayers/Blends on the template instances) and a real renderer
// would evaluate that whole graph; this only finds the one texture that
// best stands in for "what colour is this surface".
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bl4 {

class Package;
class Usmap;

struct MaterialTexture {
    Package* pkg = nullptr;
    uint32_t export_index = 0;
    std::string name;       // texture object name, e.g. "T_FT_Concrete_Clean_D"
    std::string parameter;  // parameter it was bound to, e.g. "BaseColor"
};

// Every texture parameter the chain binds, nearest instance first --
// what find_base_color_texture chooses from (exposed for diagnostics).
std::vector<MaterialTexture> list_texture_parameters(Package& pkg, const Usmap& usmap);

// The best base-colour candidate in pkg's material chain, or nullopt if
// no texture parameter looks like one.
std::optional<MaterialTexture> find_base_color_texture(Package& pkg, const Usmap& usmap);

}  // namespace bl4
