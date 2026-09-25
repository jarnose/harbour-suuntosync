#pragma once

#include "sbemtable.h"

#include <cstdint>
#include <vector>

// Where the fields this project reads actually live, for one watch.
//
// A chunk in a /Data or /Summary payload is a group descriptor, and the
// group's payload is its children's values concatenated with no per-child
// headers - so a field's byte offset is the sum of the sizes of the
// children before it. That much is the same on every watch. What is not
// the same is the group's *id*: a Suunto Race carries GPS in chunk 0x0c
// and heart rate in 0x12, a Suunto 9 Baro carries them in 0x0d and 0x15.
//
// So rather than hard-coding either watch's numbers, resolve() walks the
// watch's own descriptor table and asks, for every group, "does this one
// carry Sample.Latitude? at what offset?". The decoders then work off the
// answer. See docs/sbem-chunk-map.md for the measurements behind this.
//
// Deliberately not "find the GPS group": the 9 Baro has two groups
// carrying latitude - one with GPS altitude and one without - and both are
// real. Every group is described, and a decoder reads whichever chunk it
// meets.
namespace SbemLayout {

// -1 means "this group has no such field", everywhere below.
struct Group
{
    uint16_t id = 0;
    // Total bytes, or -1 when a variable-length (utf8) child makes the
    // size unknowable. Used only as a lower bound check; a decoder reads
    // by offset, not by expecting an exact length. Expecting an exact
    // length is what made the Race-only decoder reject the 9 Baro's
    // shorter groups outright.
    int totalSize = -1;

    // /Data sample fields.
    int utc = -1;
    int latitude = -1;
    int longitude = -1;
    int heartRate = -1;
    int cadence = -1;
    int altitudeAbsolute = -1;  // Sample.Altitude as a uint16 reading
    int altitudeDelta = -1;     // an int8 delta against Sample.Altitude
    int activityType = -1;      // Sample.Events.Array.Activity.ActivityType
    int altitudeOffset = -1;    // Sample.Events.Array.Altitude.AltitudeOffset

    // The workout's time base: a group whose single child is the
    // "baseonly" TimeISO8601. Every other chunk's leading int16 is a delta
    // against it.
    bool timeBase = false;

    int headerFieldCount = 0;   // how many Header.* children, for picking
                                 // the real header group out of the
                                 // several that carry Header.Duration
};

// The Summary header's fields, by offset within the header group.
struct Header
{
    int dateTime = -1;
    int duration = -1;
    int pauseDuration = -1;
    int distance = -1;
    int stepCount = -1;
    int activityType = -1;
    int ascent = -1;
    int descent = -1;
    int altitudeMax = -1;
    int altitudeMin = -1;
    int energy = -1;
    int epoc = -1;
    int peakTrainingEffect = -1;
    int recoveryTime = -1;
    int maxVo2 = -1;
    int trainingLoad = -1;

    // The highest offset this decoder reads plus its width, so a caller
    // can refuse a chunk too short to hold them rather than read past the
    // end. 0 when nothing was resolved.
    int minimumSize = 0;
};

struct Layout
{
    std::vector<Group> groups;   // ascending by id
    uint16_t headerGroup = 0;    // 0 = none found
    Header header;

    // Anything usable at all. A table that resolves no sample groups is
    // not one this build can decode with.
    bool valid() const { return !groups.empty(); }

    // nullptr if this chunk id is not a group the table describes.
    const Group *group(uint16_t id) const;
};

Layout resolve(const SbemDescriptors::Table &table);

// The Race layout, for a watch whose own descriptors have not been read.
// Resolved once from the compiled-in table.
const Layout &builtin();

} // namespace SbemLayout
