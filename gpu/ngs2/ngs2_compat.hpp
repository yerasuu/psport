// SPDX-License-Identifier: GPL-2.0-or-later
// What the Ngs2 sources took from AnyPS5's libc (General.hpp, VabiMacros.hpp), in libbbgpu terms.
// Their unimplemented and invalid cases throw; Exports.cpp turns that into an error code at the
// guest boundary, since an exception must not unwind through the game's frames.
#pragma once
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

// The guest calls with the System V ABI, which is the host's on Linux.
#define APS5_VABI
#define APS5_INVALID_ARG_EX throw std::invalid_argument(std::string(__func__) + ": invalid argument")

extern "C" int runtime_file_translate(const char* guest, char* out, std::size_t size);

inline std::filesystem::path ResolvePath_nid_no_patch(const char* path) {
    char host[1024];
    if (runtime_file_translate(path, host, sizeof(host)) != 0) {
        throw std::runtime_error(std::string("Ngs2: no host path for ") + path);
    }
    return host;
}

inline void NotImplemented_nid_no_patch(const char* function) {
    std::printf("Ngs2: %s is not implemented; returns 0\n", function);
}
