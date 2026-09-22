#pragma once

#include <cstdint>
#include <vector>

// Decodes a /Logbook/byId/<id>/Summary payload - the watch's own computed
// totals for a workout, as opposed to /Data's raw sample stream. Qt-free
// (STL only), same golden-vector-before-BLE discipline as logbookdecoder.h.
//
// Unlike /Data this resource is not a bulk stream: it comes back in 451-byte
// pages fetched with Mds::encodePagedReadRequest(), whose payloads
// concatenate into an ordinary SBEM0103 container (see that function's doc
// comment for the paging protocol, and MdsWhiteboardClient::fetchSummary()
// for the client side). Pass the concatenated payload here - unlike
// Logbook::decode() there is no Heatshrink layer to undo.
//
// Everything below comes from chunk 0x1b, the 133-field Header group, whose
// exact field order and byte offsets are in docs/sbem-chunk-map.md
// (generated from the watch's own descriptor table). These are the watch's
// figures, not derived ones - so where Logbook::decode() has to approximate
// (ascent/descent within ~10%, active duration, distance from GPS), this is
// exact, and should be preferred whenever a Summary fetch succeeded.
//
// Fields that the schema marks nillable and that read as their "absent"
// value are reported via the has* flags rather than as a zero.
namespace Summary {

struct DecodedSummary
{
    bool valid = false;         // false = no Header chunk found in the payload

    int activityId = 0;
    uint64_t startTimeMs = 0;   // Header.DateTime
    double durationSeconds = 0; // Header.Duration, total elapsed
    double pauseDurationSeconds = 0;
    // Header.Duration minus Header.PauseDuration. On the one workout with a
    // known real figure this reproduced the app's reported duration exactly
    // (2299.8s vs a reported 38:19), which Logbook::decode()'s GPS-gap
    // heuristic only gets within ~7% of.
    double movingTimeSeconds = 0;
    double distanceMeters = 0;
    int stepCount = 0;

    bool hasAscent = false;     // the schema marks ascent/descent nillable=0
    double ascentMeters = 0;
    double descentMeters = 0;

    bool hasAltitude = false;
    double minAltitudeMeters = 0;
    double maxAltitudeMeters = 0;

    bool hasEnergy = false;
    double energyKcal = 0;      // Header.Energy is joules; converted here

    // Training metrics, the same ones the official app's workout view shows.
    // Each is marked nillable=0 in the schema, i.e. the watch writes 0 when
    // it didn't compute one (VO2max and training load are absent from the
    // one real capture available here - evidently not produced for every
    // workout - so their decoding is offset-verified but not value-verified).
    bool hasEpoc = false;
    double epoc = 0;                  // ml/kg
    bool hasPeakTrainingEffect = false;
    double peakTrainingEffect = 0;    // 1.0-5.0
    bool hasRecoveryTime = false;
    double recoveryTimeSeconds = 0;
    bool hasMaxVo2 = false;
    double maxVo2 = 0;                // ml/kg/min
    bool hasTrainingLoad = false;
    // Header.TraingingLoadPeak - Suunto's own spelling, typo included. This
    // is the figure the app labels as training load / TSS.
    double trainingLoad = 0;
};

DecodedSummary decode(const std::vector<uint8_t> &payload);

} // namespace Summary
