#pragma once

#include "pairedwatch.h"

#include <QString>

// Local SQLite storage for the watch the user picked in PairingPage. Single
// row (like CloudAccountStore) - this app only drives one watch at a time
// for now; Phase 8 (Suunto 9 Baro support) may extend this to several.
class PairedWatchStore
{
public:
    explicit PairedWatchStore(const QString &dbPath);

    bool open(QString *error);

    // Returns a default-constructed (isValid() == false) PairedWatch if
    // none has been chosen yet.
    PairedWatch load(QString *error) const;
    bool save(const PairedWatch &watch, QString *error);
    bool clear(QString *error);

private:
    QString m_dbPath;
    QString m_connectionName;
};
