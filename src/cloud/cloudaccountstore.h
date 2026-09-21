#pragma once

#include "accountmeta.h"

#include <QString>

// Local SQLite storage for the Suunto cloud account's *metadata* only (email,
// athlete id, token expiry, last sync time) - same split as
// harbour-otpcove's AccountStore/SecretVault: nothing secret lives here, the
// actual token bytes are in TokenVault. At most one row (fixed id) - this
// app only supports a single signed-in Suunto account at a time.
class CloudAccountStore
{
public:
    // Names under which TokenVault stores this account's secrets.
    static const QString TokenSecretName;
    static const QString RefreshTokenSecretName;

    explicit CloudAccountStore(const QString &dbPath);

    bool open(QString *error);

    // Returns a default-constructed (isSignedIn() == false) CloudAccount if
    // no account has been saved yet.
    CloudAccount load(QString *error) const;
    bool save(const CloudAccount &account, QString *error);
    bool clear(QString *error);

private:
    QString m_dbPath;
    QString m_connectionName;
};
