#include "pairedwatchstore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QFileInfo>
#include <QDir>

namespace {
constexpr int kSingletonRowId = 1;
}

PairedWatchStore::PairedWatchStore(const QString &dbPath)
    : m_dbPath(dbPath)
    , m_connectionName(QStringLiteral("suuntosync_paired_watch"))
{
}

bool PairedWatchStore::open(QString *error)
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
    bool ok = q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS paired_watch ("
            "  id INTEGER PRIMARY KEY,"
            "  address TEXT NOT NULL,"
            "  object_path TEXT NOT NULL,"
            "  name TEXT NOT NULL,"
            // Genuinely unknown until Phase 8 derives it from the GATT
            // service set - NULL, not an empty-string placeholder.
            "  model TEXT"
            ")"));

    if (ok) {
        // Added after the fact, so a device that already has a paired_watch
        // row gets this table on the next launch rather than on a wipe.
        ok = q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS watch_descriptors ("
                "  address TEXT PRIMARY KEY,"
                "  payload BLOB NOT NULL"
                ")"));
    }
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

PairedWatch PairedWatchStore::load(QString *error) const
{
    PairedWatch watch;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT address, object_path, name, model FROM paired_watch WHERE id = ?"));
    q.addBindValue(kSingletonRowId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return watch;
    }
    if (q.next()) {
        watch.address = q.value(0).toString();
        watch.objectPath = q.value(1).toString();
        watch.name = q.value(2).toString();
        watch.model = q.value(3).toString();
    }
    return watch;
}

bool PairedWatchStore::save(const PairedWatch &watch, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "INSERT INTO paired_watch (id, address, object_path, name, model) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET address = excluded.address, "
            "object_path = excluded.object_path, name = excluded.name, model = excluded.model"));
    q.addBindValue(kSingletonRowId);
    q.addBindValue(watch.address);
    q.addBindValue(watch.objectPath);
    q.addBindValue(watch.name);
    q.addBindValue(watch.model);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool PairedWatchStore::clear(QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM paired_watch WHERE id = ?"));
    q.addBindValue(kSingletonRowId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool PairedWatchStore::saveDescriptors(const QString &address, const QByteArray &payload,
                                        QString *error)
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral(
            "INSERT INTO watch_descriptors (address, payload) VALUES (?, ?) "
            "ON CONFLICT(address) DO UPDATE SET payload = excluded.payload"));
    q.addBindValue(address);
    q.addBindValue(payload);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QByteArray PairedWatchStore::loadDescriptors(const QString &address) const
{
    QSqlQuery q(QSqlDatabase::database(m_connectionName));
    q.prepare(QStringLiteral("SELECT payload FROM watch_descriptors WHERE address = ?"));
    q.addBindValue(address);
    if (!q.exec() || !q.next())
        return QByteArray();
    return q.value(0).toByteArray();
}
