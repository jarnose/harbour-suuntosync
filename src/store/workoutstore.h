#pragma once

#include "workout.h"

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

private:
    QString m_dbPath;
    QString m_connectionName;
};
