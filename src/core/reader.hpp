#pragma once
// Little-endian cursor over a byte buffer, the C++ side of CUE4Parse's
// FArchive. Every read is bounds checked and throws ParseError.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace bl4 {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Reader {
public:
    Reader() = default;
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    size_t pos() const { return pos_; }
    size_t size() const { return data_.size(); }
    size_t remaining() const { return data_.size() - pos_; }
    bool eof() const { return pos_ >= data_.size(); }
    void seek(size_t p) {
        if (p > data_.size()) fail("seek past end");
        pos_ = p;
    }
    void skip(size_t n) { seek(pos_ + n); }
    void align(size_t a) { seek((pos_ + a - 1) / a * a); }
    std::span<const uint8_t> data() const { return data_; }

    template <class T>
        requires std::is_trivially_copyable_v<T>
    T read() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    template <class T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> read_array(size_t count) {
        if (count > remaining() / (sizeof(T) ? sizeof(T) : 1) + 1)
            fail("array too large");
        need(count * sizeof(T));
        std::vector<T> out(count);
        if (count) std::memcpy(out.data(), data_.data() + pos_, count * sizeof(T));
        pos_ += count * sizeof(T);
        return out;
    }

    // TArray<T> of trivially copyable T: int32 count then elements.
    template <class T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> read_tarray() {
        int32_t n = read<int32_t>();
        if (n < 0) fail("negative array count");
        return read_array<T>(static_cast<size_t>(n));
    }

    std::span<const uint8_t> bytes(size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    // UE bool: int32 that must be 0 or 1.
    bool read_bool() {
        int32_t v = read<int32_t>();
        if (v != 0 && v != 1) fail("bad bool " + std::to_string(v));
        return v == 1;
    }

    // FString: int32 length incl. terminator; negative means UTF-16.
    std::string read_fstring();

    int32_t read_count() {
        int32_t n = read<int32_t>();
        if (n < 0) fail("negative count");
        return n;
    }

    [[noreturn]] void fail(const std::string& what) const {
        throw ParseError(what + " at " + std::to_string(pos_) + "/" +
                         std::to_string(data_.size()));
    }

private:
    void need(size_t n) const {
        if (n > data_.size() - pos_) fail("read past end (" + std::to_string(n) + ")");
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

std::string utf16_to_utf8(const char16_t* s, size_t n);

}  // namespace bl4
