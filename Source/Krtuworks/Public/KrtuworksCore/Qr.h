#pragma once

#include "KrtuworksCore/Export.h"

#include <string>
#include <vector>

namespace krtu
{
/// A QR code (ISO/IEC 18004) for a console screen's sign-in address: byte mode, error correction level M, versions
/// 1 to 10 (up to 213 bytes). `modules[row][column]` is true for a dark module; the quiet zone (four light modules
/// on every side) is the drawer's. `mask` forces one of the eight masks (0 to 7); -1 is the standard's choice.
/// Throws std::length_error when `text` does not fit version 10, std::invalid_argument for a mask outside -1..7.
KRTU_API std::vector<std::vector<bool>> qrModules(const std::string& text, int mask = -1);
}  // namespace krtu
