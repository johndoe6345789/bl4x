#pragma once
// One IoStore container: a .utoc table of contents plus its .ucas data
// partitions (FIoStoreTocResource + IoStoreReader in CUE4Parse).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "io/file.hpp"

namespace bl4 {

enum class ChunkType : uint8_t {
    ExportBundleData = 1,
    BulkData = 2,
    OptionalBulkData = 3,
    MemoryMappedBulkData = 4,
    ScriptObjects = 5,
    ContainerHeader = 6,
};

#pragma pack(push, 1)
struct ChunkId {
    uint64_t id = 0;
    uint16_t index_be = 0;  // network byte order, as stored
    uint8_t pad = 0;
    uint8_t type = 0;

    static ChunkId make(uint64_t id, uint16_t index, ChunkType t) {
        ChunkId c;
        c.id = id;
        c.index_be = static_cast<uint16_t>((index >> 8) | (index << 8));
        c.type = static_cast<uint8_t>(t);
        return c;
    }
    uint16_t index() const {
        return static_cast<uint16_t>((index_be >> 8) | (index_be << 8));
    }
    bool operator==(const ChunkId& o) const {
        return id == o.id && index_be == o.index_be && type == o.type;
    }
};
#pragma pack(pop)

struct FileEntry {
    std::string path;  // mount-relative, e.g. OakGame/Content/X.umap
    uint32_t chunk = 0;
};

class IoStore {
public:
    explicit IoStore(const std::string& utoc_path);

    const std::string& name() const { return name_; }
    uint64_t container_id() const { return container_id_; }
    int order() const { return order_; }
    size_t chunk_count() const { return chunk_ids_.size(); }
    const ChunkId& chunk_id(uint32_t i) const { return chunk_ids_[i]; }
    uint64_t chunk_size(uint32_t i) const { return lengths_[i]; }

    // Index of the chunk, or -1.
    int64_t find(const ChunkId& id) const;

    // Whole chunk, or [offset, offset + size) of it.
    std::vector<uint8_t> read(uint32_t chunk) const;
    std::vector<uint8_t> read(uint32_t chunk, uint64_t offset,
                              uint64_t size) const;

    // Files from the directory index (empty if the container has none).
    const std::vector<FileEntry>& files() const { return files_; }
    void release_files() { std::vector<FileEntry>().swap(files_); }

private:
    struct Block {
        uint64_t offset;
        uint32_t comp_size;
        uint32_t raw_size;
        uint8_t method;
    };
    void parse_toc(const std::vector<uint8_t>& toc);
    void parse_directory(const uint8_t* data, size_t size);

    std::string name_;
    int order_ = 0;
    uint64_t container_id_ = 0;
    uint32_t block_size_ = 0;
    uint64_t partition_size_ = ~0ull;
    std::vector<ChunkId> chunk_ids_;
    std::vector<uint64_t> offsets_;
    std::vector<uint64_t> lengths_;
    std::vector<int32_t> seeds_;
    std::vector<int32_t> no_hash_;
    std::vector<Block> blocks_;
    std::vector<std::string> methods_;  // [0] = none
    std::vector<File> partitions_;
    std::vector<FileEntry> files_;
};

}  // namespace bl4
