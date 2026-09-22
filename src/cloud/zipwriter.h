#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A minimal ZIP writer - just enough to build the `sml.zip` part of a
// workout upload (samples.json + summary.json, see docs/workout-upload.md).
//
// Qt has no public zip writer (QZipWriter is private API and not something
// a harbour- package should reach into), and the alternative of shipping
// minizip for two entries would be a lot of vendored code for a format
// whose writing side is a few dozen lines. Deflate itself comes from zlib,
// which is already everywhere.
//
// Writes the classic 32-bit format: local header per entry, central
// directory, end-of-central-directory. No zip64, no encryption, no
// directories - a two-entry archive of a few hundred kilobytes needs none
// of it. Entries are deflated; a real captured sml.zip compresses
// 8 kB of JSON to 670 bytes, and the upload is over a phone's mobile data.
namespace ZipWriter {

struct Entry
{
    std::string name;    // stored path, e.g. "samples.json"
    std::string content;
};

// Returns the archive bytes, or an empty vector if deflating failed (which
// in practice means out of memory - there is no malformed input here).
std::vector<uint8_t> build(const std::vector<Entry> &entries);

} // namespace ZipWriter
