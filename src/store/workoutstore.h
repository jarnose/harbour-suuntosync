#pragma once

#include "workout.h"

#include <QByteArray>
#include <QString>
#include <QVector>

// Local SQLite cache of workout summaries - protocol-agnostic (see Workout's
// `source` field), same open/CRUD style as CloudAccountStore/PairedWatchStore.
class WorkoutStore
{
public:
    explicit WorkoutStore(const QString &dbPath);

    bool open(QString *error);

    // Newest first (by startTime).
    QVector<Workout> loadAll(QString *error) const;
    // Insert-or-replace by key - a re-sync just overwrites with the latest data.
    bool upsert(const Workout &workout, QString *error);

    // A workout's GPS track, kept in its own table so loadAll() doesn't drag
    // a few tens of kilobytes per row into the list view. The blob is packed
    // pairs of little-endian int32, degrees x 1e7 - the watch's own on-wire
    // representation, stored as-is rather than re-encoded. Only BLE workouts
    // have one; loadRoute() returns an empty array when there is none.
    bool saveRoute(const QString &key, const QByteArray &packedPoints, QString *error);
    QByteArray loadRoute(const QString &key) const;

    // Everything else the watch recorded for a workout, as a JSON object of
    // field name -> { value, unit }. There are well over a hundred of these
    // (see docs/sbem-chunk-map.md) and only a handful are worth a column of
    // their own, so the rest live here rather than widening the workouts
    // table every time another field gets decoded. Empty for a cloud
    // workout. Its own table for the same reason as the route: the list
    // view has no use for it.
    bool saveDetails(const QString &key, const QByteArray &json, QString *error);
    QByteArray loadDetails(const QString &key) const;

private:
    QString m_dbPath;
    QString m_connectionName;
};
