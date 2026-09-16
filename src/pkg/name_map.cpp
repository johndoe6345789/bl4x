#include "pkg/name_map.hpp"

namespace bl4 {

std::vector<std::string> load_name_batch(Reader& r) {
    int32_t count = r.read<int32_t>();
    if (count <= 0) return {};
    r.skip(4);                          // numStringBytes
    r.skip(8);                          // hash version
    r.skip(static_cast<size_t>(count) * 8);  // per-name hashes

    struct Header { uint8_t a, b; };
    auto headers = r.read_array<Header>(static_cast<size_t>(count));

    std::vector<std::string> names(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        bool is_utf16 = (headers[i].a & 0x80) != 0;
        uint32_t len = ((headers[i].a & 0x7Fu) << 8) + headers[i].b;
        if (is_utf16) {
            auto bytes = r.bytes(size_t(len) * 2);
            names[i] = utf16_to_utf8(reinterpret_cast<const char16_t*>(bytes.data()), len);
        } else {
            auto bytes = r.bytes(len);
            names[i].assign(reinterpret_cast<const char*>(bytes.data()), len);
        }
    }
    return names;
}

std::string resolve_mapped_name(const MappedName& m,
                                const std::vector<std::string>& name_map) {
    if (m.index >= name_map.size()) return "<bad-name>";
    if (m.extra == 0) return name_map[m.index];
    return name_map[m.index] + "_" + std::to_string(m.extra - 1);
}

}  // namespace bl4
