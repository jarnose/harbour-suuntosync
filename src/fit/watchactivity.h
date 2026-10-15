#pragma once

#include "activity.h"

#include "../ble/sbemtable.h"

#include <cstdint>
#include <vector>

// Turns a watch's own workout payload into the shape a FIT file is written
// from, so a workout read over BLE can be exported like an imported one.
//
// Only the samples: the summary figures a watch workout shows come from
// its /Summary resource and from the stored row, which the caller already
// has and this cannot see. It fills Summary::startTime and the altitude
// extremes, because those fall out of the samples for free.
namespace Fit {

// `compressedData` is the /Data payload as the watch sent it, Heatshrink
// and all. `table` must be the table of the watch that recorded it - the
// chunk ids are per model, and the wrong table yields a workout of zeros
// rather than an error (see src/ble/sbemtable.h).
Activity activityFromWatchData(const std::vector<uint8_t> &compressedData,
                                const SbemDescriptors::Table &table);

} // namespace Fit
