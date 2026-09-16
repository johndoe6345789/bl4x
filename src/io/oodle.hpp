#pragma once
// Oodle decompression through the oo2core DLL CUE4Parse downloaded
// (D:\BL4Export\oodle.dll). The game links Oodle statically, so there is
// no copy to borrow from the install.
#include <cstddef>
#include <cstdint>
#include <string>

namespace bl4::oodle {

// Loads the DLL; throws std::runtime_error on failure.
void init(const std::string& dll_path);

// Decompresses exactly raw_size bytes; throws on failure.
void decompress(const uint8_t* comp, size_t comp_size, uint8_t* raw,
                size_t raw_size);

}  // namespace bl4::oodle
