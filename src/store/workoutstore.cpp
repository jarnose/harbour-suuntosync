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
            "  max_heart_rate REAL NOT NULL DEFAULT 0,"
            "  epoc REAL NOT NULL DEFAULT 0,"
            "  peak_training_effect REAL NOT NULL DEFAULT 0,"
            "  recovery_time REAL NOT NULL DEFAULT 0,"
            "  max_vo2 REAL NOT NULL DEFAULT 0,"
            "  training_load REAL NOT NULL DEFAULT 0,"
            "  training_stress_score REAL NOT NULL DEFAULT 0"
            ")"));
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }

    // The columns after total_descent were all added after this table was
    // already in use
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
        QStringLiteral("ALTER TABLE workouts ADD COLUMN epoc REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN peak_training_effect REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN recovery_time REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN max_vo2 REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN training_load REAL NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE workouts ADD COLUMN training_stress_score REAL NOT NULL DEFAULT 0"),
    };
    for (const QString &statement : kMigrationColumns) {
        QSqlQuery migrate(db);
        migrate.exec(statement); // failure here just means the column already exists
    }

    QSqlQuery routes(db);
    if (!routes.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workout_routes ("
            "  key TEXT PRIMARY KEY,"
            "  points BLOB NOT NULL"
            ")"))) {
        if (error)
            *error = routes.lastError().text();
        return false;
    }

    QSqlQuery laps(db);
    if (!laps.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workout_laps ("
            "  key TEXT PRIMARY KEY,"
            "  laps TEXT NOT NULL"
            ")"))) {
        if (error)
            *error = laps.lastError().text();
        return false;
    }

    QSqlQuery series(db);
    if (!series.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workout_series ("
            "  key TEXT PRIMARY KEY,"
            "  series TEXT NOT NULL"
            ")"))) {
        if (error)
            *error = series.lastError().text();
        return false;
    }

    // The upload inputs plus the outcome. uploaded_key stays NULL until the
    // cloud has taken it; see WorkoutStore::saveSmlSources().
    QSqlQuery sml(db);
    if (!sml.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workout_sml ("
            "  key TEXT PRIMARY KEY,"
            "  data BLOB,"
            "  summary BLOB,"
            "  uploaded_key TEXT"
            ")"))) {
        if (error)
            *error = sml.lastError().text();
        return false;
    }

    // This table first held a finished `zip` column; the rows it left
    // behind carry a payload built by the old, broken writer, so they are
    // dropped rather than migrated - the data is re-fetchable from the
    // watch, and keeping them would silently offer a bad upload.
    {
        QSqlQuery legacy(db);
        if (legacy.exec(QStringLiteral("SELECT zip FROM workout_sml LIMIT 1"))) {
            legacy.exec(QStringLiteral("DROP TABLE workout_sml"));
            legacy.exec(QStringLiteral(
                    "CREATE TABLE workout_sml ("
                    "  key TEXT PRIMARY KEY,"
                    "  data BLOB,"
                    "  summary BLOB,"
                    "  uploaded_key TEXT"
                    ")"));
        }
    }

    QSqlQuery details(db);
    if (!details.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS workout_details ("
            "  key TEXT PRIMARY KEY,"
            "  fields TEXT NOT NULL"
            ")"))) {
        if (error)
            *error = details.lastError().text();
        return false;
    }

    return true;
}

bool WorkoutStore::saveLaps(const QString &key, const QByteArray &json, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO workout_laps (key, laps) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET laps = excluded.laps"));
    q.addBindValue(key);
    q.addBindValue(QString::fromUtf8(json));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QByteArray WorkoutStore::loadLaps(const QString &key) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT laps FROM workout_laps WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return QByteArray();
    return q.value(0).toString().toUtf8();
}

bool WorkoutStore::saveSeries(const QString &key, const QByteArray &json, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO workout_series (key, series) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET series = excluded.series"));
    q.addBindValue(key);
    q.addBindValue(QString::fromUtf8(json));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QByteArray WorkoutStore::loadSeries(const QString &key) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT series FROM workout_series WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return QByteArray();
    return q.value(0).toString().toUtf8();
}

bool WorkoutStore::updateTrainingMetrics(const QString &key, double epoc,
                                          double peakTrainingEffect, double recoveryTime,
                                          QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "UPDATE workouts SET epoc = ?, peak_training_effect = ?, recovery_time = ? "
            "WHERE key = ?"));
    q.addBindValue(epoc);
    q.addBindValue(peakTrainingEffect);
    q.addBindValue(recoveryTime);
    q.addBindValue(key);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool WorkoutStore::saveDetails(const QString &key, const QByteArray &json, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO workout_details (key, fields) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET fields = excluded.fields"));
    q.addBindValue(key);
    q.addBindValue(QString::fromUtf8(json));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QByteArray WorkoutStore::loadDetails(const QString &key) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT fields FROM workout_details WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return QByteArray();
    return q.value(0).toString().toUtf8();
}

