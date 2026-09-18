#include "pkg/texture_export.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "pkg/bc_decode.hpp"
#include "pkg/texture.hpp"

namespace bl4 {
namespace {

struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA8, top row first
};

std::string bare_format(const std::string& f) { return f.rfind("PF_", 0) == 0 ? f.substr(3) : f; }

bool decode_mip(const TextureMip& mip, const std::string& format, Rgba& out) {
    const std::string f = bare_format(format);
    size_t count = static_cast<size_t>(mip.width) * static_cast<size_t>(mip.height);
    out.width = mip.width;
    out.height = mip.height;
    if (bc_format_supported(f)) {
        out.pixels = decode_bc_image(mip.data.data(), mip.data.size(), mip.width, mip.height, f);
        return true;
    }
    if ((f == "R8G8B8A8" || f == "B8G8R8A8") && mip.data.size() >= count * 4) {
        out.pixels.assign(mip.data.begin(), mip.data.begin() + static_cast<long>(count * 4));
        if (f == "B8G8R8A8")
            for (size_t i = 0; i < count; ++i) std::swap(out.pixels[i * 4], out.pixels[i * 4 + 2]);
        return true;
    }
    if (f == "G8" && mip.data.size() >= count) {
        out.pixels.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            std::memset(&out.pixels[i * 4], mip.data[i], 3);
            out.pixels[i * 4 + 3] = 255;
        }
        return true;
    }
    return false;
}

// 2x2 box filter; odd trailing rows/columns are dropped.
Rgba halve(const Rgba& in) {
    Rgba out;
    out.width = in.width > 1 ? in.width / 2 : 1;
    out.height = in.height > 1 ? in.height / 2 : 1;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            for (int c = 0; c < 4; ++c) {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = std::min(x * 2 + dx, in.width - 1);
                        int sy = std::min(y * 2 + dy, in.height - 1);
                        sum += in.pixels[(static_cast<size_t>(sy) * in.width + sx) * 4 + c];
                    }
                out.pixels[(static_cast<size_t>(y) * out.width + x) * 4 + c] =
                    static_cast<uint8_t>(sum / 4);
            }
        }
    }
    return out;
}

void write_tga(const Rgba& img, const std::string& path) {
    uint8_t header[18] = {};
    header[2] = 2;  // uncompressed true-colour
    header[12] = static_cast<uint8_t>(img.width);
    header[13] = static_cast<uint8_t>(img.width >> 8);
    header[14] = static_cast<uint8_t>(img.height);
    header[15] = static_cast<uint8_t>(img.height >> 8);
    header[16] = 32;
    header[17] = 0x28;  // 8 alpha bits, top-left origin
    std::vector<uint8_t> bgra(img.pixels);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
}

}  // namespace

bool write_texture_tga(const TextureData& tex, const std::string& path, int max_size) {
    const TextureMip* chosen = nullptr;
    for (const TextureMip& mip : tex.mips) {  // largest first
        if (mip.data.empty()) continue;
        chosen = &mip;
        if (mip.width <= max_size && mip.height <= max_size) break;
    }
    if (!chosen) return false;
    Rgba img;
    if (!decode_mip(*chosen, tex.pixel_format, img)) return false;
    while (img.width > max_size || img.height > max_size) img = halve(img);
    write_tga(img, path);
    return true;
}

}  // namespace bl4
