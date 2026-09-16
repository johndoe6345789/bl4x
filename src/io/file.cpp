#include "io/file.hpp"

#include <windows.h>

#include <stdexcept>
#include <utility>

namespace bl4 {

File::File(const std::string& path) : path_(path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open " + path);
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    handle_ = h;
    size_ = static_cast<uint64_t>(sz.QuadPart);
}

File::~File() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

File::File(File&& o) noexcept
    : handle_(std::exchange(o.handle_, nullptr)),
      size_(o.size_),
      path_(std::move(o.path_)) {}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = std::exchange(o.handle_, nullptr);
        size_ = o.size_;
        path_ = std::move(o.path_);
    }
    return *this;
}

void File::read_at(uint64_t offset, uint8_t* dst, size_t n) const {
    while (n) {
        DWORD chunk = n > 0x40000000 ? 0x40000000 : static_cast<DWORD>(n);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), dst, chunk, &got, &ov) ||
            got != chunk)
            throw std::runtime_error("short read from " + path_);
        dst += got;
        offset += got;
        n -= got;
    }
}

std::vector<uint8_t> File::read_all(const std::string& path) {
    File f(path);
    std::vector<uint8_t> out(static_cast<size_t>(f.size()));
    if (!out.empty()) f.read_at(0, out.data(), out.size());
    return out;
}

}  // namespace bl4
