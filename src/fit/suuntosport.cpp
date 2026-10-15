#include "suuntosport.h"

#include <map>

namespace Fit {

int suuntoActivityId(const std::string &sportKey)
{
    // Every one of sportKey()'s own return values, so nothing it can say
    // falls through silently. The ids are the watch's: see
    // qml/pages/ActivityTypes.js, which is generated from the official
    // app's ActivityMapping enum.
    static const std::map<std::string, int> kMap = {
        // Running
        { "running", 3 },              // Running
        { "trail_running", 82 },       // Trail running
        { "treadmill_running", 93 },   // Treadmill
        // Cycling, which is where FIT's sub-sports are most detailed
        { "cycling", 4 },              // Cycling
        { "indoor_cycling", 17 },      // Indoor cycling
        { "mountain_biking", 5 },      // Mountain biking
        { "gravel_cycling", 7 },       // Gravel cycling
        { "cyclocross", 107 },         // Cyclocross
        { "hand_cycling", 108 },       // Hand cycling
        { "e_biking", 105 },           // E-biking
        // Track cycling has no entry of its own in the watch's table, so it
        // goes in as what it still is rather than as nothing.
        { "track_cycling", 4 },        // Cycling
        // Everything else sportKey() knows
        { "walking", 12 },             // Walking
        { "hiking", 96 },              // Hiking
        { "swimming", 6 },             // Swimming
        { "rowing", 15 },              // Rowing
        { "cross_country_skiing", 22 },// Nordic skiing
        { "alpine_skiing", 20 },       // Downhill skiing
        { "training", 18 },            // Circuit training
        { "fitness_equipment", 18 },   // Circuit training
        { "other", 1 },                // Unspecified sport
    };
    const auto it = kMap.find(sportKey);
    return it == kMap.end() ? 1 : it->second;
}

} // namespace Fit
