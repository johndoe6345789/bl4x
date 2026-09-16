#pragma once
// Read-only file with positional, thread-safe reads.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bl4 {

class File {
public:
    File() = default;
    explicit File(const std::string& path);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;

    uint64_t size() const { return size_; }
    const std::string& path() const { return path_; }
    // Reads exactly n bytes at offset; throws on short read.
    void read_at(uint64_t offset, uint8_t* dst, size_t n) const;

    static std::vector<uint8_t> read_all(const std::string& path);

private:
    void* handle_ = nullptr;
    uint64_t size_ = 0;
    std::string path_;
};

}  // namespace bl4
