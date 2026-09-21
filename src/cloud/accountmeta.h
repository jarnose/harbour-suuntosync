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

    bool isSignedIn() const { return !athleteId.isEmpty(); }
};
