#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The watch's own plugin registry: `index.db` in the plugin directory.
//
// This exists because none of the three resources that *describe* what is
// installed can be trusted to be complete. `/Plugin/List` named fifteen
// things where the directory held seventeen files, and it does not name a
// plugin whose file has been deleted at all - while the watch keeps drawing
// that plugin in its own SuuntoPlus menu, as a row with a button and no
// name, because the name lived in the file. The registry is the thing those
// rows come from, so it is the only honest answer to "what will the watch
// show".
//
// The format, read off a real Suunto Race (408 bytes, 20 entries, every
// byte accounted for):
//
//     entry := id(8 bytes, ASCII, space-padded) value(LE32)* 0xFFFFFFFF
//
// The first value is the kind, and it agrees with the file extensions the
// directory listing shows:
//
//     1 = sports app   (<pluginId>.fea)
//     2 = guide        (<id>.gui)
//     4 = development build, pushed by hand (<id>.dev)
//     8 = watch face   (<id>.wfa)
//
// The second value was 0 for all twenty. Two entries carried a third one -
// 54 for a sports app, 3 for the guide - and what it means is not known;
// it is kept rather than discarded so a later reading has it.
//
// Qt-free on purpose, like the other decoders here, so it can be tested
// with a plain compiler.
namespace PluginRegistry {

struct Entry
{
    std::string id;
    uint32_t kind = 0;
    std::vector<uint32_t> values; // kind included, as it arrived
};

// Parses the whole file. A truncated entry at the end is dropped rather
// than guessed at: the file is written by the watch and a short read is a
// transport failure, not a format.
std::vector<Entry> parse(const std::vector<uint8_t> &bytes);

// The file extension a kind is stored under, or an empty string for a kind
// this does not know. Used to tell "the file is there" from "the file is
// gone" without a second listing.
std::string extensionForKind(uint32_t kind);

} // namespace PluginRegistry
