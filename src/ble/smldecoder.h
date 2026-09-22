#pragma once

#include "sbemcontainer.h"
#include "sbemdescriptors.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A generic, descriptor-driven decoder for SBEM payloads - the way the
// official app does it (BSML::DecoderBase, decompiled from libmds.so): look
// the field up in the watch's own table rather than hard-coding a byte
// offset. Qt-free, same discipline as the rest of src/ble.
//
// Every chunk id is a descriptor id; a group descriptor's payload is its
// children's values concatenated in order with no per-child headers. This
// walks that structure and hands each decoded reading to a callback, so a
// consumer picks the fields it wants without anyone writing another offset
// table. See docs/sbem-chunk-map.md for what's available.
//
// Values are reported in the schema's own canonical units, i.e. after its
// <MOD> expression: heart rate and cadence in Hz (not bpm), temperature in
// Kelvin, energy in joules, speed in m/s, distance and altitude in metres.
// Latitude and longitude are the deliberate exception - degrees, not the
// schema's radians, since that is what the rest of this project uses.
//
// Differential fields (<DELTAREF>) are resolved as they go: a delta chunk
// updates its target's running value, and the reading is reported against
// the *target* descriptor, so a consumer asking for Sample.Altitude gets
// every sample whether it arrived absolute or as a delta.
//
// Readings marked nillable that carry the "no reading" value are skipped
// entirely rather than reported as a number.
//
// This does not replace Logbook::decode() or Summary::decode(); those two
// are hand-written, hardware-validated and stay that way. This exists for
// everything they don't cover.
namespace Sml {

struct Reading
{
    uint16_t descriptorId = 0;
    const SbemDescriptors::Descriptor *descriptor = nullptr;
    double value = 0;
    // Which chunk this came from. Consumers that only want values ignore
    // it; the SML JSON writer needs it because the cloud's format is one
    // JSON sample per chunk (see src/cloud/smljson.h).
    size_t chunkIndex = 0;
    // Set for utf8 fields, where `value` is meaningless. The only ones seen
    // so far are Sample.Events.Array.Activity.CustomModeId and the header's
    // free-text fields, but they have to survive the trip to reproduce what
    // the app uploads.
    bool isText = false;
    std::string text;
    // Milliseconds since the epoch, UTC, tracked through the payload's time
    // base (chunk 0x01) and the int16 deltas every group carries. 0 until
    // the first time reference appears.
    int64_t timeMs = 0;
};

using ReadingCallback = std::function<void(const Reading &)>;

// Walks the container and calls back once per decoded reading, in payload
// order. Chunks whose descriptor the table doesn't know, and children that
// would run past the end of their chunk, are skipped rather than guessed at.
void decode(const std::vector<Sbem::Chunk> &chunks, const ReadingCallback &callback);

} // namespace Sml
