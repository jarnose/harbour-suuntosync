#include "cloudaccountstore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QFileInfo>
#include <QDir>

const QString CloudAccountStore::TokenSecretName = QStringLiteral("cloud_access_token");
const QString CloudAccountStore::RefreshTokenSecretName = QStringLiteral("cloud_refresh_token");

namespace {
constexpr int kSingletonRowId = 1;
}

CloudAccountStore::CloudAccountStore(const QString &dbPath)
    : m_dbPath(dbPath)
    , m_connectionName(QStringLiteral("suuntosync_cloud_account"))
{
}

bool CloudAccountStore::open(QString *error)
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
            "CREATE TABLE IF NOT EXISTS cloud_account ("
            "  id INTEGER PRIMARY KEY,"
            "  email TEXT NOT NULL,"
            "  athlete_id TEXT NOT NULL,"
            "  token_expiry INTEGER NOT NULL,"
            "  last_sync INTEGER NOT NULL"
            ")"));
    if (!ok) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

CloudAccount CloudAccountStore::load(QString *error) const
{
    CloudAccount account;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
            "SELECT email, athlete_id, token_expiry, last_sync FROM cloud_account WHERE id = ?"));
    q.addBindValue(kSingletonRowId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return account;
    }
    if (q.next()) {
        account.email = q.value(0).toString();
        account.athleteId = q.value(1).toString();
        account.tokenExpiry = q.value(2).toLongLong();
        account.lastSync = q.value(3).toLongLong();
    }
    return account;
}

bool CloudAccountStore::save(const CloudAccount &account, QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    // Single-row upsert - id is always kSingletonRowId, so this always
    // replaces whatever account was previously saved.
    q.prepare(QStringLiteral(
            "INSERT INTO cloud_account (id, email, athlete_id, token_expiry, last_sync) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET email = excluded.email, "
            "athlete_id = excluded.athlete_id, token_expiry = excluded.token_expiry, "
            "last_sync = excluded.last_sync"));
    q.addBindValue(kSingletonRowId);
    q.addBindValue(account.email);
    q.addBindValue(account.athleteId);
    q.addBindValue(account.tokenExpiry);
    q.addBindValue(account.lastSync);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

bool CloudAccountStore::clear(QString *error)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM cloud_account WHERE id = ?"));
    q.addBindValue(kSingletonRowId);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}
