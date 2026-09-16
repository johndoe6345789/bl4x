#include "pkg/container_header.hpp"

#include "core/reader.hpp"

namespace bl4 {
namespace {
constexpr uint32_t kSignature = 0x496f436e;
constexpr int32_t kOptionalSegmentPackages = 2;
constexpr int32_t kNoExportInfo = 3;
}  // namespace

ContainerHeader::ContainerHeader(std::span<const uint8_t> data) {
    Reader r(data);
    uint32_t sig = r.read<uint32_t>();
    if (sig != kSignature) r.fail("bad container header signature");
    int32_t version = r.read<int32_t>();
    r.skip(8);  // container id
    if (version < kOptionalSegmentPackages) r.skip(4);  // legacy packageCount

    auto package_ids = r.read_tarray<uint64_t>();
    int32_t entries_size = r.read<int32_t>();
    size_t entries_end = r.pos() + static_cast<size_t>(entries_size);
    by_id_.reserve(package_ids.size() * 2);
    for (uint64_t pid : package_ids) {
        size_t entry_start = r.pos();
        StoreEntry e;
        if (version < kNoExportInfo) r.skip(8);  // exportCount + exportBundleCount
        int32_t num = r.read<int32_t>();
        int32_t rel_offset = r.read<int32_t>();
        if (num > 0) {
            size_t continue_pos = r.pos();
            r.seek(entry_start + static_cast<size_t>(rel_offset));
            e.imported_packages = r.read_array<uint64_t>(static_cast<size_t>(num));
            r.seek(continue_pos);
        }
        r.skip(8);  // reserved
        by_id_[pid] = std::move(e);
    }
    r.seek(entries_end);
}

}  // namespace bl4
