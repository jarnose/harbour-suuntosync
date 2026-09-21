#include "workoutstore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QFileInfo>
#include <QDir>

WorkoutStore::WorkoutStore(const QString &dbPath)
    : m_dbPath(dbPath)
    , m_connectionName(QStringLiteral("suuntosync_workouts"))
{
}

bool WorkoutStore::open(QString *error)
{
    QDir().mkpath(QFileInfo(m_dbPath).absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_dbPath);
    if (!db.open()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }

    QSqlQuery q(db);
    const bool ok = q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workouts ("
            "  key TEXT PRIMARY KEY,"
            "  source TEXT NOT NULL,"
            "  activity_id INTEGER NOT NULL,"
            "  start_time INTEGER NOT NULL,"
            "  stop_time INTEGER NOT NULL,"
            "  total_time REAL NOT NULL,"
            "  total_distance REAL NOT NULL,"
            "  total_ascent REAL NOT NULL,"
            "  total_descent REAL NOT NULL"
            ")"));
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QVector<Workout> WorkoutStore::loadAll(QString *error) const
{
    QVector<Workout> result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT key, source, activity_id, start_time, stop_time, total_time, "
            "total_distance, total_ascent, total_descent FROM workouts "
            "ORDER BY start_time DESC"))) {
        if (error)
            *error = q.lastError().text();
        return result;
    }

    while (q.next()) {
        Workout w;
        w.key = q.value(0).toString();
        w.source = q.value(1).toString();
        w.activityId = q.value(2).toInt();
        w.startTime = q.value(3).toLongLong();
        w.stopTime = q.value(4).toLongLong();
        w.totalTime = q.value(5).toDouble();
        w.totalDistance = q.value(6).toDouble();
        w.totalAscent = q.value(7).toDouble();
        w.totalDescent = q.value(8).toDouble();
        result.append(w);
    }
    return result;
}

bool WorkoutStore::upsert(const Workout &workout, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO workouts (key, source, activity_id, start_time, stop_time, "
            "total_time, total_distance, total_ascent, total_descent) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET source = excluded.source, "
            "activity_id = excluded.activity_id, start_time = excluded.start_time, "
            "stop_time = excluded.stop_time, total_time = excluded.total_time, "
            "total_distance = excluded.total_distance, total_ascent = excluded.total_ascent, "
            "total_descent = excluded.total_descent"));
    q.addBindValue(workout.key);
    q.addBindValue(workout.source);
    q.addBindValue(workout.activityId);
    q.addBindValue(workout.startTime);
    q.addBindValue(workout.stopTime);
    q.addBindValue(workout.totalTime);
    q.addBindValue(workout.totalDistance);
    q.addBindValue(workout.totalAscent);
    q.addBindValue(workout.totalDescent);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}
