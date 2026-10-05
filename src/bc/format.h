// The .embc binary file format. See docs/BYTECODE.md for the layout.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "bc/module.h"

namespace ember::bc {

constexpr char kMagic[4] = {'E', 'M', 'B', 'C'};
constexpr uint16_t kVersionMajor = 1;
constexpr uint16_t kVersionMinor = 1;  // 1.1: superinstructions
constexpr uint32_t kFlagDebugInfo = 1u << 0;
constexpr size_t kHeaderSize = 4 + 2 + 2 + 4 + 4 + 4;  // magic, major, minor, flags, payload size, crc32

// Thrown by decode() for any malformed input. Never thrown for valid files.
struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t crc32(const uint8_t* data, size_t n);

std::vector<uint8_t> encode(const Module& m, bool include_debug_info = true);

// Decode with full bounds checking. The result is structurally well-formed
// (all counts and indices within the file) but must still be verified with
// verify() before execution. max_size bounds the input size.
Module decode(const std::vector<uint8_t>& bytes, size_t max_size = 64u * 1024 * 1024);

}  // namespace ember::bc
