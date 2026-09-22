#include "healthstore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QFileInfo>
#include <QDir>

HealthStore::HealthStore(const QString &dbPath)
    : m_dbPath(dbPath)
    , m_connectionName(QStringLiteral("suuntosync_health"))
{
}

bool HealthStore::open(QString *error)
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
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS health_entries ("
            "  kind TEXT NOT NULL,"
            "  timestamp INTEGER NOT NULL,"
            "  data TEXT NOT NULL,"
            "  PRIMARY KEY (kind, timestamp)"
            ")"))) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    // Every query here is "newest first for one kind", which this covers.
    q.exec(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_health_kind_time"
            "  ON health_entries (kind, timestamp DESC)"));
    return true;
}

bool HealthStore::upsert(const QVector<HealthEntry> &entries, QString *error)
{
    if (entries.isEmpty())
        return true;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    // One transaction for the lot: a sync brings a few hundred rows and
    // committing each separately is the difference between milliseconds and
    // seconds on a phone's flash.
    db.transaction();

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO health_entries (kind, timestamp, data)"
            " VALUES (?, ?, ?)"));
    for (const HealthEntry &e : entries) {
        q.addBindValue(e.kind);
        q.addBindValue(e.timestamp);
        q.addBindValue(QString::fromUtf8(e.data));
        if (!q.exec()) {
            if (error)
                *error = q.lastError().text();
            db.rollback();
            return false;
        }
    }

    if (!db.commit()) {
        if (error)
            *error = db.lastError().text();
        return false;
    }
    return true;
}

QVector<HealthEntry> HealthStore::load(const QString &kind, int limit, QString *error) const
{
    QVector<HealthEntry> out;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT kind, timestamp, data FROM health_entries"
            " WHERE kind = ? ORDER BY timestamp DESC")
              + (limit > 0 ? QStringLiteral(" LIMIT ?") : QString()));
    q.addBindValue(kind);
    if (limit > 0)
        q.addBindValue(limit);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        HealthEntry e;
        e.kind = q.value(0).toString();
        e.timestamp = q.value(1).toLongLong();
        e.data = q.value(2).toString().toUtf8();
        out.append(e);
    }
    return out;
}

qint64 HealthStore::newestTimestamp(const QString &kind) const
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT MAX(timestamp) FROM health_entries WHERE kind = ?"));
    q.addBindValue(kind);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong(); // NULL (no rows) converts to 0
}
