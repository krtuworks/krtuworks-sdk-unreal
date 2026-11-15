// Gzip (RFC 1952) around deflate (RFC 1951) with only the standard library: event batches go to
// POST /v1/events with Content-Encoding: gzip, and the engine-independent core cannot link zlib.
#pragma once

#include "KrtuworksCore/Export.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace krtu
{
/// CRC-32 (IEEE 802.3, the one gzip uses) of `data`.
std::uint32_t crc32(std::string_view data) noexcept;

/// `data` as a gzip stream (RFC 1952: header without a name or time, deflate body, CRC-32 and size).
/// Every block is the smallest of stored, fixed Huffman and dynamic Huffman, so incompressible input
/// grows by about 5 bytes per 64 KiB plus the 18 bytes of header and trailer. No shared state: safe to
/// call from any thread.
KRTU_API std::string gzip(std::string_view data);
}  // namespace krtu
