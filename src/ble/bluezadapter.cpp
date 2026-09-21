#include "bluezadapter.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>

namespace {

const QString kBluezService = QStringLiteral("org.bluez");
const QString kObjectManagerInterface = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString kAdapterInterface = QStringLiteral("org.bluez.Adapter1");
const QString kDeviceInterface = QStringLiteral("org.bluez.Device1");

} // namespace

BluezAdapter::BluezAdapter(QObject *parent)
    : QObject(parent)
{
    registerBluezManagedObjectTypes();

    // "Any path" (empty string) match - InterfacesAdded is emitted by the
    // single root ObjectManager object ("/"), not per-device, and already
    // carries the new object's path as its first argument, so no per-device
    // subscription juggling is needed here (unlike PropertiesChanged, which
    // is per-object and deliberately NOT tracked yet - see refresh()'s
    // doc comment in the header for why that's an acceptable Phase 5 cut).
    QDBusConnection::systemBus().connect(
            kBluezService, QString(), kObjectManagerInterface,
            QStringLiteral("InterfacesAdded"), this,
            SLOT(onInterfacesAdded(QDBusObjectPath, BluezInterfaceMap)));
}

QString BluezAdapter::adapterPath()
{
    if (!m_adapterPath.isEmpty())
        return m_adapterPath;

    QDBusInterface manager(kBluezService, QStringLiteral("/"), kObjectManagerInterface,
                            QDBusConnection::systemBus());
    QDBusReply<BluezObjectMap> reply = manager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid()) {
        emit errorOccurred(tr("Could not reach BlueZ (org.bluez): %1")
                                    .arg(reply.error().message()));
        return QString();
    }

    const BluezObjectMap objects = reply.value();
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        if (it.value().contains(kAdapterInterface)) {
            m_adapterPath = it.key().path();
            break;
        }
    }
    if (m_adapterPath.isEmpty())
        emit errorOccurred(tr("No Bluetooth adapter found"));
    return m_adapterPath;
}

void BluezAdapter::emitDeviceIfPresent(const QString &objectPath,
                                        const BluezInterfaceMap &interfaces)
{
    if (!interfaces.contains(kDeviceInterface))
        return;

    const QVariantMap props = interfaces.value(kDeviceInterface);
    Device device;
    device.objectPath = objectPath;
    device.address = props.value(QStringLiteral("Address")).toString();
    // Alias always has a value (BlueZ defaults it to the address if the
    // device advertised no name); Name may be entirely absent.
    device.name = props.value(QStringLiteral("Alias"),
                               props.value(QStringLiteral("Name"), device.address))
                          .toString();
    device.paired = props.value(QStringLiteral("Paired")).toBool();
    device.connected = props.value(QStringLiteral("Connected")).toBool();
    emit deviceUpdated(device);
}

void BluezAdapter::refresh()
{
    QDBusInterface manager(kBluezService, QStringLiteral("/"), kObjectManagerInterface,
                            QDBusConnection::systemBus());
    QDBusReply<BluezObjectMap> reply = manager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid()) {
        emit errorOccurred(tr("Could not list Bluetooth devices: %1")
                                    .arg(reply.error().message()));
        return;
    }

    const BluezObjectMap objects = reply.value();
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it)
        emitDeviceIfPresent(it.key().path(), it.value());
}

void BluezAdapter::onInterfacesAdded(const QDBusObjectPath &path,
                                      const BluezInterfaceMap &interfaces)
{
    emitDeviceIfPresent(path.path(), interfaces);
}

void BluezAdapter::startDiscovery()
{
    const QString path = adapterPath();
    if (path.isEmpty())
        return;
    QDBusInterface adapter(kBluezService, path, kAdapterInterface, QDBusConnection::systemBus());
    auto *watcher = new QDBusPendingCallWatcher(adapter.asyncCall(QStringLiteral("StartDiscovery")),
                                                 this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<> reply = *w;
        if (reply.isError())
            emit errorOccurred(tr("Could not start scanning: %1").arg(reply.error().message()));
        w->deleteLater();
    });
}

void BluezAdapter::stopDiscovery()
{
    const QString path = adapterPath();
    if (path.isEmpty())
        return;
    QDBusInterface adapter(kBluezService, path, kAdapterInterface, QDBusConnection::systemBus());
    // Fire-and-forget: nothing meaningful to do differently if this fails
    // (e.g. discovery already stopped/never started).
    adapter.asyncCall(QStringLiteral("StopDiscovery"));
}

void BluezAdapter::connectToDevice(const QString &objectPath)
{
    QDBusInterface device(kBluezService, objectPath, kDeviceInterface,
                           QDBusConnection::systemBus());
    auto *watcher = new QDBusPendingCallWatcher(device.asyncCall(QStringLiteral("Connect")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished,
            this, [this, objectPath](QDBusPendingCallWatcher *w) {
                const QDBusPendingReply<> reply = *w;
                emit connectFinished(objectPath, !reply.isError(), reply.error().message());
                w->deleteLater();
            });
}

void BluezAdapter::disconnectFromDevice(const QString &objectPath)
{
    QDBusInterface device(kBluezService, objectPath, kDeviceInterface,
                           QDBusConnection::systemBus());
    device.asyncCall(QStringLiteral("Disconnect"));
}
