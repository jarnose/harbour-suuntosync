#pragma once

#include <QString>

// Plain workout record, model-agnostic (source == "cloud" today; "ble" once
// LogbookSync exists - see ~/.claude/plans/agile-hopping-harp.md, Phase 6's
// still-open LogEntry field mapping). Field names/units mirror
// RemoteSyncedWorkout from tajchert/suuntool's internal/api/endpoints/
// workouts.go - note that struct is shared by *both* GET /v1/workouts (the
// list) and GET /v1/workouts/{key} (the detail), so the optional fields
// below (energyConsumption/maxSpeed/stepCount/heart rate) already arrive in
// the plain list response - no separate per-workout detail fetch needed for
// these. Only `extensions` (Fitness/Intensity/etc., a detail-only field) is
// still out of scope.
struct Workout
{
    QString key;         // Suunto's own id, e.g. "wk_abc123" - primary key here too
    QString source;      // "cloud" or "ble"
    int activityId = 0;  // Suunto's numeric sport type; see ActivityName() equivalent, not ported yet
    qint64 startTime = 0; // unix ms
    qint64 stopTime = 0;  // unix ms
    double totalTime = 0;      // seconds
    double totalDistance = 0;  // meters
    double totalAscent = 0;    // meters
    double totalDescent = 0;   // meters

    // All optional in the API response (Suunto omits a field entirely when
    // it has no value, e.g. a strength workout has no maxSpeed) - 0 doubles
    // as "absent" here rather than adding a parallel set of has-X bools,
    // since none of these are legitimately exactly 0 for a real workout.
    double maxSpeed = 0;          // m/s
    double energyConsumption = 0; // kcal
    int stepCount = 0;
    double avgHeartRate = 0; // bpm (hrdata.avg)
    double maxHeartRate = 0; // bpm (hrdata.max)

    // Training metrics. Only a BLE sync fills these, from the watch's own
    // /Summary header (see src/ble/summarydecoder.h) - the cloud workout
    // list doesn't carry them. Same "0 means absent" convention as above,
    // which matches the watch's own schema marking every one nillable=0.
    double epoc = 0;              // ml/kg
    double peakTrainingEffect = 0; // 1.0-5.0
    double recoveryTime = 0;      // seconds
    double maxVo2 = 0;            // ml/kg/min
    double trainingLoad = 0;      // Suunto's Header.TraingingLoadPeak
};
