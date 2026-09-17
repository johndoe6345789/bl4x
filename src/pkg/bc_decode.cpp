#include "pkg/bc_decode.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace bl4 {
namespace {

bool is_fmt(const std::string& name, const char* s) {
    return name == s || name == (std::string("PF_") + s);
}

uint8_t expand5(uint8_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
uint8_t expand6(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

// Shared 6-bytes-of-3-bit-indices layout used by BC3's alpha block, BC4 and
// each BC5 channel: 2 endpoint bytes, then 16 x 3-bit indices (48 bits).
struct AlphaLikeBlock {
    uint8_t endpoint[2];
    uint8_t index[16];
};

AlphaLikeBlock read_alpha_like(const uint8_t* p) {
    AlphaLikeBlock b{};
    b.endpoint[0] = p[0];
    b.endpoint[1] = p[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(p[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) b.index[i] = static_cast<uint8_t>((bits >> (3 * i)) & 7);
    return b;
}

std::array<uint8_t, 8> alpha_like_palette(uint8_t v0, uint8_t v1) {
    std::array<uint8_t, 8> pal{};
    pal[0] = v0;
    pal[1] = v1;
    if (v0 > v1) {
        for (int i = 1; i < 7; ++i)
            pal[static_cast<size_t>(i) + 1] =
                static_cast<uint8_t>(((7 - i) * v0 + i * v1) / 7);
    } else {
        for (int i = 1; i < 5; ++i)
            pal[static_cast<size_t>(i) + 1] =
                static_cast<uint8_t>(((5 - i) * v0 + i * v1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    return pal;
}

// BC1's 4-entry RGB palette. `force_opaque` selects the "always 4 distinct
// colors" interpolation BC2/BC3 use for their color block, regardless of the
// numeric c0-vs-c1 comparison BC1 itself uses to pick 4-color-vs-punchthrough.
struct Rgb { uint8_t r, g, b; };
std::array<Rgb, 4> bc1_palette(uint16_t c0, uint16_t c1, bool force_opaque, bool& punchthrough) {
    Rgb e0{expand5(static_cast<uint8_t>((c0 >> 11) & 0x1F)),
           expand6(static_cast<uint8_t>((c0 >> 5) & 0x3F)),
           expand5(static_cast<uint8_t>(c0 & 0x1F))};
    Rgb e1{expand5(static_cast<uint8_t>((c1 >> 11) & 0x1F)),
           expand6(static_cast<uint8_t>((c1 >> 5) & 0x3F)),
           expand5(static_cast<uint8_t>(c1 & 0x1F))};
    std::array<Rgb, 4> pal{e0, e1, Rgb{}, Rgb{}};
    punchthrough = false;
    if (force_opaque || c0 > c1) {
        pal[2] = Rgb{static_cast<uint8_t>((2 * e0.r + e1.r) / 3),
                      static_cast<uint8_t>((2 * e0.g + e1.g) / 3),
                      static_cast<uint8_t>((2 * e0.b + e1.b) / 3)};
        pal[3] = Rgb{static_cast<uint8_t>((e0.r + 2 * e1.r) / 3),
                      static_cast<uint8_t>((e0.g + 2 * e1.g) / 3),
                      static_cast<uint8_t>((e0.b + 2 * e1.b) / 3)};
    } else {
        pal[2] = Rgb{static_cast<uint8_t>((e0.r + e1.r) / 2),
                      static_cast<uint8_t>((e0.g + e1.g) / 2),
                      static_cast<uint8_t>((e0.b + e1.b) / 2)};
        pal[3] = Rgb{0, 0, 0};
        punchthrough = true;
    }
    return pal;
}

void decode_bc1_block(const uint8_t* p, uint8_t out[64]) {
    uint16_t c0 = static_cast<uint16_t>(p[0] | (p[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(p[2] | (p[3] << 8));
    uint32_t idx = static_cast<uint32_t>(p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
    bool punchthrough = false;
    auto pal = bc1_palette(c0, c1, false, punchthrough);
    for (int i = 0; i < 16; ++i) {
        int sel = (idx >> (2 * i)) & 3;
        out[i * 4 + 0] = pal[static_cast<size_t>(sel)].r;
        out[i * 4 + 1] = pal[static_cast<size_t>(sel)].g;
        out[i * 4 + 2] = pal[static_cast<size_t>(sel)].b;
        out[i * 4 + 3] = static_cast<uint8_t>(punchthrough && sel == 3 ? 0 : 255);
    }
}

void decode_bc3_block(const uint8_t* p, uint8_t out[64]) {
    AlphaLikeBlock ablock = read_alpha_like(p);
    auto apal = alpha_like_palette(ablock.endpoint[0], ablock.endpoint[1]);

    const uint8_t* cp = p + 8;
    uint16_t c0 = static_cast<uint16_t>(cp[0] | (cp[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(cp[2] | (cp[3] << 8));
    uint32_t cidx = static_cast<uint32_t>(cp[4] | (cp[5] << 8) | (cp[6] << 16) | (cp[7] << 24));
    bool punchthrough = false;
    auto pal = bc1_palette(c0, c1, true, punchthrough);

    for (int i = 0; i < 16; ++i) {
        int csel = (cidx >> (2 * i)) & 3;
        out[i * 4 + 0] = pal[static_cast<size_t>(csel)].r;
        out[i * 4 + 1] = pal[static_cast<size_t>(csel)].g;
        out[i * 4 + 2] = pal[static_cast<size_t>(csel)].b;
        out[i * 4 + 3] = apal[ablock.index[i]];
    }
}

void decode_bc4_block(const uint8_t* p, uint8_t out[64]) {
    AlphaLikeBlock b = read_alpha_like(p);
    auto pal = alpha_like_palette(b.endpoint[0], b.endpoint[1]);
    for (int i = 0; i < 16; ++i) {
        uint8_t v = pal[b.index[i]];
        out[i * 4 + 0] = v;
        out[i * 4 + 1] = v;
        out[i * 4 + 2] = v;
        out[i * 4 + 3] = 255;
    }
}

void decode_bc5_block(const uint8_t* p, uint8_t out[64]) {
    AlphaLikeBlock rb = read_alpha_like(p);
    AlphaLikeBlock gb = read_alpha_like(p + 8);
    auto rpal = alpha_like_palette(rb.endpoint[0], rb.endpoint[1]);
    auto gpal = alpha_like_palette(gb.endpoint[0], gb.endpoint[1]);
    for (int i = 0; i < 16; ++i) {
        out[i * 4 + 0] = rpal[rb.index[i]];
        out[i * 4 + 1] = gpal[gb.index[i]];
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 255;
    }
}

}  // namespace

bool bc_format_supported(const std::string& format) {
    return is_fmt(format, "DXT1") || is_fmt(format, "DXT5") ||
           is_fmt(format, "BC4") || is_fmt(format, "BC5");
}

size_t bc_block_bytes(const std::string& format) {
    if (is_fmt(format, "DXT1") || is_fmt(format, "BC4")) return 8;
    return 16;
}

std::vector<uint8_t> decode_bc_image(const uint8_t* data, size_t data_size,
                                      int width, int height,
                                      const std::string& format) {
    if (!bc_format_supported(format))
        throw std::runtime_error("unsupported block-compressed VT layer format: " + format);
    size_t block_bytes = bc_block_bytes(format);
    int blocks_w = (width + 3) / 4;
    int blocks_h = (height + 3) / 4;
    if (static_cast<size_t>(blocks_w) * static_cast<size_t>(blocks_h) * block_bytes > data_size)
        throw std::runtime_error("truncated block-compressed data for " + format);

    std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    uint8_t block[64];
    for (int by = 0; by < blocks_h; ++by) {
        for (int bx = 0; bx < blocks_w; ++bx) {
            const uint8_t* src = data + (static_cast<size_t>(by) * blocks_w + bx) * block_bytes;
            if (is_fmt(format, "DXT1")) decode_bc1_block(src, block);
            else if (is_fmt(format, "DXT5")) decode_bc3_block(src, block);
            else if (is_fmt(format, "BC4")) decode_bc4_block(src, block);
            else decode_bc5_block(src, block);

            for (int ty = 0; ty < 4; ++ty) {
                int py = by * 4 + ty;
                if (py >= height) break;
                for (int tx = 0; tx < 4; ++tx) {
                    int px = bx * 4 + tx;
                    if (px >= width) break;
                    std::memcpy(&out[(static_cast<size_t>(py) * width + px) * 4],
                               &block[(ty * 4 + tx) * 4], 4);
                }
            }
        }
    }
    return out;
}

}  // namespace bl4
