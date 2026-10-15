#pragma once

// Copied from harbour-edgesync (the same author, MIT), src/fit/ and
// src/gfdi/crc.*, where it was written to read a Garmin Edge's own files.
// FIT is FIT: nothing in here is Edge-specific, which is why the copy is
// verbatim apart from the include paths. **Both copies exist** - a fix to
// one belongs in the other, and the two apps are in the same workspace.
#include <cstddef>
#include <cstdint>

namespace Gfdi {

// The CRC-16 of GFDI frames and of file-transfer chunks: the nibble-table form
// of CRC-16/ARC (polynomial 0xA001, reflected). `seed` is the running value, so
// a file's checksum is the fold of its chunks; a frame starts from 0.
uint16_t crc16(const uint8_t *data, size_t length, uint16_t seed = 0);

} // namespace Gfdi
