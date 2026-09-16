#include "io/oodle.hpp"

#include <windows.h>

#include <stdexcept>

namespace bl4::oodle {
namespace {

using DecompressFn = intptr_t(__stdcall*)(
    const void* comp, intptr_t comp_size, void* raw, intptr_t raw_size,
    int fuzz_safe, int check_crc, int verbosity, void* dec_buf_base,
    intptr_t dec_buf_size, void* fp_callback, void* callback_user_data,
    void* decoder_memory, intptr_t decoder_memory_size, int thread_phase);

DecompressFn g_decompress = nullptr;

}  // namespace

void init(const std::string& dll_path) {
    if (g_decompress) return;
    HMODULE lib = LoadLibraryA(dll_path.c_str());
    if (!lib) throw std::runtime_error("cannot load " + dll_path);
    g_decompress = reinterpret_cast<DecompressFn>(
        GetProcAddress(lib, "OodleLZ_Decompress"));
    if (!g_decompress)
        throw std::runtime_error("OodleLZ_Decompress missing in " + dll_path);
}

void decompress(const uint8_t* comp, size_t comp_size, uint8_t* raw,
                size_t raw_size) {
    if (!g_decompress) throw std::runtime_error("oodle not initialised");
    intptr_t n = g_decompress(comp, static_cast<intptr_t>(comp_size), raw,
                              static_cast<intptr_t>(raw_size), 1, 0, 0,
                              nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    if (n != static_cast<intptr_t>(raw_size))
        throw std::runtime_error("oodle decompress failed (" +
                                 std::to_string(n) + ")");
}

}  // namespace bl4::oodle
