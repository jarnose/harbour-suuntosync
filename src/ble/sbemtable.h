#pragma once

#include "sbemdescriptors.h"

#include <cstdint>
#include <string>
#include <vector>

// A descriptor table that belongs to one watch, built at runtime from that
// watch's own /Logbook/byId/<id>/Descriptors reply.
//
// Why this exists: the compiled-in table in sbemdescriptors.h is a Suunto
// Race's, and the numbering is per model. A Suunto 9 Baro transfers a
// workout perfectly and then decodes to all zeros, because it puts GPS in
// chunk 0x0d where the Race puts it in 0x0c, heart rate in 0x15 where the
// Race puts it in 0x12, and so on. 211 of the two watches' descriptor
// texts are character-for-character identical and exactly five of those
// share an id. See docs/sbem-chunk-map.md.
//
// Qt-free on purpose, like every other decoder here, so it can be tested
// against both watches' real replies with plain g++.
namespace SbemDescriptors {

// Descriptor::size values that mean something other than a byte count.
// -1 is the generated table's own convention for utf8; -2 is new and says
// the <FRM> named a format this build has no size for, which is different
// from "variable" and has to stop offset arithmetic dead.
constexpr int8_t kVariableSize = -1;
constexpr int8_t kUnknownSize = -2;

class Table
{
public:
    Table() = default;
    Table(const Table &other);
    Table(Table &&other) noexcept;
    Table &operator=(Table other);

    // Parses a raw /Descriptors reply (an SBEM0103 container of
    // definition chunks). Returns an empty table if the payload isn't one;
    // a table that parses at all is used, because a single field this
    // project never reads should not cost a whole watch.
    static Table parse(const std::vector<uint8_t> &payload);

    // The generated Race table, for a watch whose own has not been read.
    static const Table &builtin();

    bool empty() const { return m_entries.empty(); }
    size_t size() const { return m_entries.size(); }

    // nullptr for an id this table doesn't describe.
    const Descriptor *find(uint16_t id) const;

    // False if this descriptor's <MOD> expression was not recognised, so
    // its raw value cannot be converted to a unit. Callers suppress the
    // reading rather than emit one scaled by a guess.
    bool hasKnownScale(uint16_t id) const;

    // Every descriptor, ascending by id - SbemLayout::resolve() walks them
    // because it has to find which group carries a field rather than being
    // told which one does.
    const Descriptor *at(size_t index) const { return &m_entries[index].descriptor; }

    // Descriptors whose <FRM> named a format this build doesn't know, and
    // whose size is therefore unknown. Counted rather than fatal: an
    // unknown format only makes the offsets *after* it unusable, which
    // SbemLayout handles, and refusing the table outright would mean
    // refusing the watch.
    int unknownFormats() const { return m_unknownFormats; }
    // Descriptors with a <MOD> expression this build doesn't recognise.
    // Their size is still known, so offsets are fine; only their scaling
    // is not, and readings from them are suppressed rather than emitted
    // wrongly scaled. The 9 Baro brought one the Race never had - a
    // compass heading - which is why this is a count and not a hard error.
    int unknownMods() const { return m_unknownMods; }

private:
    struct Entry
    {
        Descriptor descriptor {};
        std::string name;
        std::vector<uint16_t> children;
    };

    // Descriptor holds bare pointers into Entry::name/children, so they
    // have to be re-pointed whenever the vector that owns them moves.
    void relink();

    std::vector<Entry> m_entries; // sorted by id
    std::vector<uint16_t> m_unscaled; // sorted; see hasKnownScale()
    int m_unknownFormats = 0;
    int m_unknownMods = 0;
};

} // namespace SbemDescriptors
