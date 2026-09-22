#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Decodes a /Logbook/byId/<id>/Data payload once MdsWhiteboardClient has
// reassembled it from the BLE bulk-transfer chunks (see
// docs/logbook-data-format.md for the full pipeline this sits at the end
// of). Two independent steps, both reverse-engineered against real bytes
// captured from Jarno's own Suunto Race and cross-checked against
// libdivecomputer's suunto_nautic driver (a real, working implementation
// for a related Suunto BLE device that turned out to share this exact
// on-wire format):
//
//   1. Heatshrink (LZSS) decompression - window_sz2=7, lookahead_sz2=5,
//      confirmed by both suunto_nautic.c's own constants and by these
//      exact parameters reproducing the literal "SBEM0103" magic at the
//      start of real captured/decompressed data (see
//      tests/test_sbemcontainer.cpp).
//   2. The decompressed bytes are a "SBEM0103"-tagged TLV container:
//      repeating [chunk id: 1 byte][length: 1 byte][value: length bytes],
//      where length == 0xFF means an extended 4-byte little-endian length
//      follows immediately instead of a literal value length.
namespace Sbem {

// Throws std::runtime_error on a Heatshrink decode error (malformed input).
std::vector<uint8_t> heatshrinkDecompress(const std::vector<uint8_t> &compressed);

// Decodes the schema's "local64" timestamp format to ordinary UTC
// milliseconds since the epoch. The low 56 bits are milliseconds since the
// epoch in *local* time; the top byte is the UTC offset in quarter-hours
// (signed, so west of Greenwich works too).
//
// Confirmed on two independent real workouts: the cycling capture's base
// timestamp decodes to 800ms before its first GPS fix and the walking one's
// to exactly its first fix, both with a +3h offset (0x0c = 12 quarter-hours)
// matching Finnish summer time.
int64_t decodeLocal64(uint64_t raw);

struct Chunk {
    // 16-bit, not 8: a chunk id of 0xFF means the real id is the next two
    // bytes, little-endian (confirmed by decompiling libmds.so's
    // BSML::SmlStreamParser::parseChunkHeader - the length field's own
    // 0xFF escape has the same shape). Ids above 254 do occur: the
    // descriptor table itself runs past 300.
    uint16_t id;
    std::vector<uint8_t> value;
};

// Parses the "SBEM0103"-prefixed TLV stream (post-decompression). Returns
// an empty vector if the magic prefix doesn't match. Stops (without
// throwing) at the first malformed/truncated chunk, since a partial parse
// of everything before it is still useful.
std::vector<Chunk> parseContainer(const std::vector<uint8_t> &decompressed);

} // namespace Sbem
