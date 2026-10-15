#pragma once

#include "../ble/sbemcontainer.h"
#include "../ble/sbemtable.h"

#include <cstdint>
#include <string>
#include <vector>

// Builds the JSON the Suunto cloud accepts for a watch-recorded workout.
//
// A watch-synced upload is POST /apiserver/v1/workout as multipart with two
// parts: `workoutExtensions` (just "[]") and `sml`, a zip holding
// samples.json and summary.json. Confirmed against a real capture - see
// docs/workout-upload.md. There is deliberately no `workoutBinary`: that
// part is what a *phone-recorded* workout sends instead, and the two are
// alternatives.
//
// Both JSON files have the same envelope:
//
//   {"Samples": [ {"Attributes": {"suunto/sml": {...}},
//                  "Source": "suunto-<serial>",
//                  "TimeISO8601": "2026-09-22T15:20:28.430+03:00"}, ... ]}
//
// and one entry is one SBEM chunk: the chunk's descriptor names spell out
// the object to build, since the watch's own table stores them as dotted
// paths ("Sample.HR", "Header.Duration",
// "Sample.Events.Array.Lap.Type"). A path segment "Array" means the segment
// before it is a JSON array, which is how Events arrive.
//
// Values keep the schema's canonical units, because that is what the cloud
// stores: a captured upload has "HR": 1.35, i.e. hertz, not bpm.
//
// Qt-free, so it can be tested here against a real /Data fixture rather
// than only on the phone.
// Only referenced; src/fit/activity.h is included where these are built.
namespace Fit { struct Activity; }

namespace SmlJson {

// `source` is the watch's own identifier, e.g. "suunto-2352D0000247" - the
// serial as it appears in a captured upload. `offsetMinutes` is the local
// UTC offset to stamp timestamps with; the watch records local time plus an
// offset (see Sbem::decodeLocal64) and the cloud's format keeps both.
//
// `fallbackTimeMs` stamps entries that carry no clock of their own. A
// /Data payload needs none (it has a time base chunk); a /Summary payload
// has no clock at all, and the captured upload stamps its entries at the
// end of the workout. Pass 0 for /Data.
//
// Returns the document, or an empty string if nothing decodable was found.
std::string buildDocument(const std::vector<Sbem::Chunk> &chunks,
                           const std::string &source, int offsetMinutes,
                           int64_t fallbackTimeMs = 0);

// The same, against a particular watch's field table. Descriptor ids are
// per watch model (see src/ble/sbemtable.h), so a 9 Baro's payload built
// against a Race's table would produce an empty document - and an upload
// of nothing.
std::string buildDocument(const std::vector<Sbem::Chunk> &chunks,
                           const SbemDescriptors::Table &table,
                           const std::string &source, int offsetMinutes,
                           int64_t fallbackTimeMs = 0);

// The same two documents for a workout that came from a FIT file rather
// than from a watch. Here rather than in a module of their own so that
// `formatNumber()` - locale-proof, and the shape the capture showed -
// stays one copy: a second number formatter is a second chance to send a
// decimal comma, which this project has already done once.
//
// The field names and units are the watch's, because they are the cloud's:
// position in radians, heart rate and cadence in hertz, temperature in
// kelvin, energy in joules. `activityId` is the SML ActivityType the
// import mapped the file's sport to.
//
// Deliberately a small subset of the sixty fields a watch's own Header
// carries. A minimal document is known to be accepted - an indoor workout
// with almost nothing in it uploaded cleanly while a richer one was being
// refused for an unrelated reason - and inventing figures the file does
// not state would be worse than leaving the cloud to compute them from the
// samples.
std::string buildFitSamples(const Fit::Activity &activity, const std::string &source,
                             int offsetMinutes);
std::string buildFitSummary(const Fit::Activity &activity, const std::string &source,
                             int offsetMinutes, int activityId);

} // namespace SmlJson