bool WorkoutStore::saveRoute(const QString &key, const QByteArray &packedPoints, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO workout_routes (key, points) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET points = excluded.points"));
    q.addBindValue(key);
    q.addBindValue(packedPoints);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QByteArray WorkoutStore::loadRoute(const QString &key) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT points FROM workout_routes WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return QByteArray();
    return q.value(0).toByteArray();
}

QVector<Workout> WorkoutStore::loadAll(QString *error) const
{
    QVector<Workout> result;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT key, source, activity_id, start_time, stop_time, total_time, "
            "total_distance, total_ascent, total_descent, max_speed, energy_consumption, "
            "step_count, avg_heart_rate, max_heart_rate, epoc, peak_training_effect, "
            "recovery_time, max_vo2, training_load, training_stress_score FROM workouts "
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
        w.epoc = q.value(14).toDouble();
        w.peakTrainingEffect = q.value(15).toDouble();
        w.recoveryTime = q.value(16).toDouble();
        w.maxVo2 = q.value(17).toDouble();
        w.trainingLoad = q.value(18).toDouble();
        w.trainingStressScore = q.value(19).toDouble();
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
            "energy_consumption, step_count, avg_heart_rate, max_heart_rate, epoc, "
            "peak_training_effect, recovery_time, max_vo2, training_load, "
            "training_stress_score) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET source = excluded.source, "
            "activity_id = excluded.activity_id, start_time = excluded.start_time, "
            "stop_time = excluded.stop_time, total_time = excluded.total_time, "
            "total_distance = excluded.total_distance, total_ascent = excluded.total_ascent, "
            "total_descent = excluded.total_descent, max_speed = excluded.max_speed, "
            "energy_consumption = excluded.energy_consumption, "
            "step_count = excluded.step_count, avg_heart_rate = excluded.avg_heart_rate, "
            "max_heart_rate = excluded.max_heart_rate, epoc = excluded.epoc, "
            "peak_training_effect = excluded.peak_training_effect, "
            "recovery_time = excluded.recovery_time, max_vo2 = excluded.max_vo2, "
            "training_load = excluded.training_load, "
            "training_stress_score = excluded.training_stress_score"));
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
    q.addBindValue(workout.epoc);
    q.addBindValue(workout.peakTrainingEffect);
    q.addBindValue(workout.recoveryTime);
    q.addBindValue(workout.maxVo2);
    q.addBindValue(workout.trainingLoad);
    q.addBindValue(workout.trainingStressScore);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool WorkoutStore::saveSmlSources(const QString &key, const QByteArray &data,
                                   const QByteArray &summary, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    // Not INSERT OR REPLACE: that would drop uploaded_key and let a re-sync
    // offer an already-uploaded workout again.
    q.prepare(QStringLiteral(
            "INSERT INTO workout_sml (key, data, summary) VALUES (?, ?, ?) "
            "ON CONFLICT(key) DO UPDATE SET data = excluded.data,"
            " summary = excluded.summary"));
    q.addBindValue(key);
    q.addBindValue(data);
    q.addBindValue(summary.isEmpty() ? QVariant(QVariant::ByteArray) : QVariant(summary));
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool WorkoutStore::loadSmlSources(const QString &key, QByteArray *data,
                                   QByteArray *summary) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("SELECT data, summary FROM workout_sml WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return false;
    if (data)
        *data = q.value(0).toByteArray();
    if (summary)
        *summary = q.value(1).toByteArray();
    return !q.value(0).toByteArray().isEmpty();
}

bool WorkoutStore::markSmlUploaded(const QString &key, const QString &cloudKey, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("UPDATE workout_sml SET uploaded_key = ? WHERE key = ?"));
    q.addBindValue(cloudKey);
    q.addBindValue(key);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool WorkoutStore::isSmlUploaded(const QString &key) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral(
            "SELECT uploaded_key FROM workout_sml WHERE key = ? AND uploaded_key IS NOT NULL"));
    q.addBindValue(key);
    return q.exec() && q.next();
}

QVector<QString> WorkoutStore::keysPendingUpload() const
{
    QVector<QString> keys;
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    // Joined against workouts so the ordering is by when the workout
    // happened rather than by key, which is only incidentally sortable.
    if (!q.exec(QStringLiteral(
            "SELECT s.key FROM workout_sml s"
            " LEFT JOIN workouts w ON w.key = s.key"
            " WHERE s.uploaded_key IS NULL AND s.data IS NOT NULL"
            " ORDER BY w.start_time ASC"))) {
        return keys;
    }
    while (q.next())
        keys.append(q.value(0).toString());
    return keys;
}
