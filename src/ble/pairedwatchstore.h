#pragma once

#include "pairedwatch.h"

#include <QByteArray>
#include <QString>
#include <QVector>

// Local SQLite storage for the watch the user picked in PairingPage. The
// *active* watch is a single row, like CloudAccountStore, because the app
// drives one at a time - but every watch that has ever been active is also
// remembered, so switching between a Race and a 9 Baro is a menu entry
// rather than forget-then-find-then-pick.
class PairedWatchStore
{
public:
    explicit PairedWatchStore(const QString &dbPath);

    bool open(QString *error);

    // Returns a default-constructed (isValid() == false) PairedWatch if
    // none has been chosen yet.
    PairedWatch load(QString *error) const;
    bool save(const PairedWatch &watch, QString *error);

    // Forgets the active watch, and stops remembering it: a user who says
    // "forget" means it, so this drops the known_watches row too.
    bool clear(QString *error);

    // Every watch that has been the active one, most recently used first,
    // including the current one. The object path is remembered with each,
    // because BlueZ's path is derived from the adapter and the address and
    // is stable for as long as the device stays bonded - and a switch
    // needs it to call Connect().
    QVector<PairedWatch> knownWatches(QString *error = nullptr) const;

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
