#pragma once

#include "pairedwatch.h"

#include <QByteArray>
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

    // The watch's own SBEM field table, as fetched from
    // /Logbook/byId/<id>/Descriptors. Kept per address rather than on the
    // single paired_watch row, so switching between two watches and back
    // doesn't throw the other's table away - and because the table is what
    // makes that watch's workouts decodable at all (see
    // src/ble/sbemlayout.h).
    bool saveDescriptors(const QString &address, const QByteArray &payload, QString *error);
    QByteArray loadDescriptors(const QString &address) const;

private:
    QString m_dbPath;
    QString m_connectionName;
};
