#pragma once

#include <QString>

// Plain workout record, model-agnostic (source == "cloud" today; "ble" once
// LogbookSync exists - see ~/.claude/plans/agile-hopping-harp.md, Phase 6's
// still-open LogEntry field mapping). Field names/units mirror
// RemoteSyncedWorkout from tajchert/suuntool's internal/api/endpoints/
// workouts.go (GET /v1/workouts payload) - only the summary-list fields for
// now, not the heavier optional ones (polyline, tss, hrdata, positions).
struct Workout
{
    QString key;        // Suunto's own id, e.g. "wk_abc123" - primary key here too
    QString source;      // "cloud" or "ble"
    int activityId = 0;  // Suunto's numeric sport type; see ActivityName() equivalent, not ported yet
    qint64 startTime = 0; // unix ms
    qint64 stopTime = 0;  // unix ms
    double totalTime = 0;      // seconds
    double totalDistance = 0;  // meters
    double totalAscent = 0;    // meters
    double totalDescent = 0;   // meters
};
