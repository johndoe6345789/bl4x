#pragma once
// Bit/byte-level primitives Nanite cluster decode is built from: UE's
// GetBits/GetBitsAsSigned/zig-zag conventions, the unaligned-dword read
// used by the triangle index stream, the three-byte-plane (Low/Mid/High)
// delta reader used by vertex attributes, the custom 20-bit UV float
// encoding, precision-scale table, and octahedral normal / tangent-angle
// unpacking. Every formula here is transcribed from CUE4Parse's
// NaniteUtils.cs and FCluster.cs -- see D:\BL4Export\src\cpp\README.md
// for the decode-order overview; this file only holds the leaf math.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace bl4::nanite {

// value>>offset, masked to num_bits, with the shift amount masked to 5
// bits the way C#'s `>>`/`<<` do on a 32-bit operand -- some call sites
// rely on that wraparound (an offset computed as `x - 1` where x can be
// 0), so this must NOT be "safe" C++ shifting.
inline uint32_t get_bits(uint32_t value, uint32_t num_bits, uint32_t offset) {
    uint32_t shift = offset & 31u;
    uint64_t mask = num_bits >= 32 ? 0xFFFFFFFFull : ((1ull << num_bits) - 1);
    return static_cast<uint32_t>((value >> shift) & mask);
}

// Sign-extends the low num_bits of value>>offset (num_bits in [1,32]).
inline int32_t get_bits_as_signed(uint32_t value, uint32_t num_bits, uint32_t offset) {
    uint32_t x = get_bits(value, num_bits, offset);
    uint32_t shift = 32u - num_bits;
    return static_cast<int32_t>(x << shift) >> shift;
}

inline int32_t decode_zigzag(uint32_t u) {
    return static_cast<int32_t>(u >> 1) ^ -static_cast<int32_t>(u & 1);
}

inline uint32_t popcount32(uint32_t x) { return static_cast<uint32_t>(std::popcount(x)); }

// v << n with n masked to 5 bits, matching C#'s auto-masking shift
// semantics on 32-bit operands (some formulas rely on shifting by a
// uint that has wrapped to ~0u meaning "shift by 31").
inline uint32_t shl32(uint32_t v, uint32_t n) { return v << (n & 31u); }

// 31 - count-leading-zeros, or ~0u if x is 0 (matches FirstBitHigh).
inline uint32_t first_bit_high(uint32_t x) {
    if (x == 0) return 0xFFFFFFFFu;
    return 31u - static_cast<uint32_t>(std::countl_zero(x));
}

inline uint32_t byte_at(std::span<const uint8_t> data, int64_t addr) {
    return (addr >= 0 && addr < static_cast<int64_t>(data.size()))
        ? data[static_cast<size_t>(addr)] : 0u;
}

inline uint32_t read_u32_le_at(std::span<const uint8_t> data, int64_t addr) {
    return byte_at(data, addr) | (byte_at(data, addr + 1) << 8) |
           (byte_at(data, addr + 2) << 16) | (byte_at(data, addr + 3) << 24);
}

inline uint32_t bit_align_u32(uint32_t high, uint32_t low, uint32_t shift) {
    uint32_t s = shift & 31u;
    uint32_t result = low >> s;
    if (s > 0) result |= high << (32u - s);
    return result;
}

// Reads a 32-bit window starting at bit `bit_offset` (may be negative)
// from byte `base_addr_bytes` of `data`. `bit_offset >> 3`/`& 7` rely on
// C++'s two's-complement, arithmetic-shift signed integer behavior to
// match C#'s `long` semantics exactly for negative offsets.
inline uint32_t read_unaligned_dword(std::span<const uint8_t> data,
                                     int64_t base_addr_bytes, int64_t bit_offset) {
    int64_t byte_address = base_addr_bytes + (bit_offset >> 3);
    int64_t aligned = byte_address & ~int64_t{3};
    uint32_t local_bits = static_cast<uint32_t>(((byte_address - aligned) << 3) | (bit_offset & 7));
    uint32_t low = read_u32_le_at(data, aligned);
    uint32_t high = read_u32_le_at(data, aligned + 4);
    return bit_align_u32(high, low, local_bits);
}

// The three parallel byte-plane bases (Low/Mid/High) for one attribute's
// per-vertex delta stream; advances as numValues*numNonRefVertices once
// the attribute has been fully read (see lmh_advance).
struct LmhPlanes { int64_t low = 0, mid = 0, high = 0; };

