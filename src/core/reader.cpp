#include "core/reader.hpp"

namespace bl4 {

std::string utf16_to_utf8(const char16_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t lo = s[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string Reader::read_fstring() {
    int32_t len = read<int32_t>();
    if (len == 0) return {};
    if (len > 0) {
        auto b = bytes(static_cast<size_t>(len));
        size_t n = b.size();
        while (n && b[n - 1] == 0) --n;
        return std::string(reinterpret_cast<const char*>(b.data()), n);
    }
    if (len == INT32_MIN) fail("bad fstring length");
    size_t count = static_cast<size_t>(-len);
    auto b = bytes(count * 2);
    std::u16string tmp(count, u'\0');
    std::memcpy(tmp.data(), b.data(), count * 2);
    while (!tmp.empty() && tmp.back() == 0) tmp.pop_back();
    return utf16_to_utf8(tmp.data(), tmp.size());
}

}  // namespace bl4
