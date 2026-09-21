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
            "  total_descent REAL NOT NULL,"
            "  max_speed REAL NOT NULL DEFAULT 0,"
            "  energy_consumption REAL NOT NULL DEFAULT 0,"
            "  step_count INTEGER NOT NULL DEFAULT 0,"
            "  avg_heart_rate REAL NOT NULL DEFAULT 0,"
            "  max_heart_rate REAL NOT NULL DEFAULT 0"
            ")"));
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }

    // The five columns above were added after this table was already in use
    // on-device (same lesson as PairedWatchStore's model column bug) -
    // CREATE TABLE IF NOT EXISTS is a no-op against an existing table, so a
    // pre-existing database needs these added explicitly. SQLite has no
    // "ADD COLUMN IF NOT EXISTS", so just attempt each and ignore the
    // "duplicate column" error - which is exactly what happens, harmlessly,
    // on a fresh database where the CREATE TABLE above already included them.
    static const QStringList kMigrationColumns = {
        QStringLiteral("ALTER TABLE workouts ADD COLUMN max_speed REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN energy_consumption REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN step_count INTEGER NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN avg_heart_rate REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN max_heart_rate REAL NOT NULL DEFAULT 0"),
    };
    for (const QString &statement : kMigrationColumns) {
        QSqlQuery migrate(db);
        migrate.exec(statement); // failure here just means the column already exists
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
            "total_distance, total_ascent, total_descent, max_speed, energy_consumption, "
            "step_count, avg_heart_rate, max_heart_rate FROM workouts "
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
        w.maxSpeed = q.value(9).toDouble();
        w.energyConsumption = q.value(10).toDouble();
        w.stepCount = q.value(11).toInt();
        w.avgHeartRate = q.value(12).toDouble();
        w.maxHeartRate = q.value(13).toDouble();
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
            "total_time, total_distance, total_ascent, total_descent, max_speed, "
            "energy_consumption, step_count, avg_heart_rate, max_heart_rate) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET source = excluded.source, "
            "activity_id = excluded.activity_id, start_time = excluded.start_time, "
            "stop_time = excluded.stop_time, total_time = excluded.total_time, "
            "total_distance = excluded.total_distance, total_ascent = excluded.total_ascent, "
            "total_descent = excluded.total_descent, max_speed = excluded.max_speed, "
            "energy_consumption = excluded.energy_consumption, "
            "step_count = excluded.step_count, avg_heart_rate = excluded.avg_heart_rate, "
            "max_heart_rate = excluded.max_heart_rate"));
    q.addBindValue(workout.key);
    q.addBindValue(workout.source);
    q.addBindValue(workout.activityId);
    q.addBindValue(workout.startTime);
    q.addBindValue(workout.stopTime);
    q.addBindValue(workout.totalTime);
    q.addBindValue(workout.totalDistance);
    q.addBindValue(workout.totalAscent);
    q.addBindValue(workout.totalDescent);
    q.addBindValue(workout.maxSpeed);
    q.addBindValue(workout.energyConsumption);
    q.addBindValue(workout.stepCount);
    q.addBindValue(workout.avgHeartRate);
    q.addBindValue(workout.maxHeartRate);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}
