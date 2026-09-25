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
    // Fills in the three training metrics that only arrive later, from a
    // cloud workout's extensions (see AppController::loadCloudDetails()) -
    // a targeted update rather than a full upsert, since everything else
    // about the row is already correct.
    bool updateTrainingMetrics(const QString &key, double epoc, double peakTrainingEffect,
                                double recoveryTime, QString *error);

    bool saveDetails(const QString &key, const QByteArray &json, QString *error);
    QByteArray loadDetails(const QString &key) const;

    // Per-sample series for a workout's graphs - heart rate, altitude and
    // the rest - already reduced to a fixed number of points per series
    // (see AppController::buildSeriesJson()), since the raw form runs to
    // thousands of samples and a phone-sized chart can't show them. JSON,
    // its own table, same reasoning as the two above.
    bool saveSeries(const QString &key, const QByteArray &json, QString *error);
    QByteArray loadSeries(const QString &key) const;

    // A workout's lap markers with their splits, as JSON. Same pattern
    // again: a variable-length list that the workout list has no use for.
    bool saveLaps(const QString &key, const QByteArray &json, QString *error);
    QByteArray loadLaps(const QString &key) const;

    // The two raw payloads a cloud upload is built from: /Data (still
    // Heatshrink-compressed, as the watch sent it) and /Summary. Kept
    // because refetching them would mean going back to the watch.
    //
    // Deliberately NOT the finished zip, which is what this stored first.
    // That was a mistake: a fix to the JSON writer left every already-synced
    // workout carrying the old broken payload, so correcting a bug meant
    // re-syncing from the watch to see the correction. Storing the inputs
    // and building the zip at upload time costs about the same space
    // (/Data is already compressed) and makes every fix take effect
    // immediately.
    bool saveSmlSources(const QString &key, const QByteArray &data,
                         const QByteArray &summary, QString *error);
    bool loadSmlSources(const QString &key, QByteArray *data, QByteArray *summary) const;
    // Set once the cloud has accepted it, so the action can stop offering
    // itself and a second tap can't create a duplicate workout up there.
    bool markSmlUploaded(const QString &key, const QString &cloudKey, QString *error);
    bool isSmlUploaded(const QString &key) const;

    // Workouts that have a stored payload but no cloud key yet - i.e.
    // synced from the watch and not yet uploaded. Oldest first, so a batch
    // upload puts them in the cloud in the order they happened.
    QVector<QString> keysPendingUpload() const;

private:
    QString m_dbPath;
    QString m_connectionName;
};
