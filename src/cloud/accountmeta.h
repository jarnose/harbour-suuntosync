#pragma once

#include <QString>

// Non-secret metadata about the signed-in Suunto cloud account. The actual
// OAuth access/refresh token bytes live in TokenVault (Sailfish Secrets),
// keyed by CloudAccountStore::TokenSecretName /
// CloudAccountStore::RefreshTokenSecretName - never here.
struct CloudAccount
{
    QString email;
    QString athleteId;
    qint64 tokenExpiry = 0; // unix seconds; 0 = unknown/never fetched
    qint64 lastSync = 0;    // unix seconds; 0 = never synced

    // The cursor for an incremental workout sync: the server's own
    // `metadata.until` from the last successful /v1/workouts response,
    // passed back as `since` on the next one. Unix *milliseconds*, and
    // deliberately not derived from lastSync above - that one is this
    // phone's clock, and the comparison happens on the server's. A
    // measured capture of the official app shows it doing exactly this,
    // and shows `since` matching a workout's server-side ingest time
    // rather than when the workout was recorded: workouts started on
    // 25.9 came back for since = 5.10 09:00 because they reached the
    // cloud that afternoon. 0 = no cursor, fetch the whole history.
    qint64 workoutCursor = 0;

    bool isSignedIn() const { return !athleteId.isEmpty(); }
};
