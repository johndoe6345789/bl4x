#include "io/iostore.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "core/reader.hpp"
#include "io/oodle.hpp"

namespace bl4 {
namespace {

constexpr uint8_t kMagic[16] = {'-', '=', '=', '-', '-', '=', '=', '-',
                                '-', '=', '=', '-', '-', '=', '=', '-'};
constexpr uint8_t kFlagEncrypted = 1 << 1;
constexpr uint8_t kFlagSigned = 1 << 2;
constexpr uint8_t kFlagIndexed = 1 << 3;
constexpr uint8_t kVersionDirectoryIndex = 2;
constexpr uint8_t kVersionPartitionSize = 3;
constexpr uint8_t kVersionPerfectHash = 4;
constexpr uint8_t kVersionPerfectHashWithOverflow = 5;

uint64_t be40(const uint8_t* b) {
    return (uint64_t(b[0]) << 32) | (uint64_t(b[1]) << 24) |
           (uint64_t(b[2]) << 16) | (uint64_t(b[3]) << 8) | uint64_t(b[4]);
}

uint64_t hash_with_seed(const ChunkId& id, int32_t seed) {
    uint64_t h = seed ? static_cast<uint64_t>(static_cast<int64_t>(seed))
                      : 0xcbf29ce484222325ull;
    const auto* p = reinterpret_cast<const uint8_t*>(&id);
    for (size_t i = 0; i < sizeof(ChunkId); ++i)
        h = (h * 0x00000100000001B3ull) ^ p[i];
    return h;
}

// Patch containers win: "<name>_<N>_P.utoc" gets priority N + 1.
int order_from_name(const std::string& stem) {
    if (stem.size() < 2 || stem.compare(stem.size() - 2, 2, "_P") != 0)
        return 0;
    int version = 1;
    size_t end = stem.size() - 2;
    size_t start = stem.rfind('_', end - 1);
    if (start != std::string::npos) {
        std::string num = stem.substr(start + 1, end - start - 1);
        if (!num.empty() &&
            std::all_of(num.begin(), num.end(), ::isdigit)) {
            int v = std::stoi(num);
            if (v >= 1) version = v + 1;
        }
    }
    return 100 * version;
}

thread_local std::vector<uint8_t> t_comp;
thread_local std::vector<uint8_t> t_raw;

}  // namespace

IoStore::IoStore(const std::string& utoc_path) {
    std::filesystem::path p(utoc_path);
    name_ = p.filename().string();
    order_ = order_from_name(p.stem().string());
    parse_toc(File::read_all(utoc_path));
    auto base = (p.parent_path() / p.stem()).string();
    size_t parts = partitions_.capacity();
    for (size_t i = 0; i < parts; ++i) {
        auto path = i ? base + "_s" + std::to_string(i) + ".ucas"
                      : base + ".ucas";
        partitions_.emplace_back(path);
    }
}

void IoStore::parse_toc(const std::vector<uint8_t>& toc) {
    Reader r(toc);
    auto magic = r.bytes(16);
    if (std::memcmp(magic.data(), kMagic, 16) != 0) r.fail("bad utoc magic");
    uint8_t version = r.read<uint8_t>();
    r.skip(3);
    uint32_t header_size = r.read<uint32_t>();
    uint32_t entry_count = r.read<uint32_t>();
    uint32_t block_count = r.read<uint32_t>();
    uint32_t block_entry_size = r.read<uint32_t>();
    uint32_t method_count = r.read<uint32_t>();
    uint32_t method_len = r.read<uint32_t>();
    block_size_ = r.read<uint32_t>();
    uint32_t dir_size = r.read<uint32_t>();
    uint32_t partition_count = r.read<uint32_t>();
    container_id_ = r.read<uint64_t>();
    r.skip(16);  // encryption key guid
    uint8_t flags = r.read<uint8_t>();
    r.skip(3);   // encryption method, reserved
    uint32_t seed_count = r.read<uint32_t>();
    partition_size_ = r.read<uint64_t>();
    uint32_t no_hash_count = r.read<uint32_t>();
    if (flags & kFlagEncrypted) throw std::runtime_error(name_ + " is encrypted");
    if (block_entry_size != 12) r.fail("unexpected block entry size");
    if (version < kVersionPartitionSize) {
        partition_count = 1;
        partition_size_ = ~0ull;
    }
    r.seek(header_size);

    chunk_ids_ = r.read_array<ChunkId>(entry_count);
    auto ol = r.bytes(size_t(entry_count) * 10);
    offsets_.resize(entry_count);
    lengths_.resize(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        offsets_[i] = be40(ol.data() + i * 10);
        lengths_[i] = be40(ol.data() + i * 10 + 5);
    }
    if (version < kVersionPerfectHash) seed_count = 0;
    if (version < kVersionPerfectHashWithOverflow) no_hash_count = 0;
    seeds_ = r.read_array<int32_t>(seed_count);
    no_hash_ = r.read_array<int32_t>(no_hash_count);

    blocks_.resize(block_count);
    for (auto& b : blocks_) {
        uint64_t oc = r.read<uint64_t>();
        uint32_t um = r.read<uint32_t>();
        b.offset = oc & ((1ull << 40) - 1);
        b.comp_size = static_cast<uint32_t>(oc >> 40);
        b.raw_size = um & 0xFFFFFF;
        b.method = static_cast<uint8_t>(um >> 24);
    }
    methods_.push_back("");
    auto names = r.bytes(size_t(method_count) * method_len);
    for (uint32_t i = 0; i < method_count; ++i) {
        const char* s = reinterpret_cast<const char*>(names.data()) +
                        i * method_len;
        methods_.emplace_back(s, strnlen(s, method_len));
    }
    if (flags & kFlagSigned) {
        int32_t hash_size = r.read<int32_t>();
        r.skip(size_t(hash_size) * 2 + 20ull * block_count);
    }
    if (version >= kVersionDirectoryIndex && (flags & kFlagIndexed) &&
        dir_size > 0)
        parse_directory(r.bytes(dir_size).data(), dir_size);
    partitions_.reserve(partition_count ? partition_count : 1);
}

void IoStore::parse_directory(const uint8_t* data, size_t size) {
    Reader r({data, size});
    std::string mount = r.read_fstring();
    while (mount.rfind("../", 0) == 0) mount.erase(0, 3);
    if (!mount.empty() && mount[0] == '/') mount.erase(0, 1);
    struct Dir { uint32_t name, child, sibling, file; };
    struct Fil { uint32_t name, next, chunk; };
    auto dirs = r.read_tarray<Dir>();
    auto fils = r.read_tarray<Fil>();
    int32_t nstr = r.read_count();
    std::vector<std::string> strings(nstr);
    for (auto& s : strings) s = r.read_fstring();

    constexpr uint32_t none = 0xFFFFFFFF;
    struct Todo { uint32_t dir; std::string prefix; };
    std::vector<Todo> stack{{0, mount}};
    files_.reserve(fils.size());
    while (!stack.empty()) {
        Todo t = std::move(stack.back());
        stack.pop_back();
        const Dir& d = dirs.at(t.dir);
        std::string here = t.prefix;
        if (d.name != none && !strings.at(d.name).empty())
            here += strings[d.name] + "/";
        if (d.sibling != none) stack.push_back({d.sibling, t.prefix});
        if (d.child != none) stack.push_back({d.child, here});
        for (uint32_t f = d.file; f != none; f = fils.at(f).next)
            files_.push_back({here + strings.at(fils[f].name), fils[f].chunk});
    }
}

int64_t IoStore::find(const ChunkId& id) const {
    uint32_t n = static_cast<uint32_t>(chunk_ids_.size());
    if (n == 0) return -1;
    if (!seeds_.empty()) {
        uint32_t si = static_cast<uint32_t>(hash_with_seed(id, 0) % seeds_.size());
        int32_t seed = seeds_[si];
        if (seed == 0) return -1;
        uint32_t slot;
        if (seed < 0) {
            uint32_t as_index = static_cast<uint32_t>(-seed - 1);
            if (as_index >= n) goto linear;
            slot = as_index;
        } else {
            slot = static_cast<uint32_t>(hash_with_seed(id, seed) % n);
        }
        return chunk_ids_[slot] == id ? int64_t(slot) : -1;
    }
linear:
    if (!no_hash_.empty()) {
        for (int32_t i : no_hash_)
            if (chunk_ids_[i] == id) return i;
        return -1;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (chunk_ids_[i] == id) return i;
    return -1;
}

std::vector<uint8_t> IoStore::read(uint32_t chunk) const {
    return read(chunk, 0, lengths_.at(chunk));
}

std::vector<uint8_t> IoStore::read(uint32_t chunk, uint64_t offset,
                                   uint64_t size) const {
    if (offset + size > lengths_.at(chunk))
        throw std::runtime_error("chunk range out of bounds in " + name_);
    std::vector<uint8_t> out(static_cast<size_t>(size));
    if (!size) return out;
    uint64_t start = offsets_[chunk] + offset;
    uint64_t first = start / block_size_;
    uint64_t last = (start + size - 1) / block_size_;
    uint64_t in_block = start % block_size_;
    size_t written = 0;
    for (uint64_t bi = first; bi <= last; ++bi) {
        const Block& b = blocks_.at(bi);
        uint64_t part = b.offset / partition_size_;
        uint64_t part_off = b.offset % partition_size_;
        uint32_t raw_len = (b.comp_size + 15) & ~15u;  // AES-aligned
        if (raw_len > partitions_.at(part).size() - part_off)
            raw_len = b.comp_size;
        if (t_comp.size() < raw_len) t_comp.resize(raw_len);
        partitions_[part].read_at(part_off, t_comp.data(), raw_len);
        const uint8_t* src = t_comp.data();
        if (b.method != 0) {
            if (methods_.at(b.method) != "Oodle")
                throw std::runtime_error("unsupported compression " +
                                         methods_[b.method]);
            if (t_raw.size() < b.raw_size) t_raw.resize(b.raw_size);
            oodle::decompress(t_comp.data(), b.comp_size, t_raw.data(),
                              b.raw_size);
            src = t_raw.data();
        }
        size_t take = static_cast<size_t>(
            std::min<uint64_t>(block_size_ - in_block, size - written));
        std::memcpy(out.data() + written, src + in_block, take);
        written += take;
        in_block = 0;
    }
    return out;
}

}  // namespace bl4
