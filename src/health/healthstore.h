#pragma once

#include "healthentry.h"

#include <QString>
#include <QVector>


// Local SQLite cache of 247.sports-tracker.com's timeline data, same
// open/CRUD style as WorkoutStore.
class HealthStore
{
public:
    explicit HealthStore(const QString &dbPath);

    bool open(QString *error);

    // Insert-or-replace keyed by (kind, timestamp): re-syncing an overlapping
    // window is a no-op rather than a duplicate, which matters because the
    // cloud's `since` filter is inclusive and the app re-fetches from the
    // last known entry.
    // `fromWatch` marks the rows as needing an upload to the cloud. A
    // re-sync of the same entry from the cloud clears it, which is correct:
    // if the cloud already has it there is nothing to send.
    bool upsert(const QVector<HealthEntry> &entries, QString *error, bool fromWatch = false);

    // Newest first. limit <= 0 means no limit.
    QVector<HealthEntry> load(const QString &kind, int limit, QString *error) const;

    // Entries this device decoded from the watch and has not yet pushed to
    // the cloud. Rows that came *from* the cloud are never pending - they
    // are already there - so the flag is set at insert time by the caller
    // rather than inferred later.
    QVector<HealthEntry> loadPendingUpload(const QString &kind, int limit) const;
    bool markUploaded(const QString &kind, const QVector<qint64> &timestamps, QString *error);
    int pendingUploadCount() const;

    // The newest stored timestamp for a kind, or 0 if there is none - this
    // is what gets passed back as `since`, so a sync only fetches what is
    // new. Returns 0 on error too: re-fetching everything is the safe
    // failure, a silent gap is not.
    qint64 newestTimestamp(const QString &kind) const;

private:
    QString m_dbPath;
    QString m_connectionName;
};
