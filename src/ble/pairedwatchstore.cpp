#include "pairedwatchstore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDateTime>
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
    if (ok) {
        // Also added after the fact, and for the same reason: switching
        // between two watches used to mean forgetting one first.
        ok = q.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS known_watches ("
                "  address TEXT PRIMARY KEY,"
                "  object_path TEXT NOT NULL,"
                "  name TEXT NOT NULL,"
                "  model TEXT,"
                "  last_used INTEGER NOT NULL"
                ")"));
    }
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }

    // A watch paired before known_watches existed would otherwise be
    // missing from the switcher it is supposed to be the first entry of.
    // INSERT OR IGNORE, so this is a one-time backfill and not a
    // last_used reset on every launch.
    QSqlQuery backfill(db);
    backfill.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO known_watches (address, object_path, name, model, last_used) "
            "SELECT address, object_path, name, model, ? FROM paired_watch"));
    backfill.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!backfill.exec() && error)
        *error = backfill.lastError().text();
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

    // And remember it among the known ones. A failure here is worth
    // reporting but not worth undoing the save: the active watch is set,
    // which is what the caller asked for.
    QSqlQuery remember(db);
    remember.prepare(QStringLiteral(
            "INSERT INTO known_watches (address, object_path, name, model, last_used) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(address) DO UPDATE SET object_path = excluded.object_path, "
            "name = excluded.name, model = excluded.model, last_used = excluded.last_used"));
    remember.addBindValue(watch.address);
    remember.addBindValue(watch.objectPath);
    remember.addBindValue(watch.name);
    remember.addBindValue(watch.model);
    remember.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!remember.exec() && error)
        *error = remember.lastError().text();
    return true;
}

QVector<PairedWatch> PairedWatchStore::knownWatches(QString *error) const
{
    QVector<PairedWatch> watches;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT address, object_path, name, model FROM known_watches "
                                "ORDER BY last_used DESC"))) {
        if (error)
            *error = q.lastError().text();
        return watches;
    }
    while (q.next()) {
        PairedWatch watch;
        watch.address = q.value(0).toString();
        watch.objectPath = q.value(1).toString();
        watch.name = q.value(2).toString();
        watch.model = q.value(3).toString();
        watches.append(watch);
    }
    return watches;
}

bool PairedWatchStore::clear(QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    const PairedWatch active = load(nullptr);

    q.prepare(QStringLiteral("DELETE FROM paired_watch WHERE id = ?"));
    q.addBindValue(kSingletonRowId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }

    // "Forget" has to mean it, or the watch would reappear in the switcher.
    // Its descriptor table is kept: it is expensive to fetch, harmless to
    // keep, and keyed by address, so re-pairing the same watch finds it.
    if (!active.address.isEmpty()) {
        QSqlQuery forget(db);
        forget.prepare(QStringLiteral("DELETE FROM known_watches WHERE address = ?"));
        forget.addBindValue(active.address);
        if (!forget.exec() && error)
            *error = forget.lastError().text();
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