// Reads and delta-decodes num_values components for vertex_index. Adds
// onto prev_value (which must retain the UNMASKED running sum -- callers
// mask the returned `out` values themselves, per CUE4Parse).
inline void lmh_read(std::span<const uint8_t> page, LmhPlanes base, int bytes_per_value,
                     int num_values, uint32_t vertex_index, int32_t* prev_value,
                     int32_t* out) {
    for (int c = 0; c < num_values; ++c) {
        int64_t off = static_cast<int64_t>(vertex_index) * num_values + c;
        uint32_t packed = 0;
        if (bytes_per_value >= 3) packed |= byte_at(page, base.high + off) << 16;
        if (bytes_per_value >= 2) packed |= byte_at(page, base.mid + off) << 8;
        if (bytes_per_value >= 1) packed |= byte_at(page, base.low + off);
        int32_t value = decode_zigzag(packed) + prev_value[c];
        prev_value[c] = value;
        out[c] = value;
    }
}

inline void lmh_advance(LmhPlanes& base, int bytes_per_value, int64_t n) {
    if (bytes_per_value >= 1) base.low += n;
    if (bytes_per_value >= 2) base.mid += n;
    if (bytes_per_value >= 3) base.high += n;
}

// PrecisionScales[p] == 2^-p, built by exponent subtraction on 1.0f's
// bit pattern (exact, no rounding) -- matches NaniteUtils.cs exactly.
inline float precision_scale(int32_t p) {
    uint32_t bits = 0x3F800000u - (static_cast<uint32_t>(p) << 23);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// UE's custom sign/exponent/14-bit-mantissa UV float encoding.
inline float decode_uv_float(uint32_t encoded, int num_mantissa_bits = 14) {
    uint32_t em_mask = (1u << (5 + num_mantissa_bits)) - 1;
    bool neg = encoded <= em_mask;
    uint32_t em = (neg ? ~encoded : encoded) & em_mask;
    uint32_t bits = 0x3F000000u + (em << (23u - static_cast<uint32_t>(num_mantissa_bits)));
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    result = std::min(result * 2.0f - 1.0f, result);
    return neg ? -result : result;
}

// IEEE-754 binary16 -> float32 (used for the cluster header's half-float
// EdgeLength/LODError fields).
inline float half_to_float(uint16_t h) {
    uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | ((exp + 112u) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

struct Vec3f { float x = 0, y = 0, z = 0; };

inline float length(Vec3f v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// Octahedral-encoded normal unpack (no separate sign bit -- the fold
// carries it). Uses an exact normalize rather than CUE4Parse's
// fast-inverse-sqrt approximation (~0.1% deviation, documented in
// D:\BL4Export\src\cpp\README.md) since there's no reason to reproduce
// a precision bug in new code.
inline Vec3f unpack_octahedral_normal(uint32_t packed, int bits) {
    uint32_t mask = (1u << bits) - 1;
    float inv = 2.0f / static_cast<float>(mask);
    float f0 = static_cast<float>(get_bits(packed, static_cast<uint32_t>(bits), 0)) * inv - 1.0f;
    float f1 = static_cast<float>(get_bits(packed, static_cast<uint32_t>(bits), static_cast<uint32_t>(bits))) * inv - 1.0f;
    Vec3f n{f0, f1, 1.0f - std::fabs(f0) - std::fabs(f1)};
    float t = std::clamp(-n.z, 0.0f, 1.0f);
    n.x += (n.x >= 0.0f) ? -t : t;
    n.y += (n.y >= 0.0f) ? -t : t;
    float len = length(n);
    if (len > 1e-8f) { n.x /= len; n.y /= len; n.z /= len; }
    return n;
}

// Reconstructs a tangent from the cluster normal and a quantized angle
// (FNaniteVertex.UnpackTangentX): builds an orthonormal reference frame
// around the normal, then rotates by `angle_bits / 2^num_bits` turns.
inline Vec3f unpack_tangent_x(Vec3f z, uint32_t angle_bits, int num_bits) {
    bool swap_xz = std::fabs(z.z) > std::fabs(z.x);
    if (swap_xz) std::swap(z.x, z.z);
    Vec3f ref_x{-z.y, z.x, 0.0f};
    Vec3f ref_y{-z.z * z.x, -z.y * z.z, z.x * z.x + z.y * z.y};
    float denom = std::sqrt(ref_x.x * ref_x.x + ref_x.y * ref_x.y);
    float scale = denom > 0.0f ? 1.0f / denom : 0.0f;
    float angle = static_cast<float>(angle_bits) * 6.28318530717958647692f /
                 static_cast<float>(1u << num_bits);
    float c = std::cos(angle) * scale, s = std::sin(angle) * scale;
    Vec3f t{ref_x.x * c + ref_y.x * s, ref_x.y * c + ref_y.y * s, ref_x.z * c + ref_y.z * s};
    if (swap_xz) std::swap(t.x, t.z);
    return t;
}

}  // namespace bl4::nanite
