#pragma once

#include <cstdint>
#include <vector>

// Decodes the TYPE=0x05 response to Mds::encodeEntriesFetchTrigger() (the
// /Logbook/Entries shortcut - see mdswirecodec.h) into a list of logbook
// entry ids and modification timestamps. Qt-free (STL only), same
// golden-vector-before-BLE discipline as logbookdecoder.h/sbemcontainer.h.
//
// This is a hand-written decoder for this one response shape, not a
// general implementation of the underlying whiteboard::protocol_v9 wire
// structure format - see docs/logbook-data-format.md's "Third
// decompilation pass" section for the real (decompiled from libmds.so)
// header/alignment/recursive-walk algorithm this was derived from, and
// exactly what's confirmed vs. inferred:
//   - The response body's first 8 bytes are a fixed prefix shared with
//     every other 0x0d/0x05 handle-based exchange this project has
//     captured (0xF0, the 2-byte handle echoed back, 0x01 0x80 0x00,
//     0xc8 0x00) - skipped here without further interpretation.
//   - The next 2 bytes are a protocol_v9 structure header (byte0 =
//     validation byte, not used here; byte1 bits 1-3 select an alignment
//     mask from a small table found in libmds.so's own .rodata,
//     {1,3,7,15,31,63,127} for selector 1-7) - the actual array payload
//     starts after this header, rounded up to that alignment.
//   - What follows (confirmed against one real captured response, byte
//     offsets fixed relative to the payload start, not yet proven to
//     generalize to a different entry count or a different alignment
//     selector): a 2-byte element type tag, a 2-byte 9-bit-masked
//     sub-length, a 2-byte entry count, 6 more bytes not yet decoded
//     (possibly per-element stride/reserved), then that many 24-byte
//     entry records: uint32 id (little-endian), uint32
//     modificationTimestamp (little-endian, both Unix seconds), and 16
//     further bytes left undecoded (a "Size" field - matching a schema
//     field name this project separately recovered - is suspected to be
//     among them but not confirmed).
// Confirmed correct, not just plausible: decoding the one real captured
// response this was built from produces three (id, modificationTimestamp)
// pairs whose id values exactly match three real logbook ids independently
// observed as literal ASCII path segments elsewhere in the very same
// capture (in separate /Logbook/byId/<id>/Data, /Summary and /Descriptors
// requests) - see tests/test_logentriesdecoder.cpp and
// docs/logbook-data-format.md.
//
// What's NOT confirmed: whether the fixed offsets here (entry count
// position, 24-byte stride, record start offset) hold for a different
// number of entries, a bigger entries list that needs pagination, or a
// different alignment selector - only one real shape has been observed.
// A malformed/unexpected shape returns an empty vector rather than
// guessing.
namespace LogEntries {

struct Entry
{
    uint32_t id = 0;
    uint32_t modificationTimestamp = 0;
};

std::vector<Entry> decode(const std::vector<uint8_t> &responseBody);

} // namespace LogEntries
