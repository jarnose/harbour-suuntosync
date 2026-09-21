#include "tokenvault.h"

#include <Secrets/secretmanager.h>
#include <Secrets/secret.h>
#include <Secrets/createcollectionrequest.h>
#include <Secrets/storesecretrequest.h>
#include <Secrets/storedsecretrequest.h>
#include <Secrets/deletesecretrequest.h>
#include <Secrets/result.h>

using namespace Sailfish::Secrets;

const QString TokenVault::CollectionName = QStringLiteral("SuuntoSyncSecrets");

TokenVault::TokenVault(QObject *parent)
    : QObject(parent)
    , m_manager(new SecretManager(this))
{
}

TokenVault::~TokenVault() = default;

void TokenVault::ensureCollection(ResultCallback callback)
{
    auto *req = new CreateCollectionRequest(this);
    req->setManager(m_manager);
    req->setCollectionName(CollectionName);
    req->setAccessControlMode(SecretManager::OwnerOnlyMode);
    req->setCollectionLockType(CreateCollectionRequest::DeviceLock);
    // Keep the vault unlocked once the device itself has been unlocked, same
    // as OTP Cove - re-prompting for a device-lock code just to refresh a
    // background sync token would be an odd UX for this app.
    req->setDeviceLockUnlockSemantic(SecretManager::DeviceLockKeepUnlocked);
    req->setStoragePluginName(SecretManager::DefaultEncryptedStoragePluginName);
    req->setEncryptionPluginName(SecretManager::DefaultEncryptedStoragePluginName);
    req->setUserInteractionMode(SecretManager::SystemInteraction);

    connect(req, &Request::statusChanged, req, [req, callback]() {
        if (req->status() != Request::Finished)
            return;
        const Result result = req->result();
        const bool ok = result.code() != Result::Failed
                || result.errorCode() == Result::CollectionAlreadyExistsError;
        const QString error = ok ? QString() : result.errorMessage();
        req->deleteLater();
        callback(ok, error);
    });
    req->startRequest();
}

void TokenVault::storeSecret(const QString &name, const QByteArray &data, ResultCallback callback)
{
    Secret secret(Secret::Identifier(name, CollectionName,
                                      SecretManager::DefaultEncryptedStoragePluginName));
    secret.setData(data);
    secret.setType(Secret::TypeBlob);

    auto *req = new StoreSecretRequest(this);
    req->setManager(m_manager);
    req->setSecretStorageType(StoreSecretRequest::CollectionSecret);
    // See harbour-otpcove's secretvault.cpp for why this must be
    // SystemInteraction rather than PreventInteraction.
    req->setUserInteractionMode(SecretManager::SystemInteraction);
    req->setSecret(secret);

    connect(req, &Request::statusChanged, req, [req, callback]() {
        if (req->status() != Request::Finished)
            return;
        const Result result = req->result();
        const bool ok = result.code() != Result::Failed;
        const QString error = ok ? QString() : result.errorMessage();
        req->deleteLater();
        callback(ok, error);
    });
    req->startRequest();
}

void TokenVault::loadSecret(const QString &name, DataCallback callback)
{
    auto *req = new StoredSecretRequest(this);
    req->setManager(m_manager);
    req->setIdentifier(Secret::Identifier(name, CollectionName,
                                           SecretManager::DefaultEncryptedStoragePluginName));
    req->setUserInteractionMode(SecretManager::SystemInteraction);

    connect(req, &Request::statusChanged, req, [req, callback]() {
        if (req->status() != Request::Finished)
            return;
        const Result result = req->result();
        const bool ok = result.code() != Result::Failed;
        const QByteArray data = ok ? req->secret().data() : QByteArray();
        const QString error = ok ? QString() : result.errorMessage();
        req->deleteLater();
        callback(ok, data, error);
    });
    req->startRequest();
}

void TokenVault::deleteSecret(const QString &name, ResultCallback callback)
{
    auto *req = new DeleteSecretRequest(this);
    req->setManager(m_manager);
    req->setIdentifier(Secret::Identifier(name, CollectionName,
                                           SecretManager::DefaultEncryptedStoragePluginName));
    req->setUserInteractionMode(SecretManager::SystemInteraction);

    connect(req, &Request::statusChanged, req, [req, callback]() {
        if (req->status() != Request::Finished)
            return;
        const Result result = req->result();
        const bool ok = result.code() != Result::Failed;
        const QString error = ok ? QString() : result.errorMessage();
        req->deleteLater();
        callback(ok, error);
    });
    req->startRequest();
}
