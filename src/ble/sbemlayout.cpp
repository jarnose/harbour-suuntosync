#include "sbemlayout.h"

#include <algorithm>
#include <cstring>

namespace SbemLayout {

namespace {

using SbemDescriptors::Descriptor;
using SbemDescriptors::Format;
using SbemDescriptors::Table;

// The names are the watch's own, with the prefixes the table strips
// already removed - so they are the same strings on both watches, which is
// the whole reason this resolves by name.
constexpr const char *kUtc = "Sample.UTC";
constexpr const char *kLatitude = "Sample.Latitude";
constexpr const char *kLongitude = "Sample.Longitude";
constexpr const char *kHeartRate = "Sample.HR";
constexpr const char *kCadence = "Sample.Cadence";
constexpr const char *kAltitude = "Sample.Altitude";
constexpr const char *kActivityType = "Sample.Events.Array.Activity.ActivityType";
constexpr const char *kAltitudeOffset = "Sample.Events.Array.Altitude.AltitudeOffset";
constexpr const char *kTimeIso = "TimeISO8601";

bool named(const Descriptor *d, const char *name)
{
    return d && d->name && std::strcmp(d->name, name) == 0;
}

// Every Header.* field this build reads, with where to put the offset.
struct HeaderSlot
{
    const char *name;
    int Header::*member;
    int width;
};

const HeaderSlot kHeaderSlots[] = {
    { "Header.DateTime", &Header::dateTime, 8 },
    { "Header.Duration", &Header::duration, 4 },
    { "Header.PauseDuration", &Header::pauseDuration, 4 },
    { "Header.Distance", &Header::distance, 4 },
    { "Header.StepCount", &Header::stepCount, 4 },
    { "Header.ActivityType", &Header::activityType, 4 },
    { "Header.Ascent", &Header::ascent, 4 },
    { "Header.Descent", &Header::descent, 4 },
    { "Header.Altitude.Max", &Header::altitudeMax, 4 },
    { "Header.Altitude.Min", &Header::altitudeMin, 4 },
    { "Header.Energy", &Header::energy, 4 },
    { "Header.EPOC", &Header::epoc, 4 },
    { "Header.PeakTrainingEffect", &Header::peakTrainingEffect, 4 },
    { "Header.RecoveryTime", &Header::recoveryTime, 4 },
    { "Header.MAXVO2", &Header::maxVo2, 4 },
    // Suunto's own spelling, not a typo here.
    { "Header.TraingingLoadPeak", &Header::trainingLoad, 4 },
};

// Which descriptor carries Sample.Altitude as a reading. A delta against
// it is how the altitude series continues between absolute samples, and
// the only way to recognise that delta is by what it refers to - it has no
// name of its own.
uint16_t altitudeDescriptorId(const Table &table)
{
    for (size_t i = 0; i < table.size(); ++i) {
        const Descriptor *d = table.at(i);
        if (named(d, kAltitude) && d->childCount == 0 && d->deltaOf == 0)
            return d->id;
    }
    return 0;
}

} // namespace

const Group *Layout::group(uint16_t id) const
{
    const auto it = std::lower_bound(groups.begin(), groups.end(), id,
            [](const Group &g, uint16_t wanted) { return g.id < wanted; });
    if (it == groups.end() || it->id != id)
        return nullptr;
    return &*it;
}

Layout resolve(const Table &table)
{
    Layout layout;
    const uint16_t altitudeId = altitudeDescriptorId(table);

    for (size_t i = 0; i < table.size(); ++i) {
        const Descriptor *groupDescriptor = table.at(i);
        if (groupDescriptor->childCount == 0)
            continue;

        Group group;
        group.id = groupDescriptor->id;

        int offset = 0;
        bool sizeKnown = true;
        for (uint16_t c = 0; c < groupDescriptor->childCount; ++c) {
            const Descriptor *child = table.find(groupDescriptor->children[c]);
            if (!child) {
                // A group referring to a descriptor the table doesn't
                // define: everything after this point is unplaceable.
                sizeKnown = false;
                break;
            }

            if (named(child, kUtc)) group.utc = offset;
            else if (named(child, kLatitude)) group.latitude = offset;
            else if (named(child, kLongitude)) group.longitude = offset;
            else if (named(child, kHeartRate)) group.heartRate = offset;
            else if (named(child, kCadence)) group.cadence = offset;
            else if (named(child, kActivityType)) group.activityType = offset;
            else if (named(child, kAltitudeOffset)) group.altitudeOffset = offset;
            else if (named(child, kAltitude)) group.altitudeAbsolute = offset;

            // Not by name: a delta has none. It is recognised by what it
            // refers to and by being a one-byte delta.
            if (altitudeId != 0 && child->deltaOf == altitudeId
                    && child->format == Format::DeltaInt8) {
                group.altitudeDelta = offset;
            }

            if (child->name && std::strncmp(child->name, "Header.", 7) == 0)
                ++group.headerFieldCount;

            if (child->size < 0) {
                // utf8 (variable) or a format this build has no size for -
                // either way, nothing after it can be placed. Fields found
                // before it keep their offsets, which is why this breaks
                // rather than discarding the group.
                sizeKnown = false;
                break;
            }
            offset += child->size;
        }
        group.totalSize = sizeKnown ? offset : -1;

        group.timeBase = groupDescriptor->childCount == 1
                && named(table.find(groupDescriptor->children[0]), kTimeIso)
                && table.find(groupDescriptor->children[0])->format == Format::Local64;

        layout.groups.push_back(group);
    }

    std::sort(layout.groups.begin(), layout.groups.end(),
              [](const Group &a, const Group &b) { return a.id < b.id; });

    // The header group is the one with the most Header.* fields. Several
    // groups carry Header.Duration - both watches have a second, ten-field
    // one - and the full workout header is unambiguously the largest.
    const Group *best = nullptr;
    for (const Group &g : layout.groups) {
        if (g.headerFieldCount > 0 && (!best || g.headerFieldCount > best->headerFieldCount))
            best = &g;
    }
    if (best) {
        layout.headerGroup = best->id;

        const Descriptor *header = table.find(best->id);
        int offset = 0;
        for (uint16_t c = 0; c < header->childCount; ++c) {
            const Descriptor *child = table.find(header->children[c]);
            if (!child || child->size < 0)
                break;
            for (const HeaderSlot &slot : kHeaderSlots) {
                if (named(child, slot.name)) {
                    layout.header.*(slot.member) = offset;
                    layout.header.minimumSize =
                            std::max(layout.header.minimumSize, offset + slot.width);
                }
            }
            offset += child->size;
        }
    }

    return layout;
}

const Layout &builtin()
{
    static const Layout layout = resolve(Table::builtin());
    return layout;
}

} // namespace SbemLayout
