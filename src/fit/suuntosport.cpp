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

bool fitSportFromSuunto(int activityId, int *sport, int *subSport)
{
    // The inverse of the table above, written out rather than searched, so
    // that the several watch activities which share one FIT sport - track
    // cycling and plain cycling both being sport 2 - resolve the way round
    // that loses nothing. The sub-sport values are FIT's own, as
    // Fit::sportKey() reads them.
    static const std::map<int, std::pair<int, int>> kMap = {
        { 3, { 1, -1 } },    // Running
        { 82, { 1, 3 } },    // Trail running
        { 93, { 1, 1 } },    // Treadmill
        { 4, { 2, -1 } },    // Cycling
        { 17, { 2, 6 } },    // Indoor cycling
        { 5, { 2, 8 } },     // Mountain biking
        { 7, { 2, 46 } },    // Gravel cycling
        { 107, { 2, 11 } },  // Cyclocross
        { 108, { 2, 12 } },  // Hand cycling
        { 105, { 2, 28 } },  // E-biking
        { 12, { 11, -1 } },  // Walking
        { 96, { 17, -1 } },  // Hiking
        { 6, { 5, -1 } },    // Swimming
        { 15, { 15, -1 } },  // Rowing
        { 22, { 12, -1 } },  // Nordic skiing
        { 20, { 13, -1 } },  // Downhill skiing
        { 18, { 10, -1 } },  // Circuit training
    };
    const auto it = kMap.find(activityId);
    if (it == kMap.end())
        return false;
    if (sport)
        *sport = it->second.first;
    if (subSport)
        *subSport = it->second.second;
    return true;
}

} // namespace Fit
