#include "mdswhiteboardclient.h"
#include "bluezmanagedobjects.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>

#include <algorithm>

namespace {

const QString kBluezService = QStringLiteral("org.bluez");
const QString kPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");
const QString kDeviceInterface = QStringLiteral("org.bluez.Device1");
const QString kServiceInterface = QStringLiteral("org.bluez.GattService1");
const QString kCharInterface = QStringLiteral("org.bluez.GattCharacteristic1");
const QString kObjectManagerInterface = QStringLiteral("org.freedesktop.DBus.ObjectManager");

// Confirmed 2026-09-21 against a real Suunto Race via HCI snoop log - see
// mdswirecodec.h and ~/.claude/plans/agile-hopping-harp.md, Phase 0a.
const QString kWhiteboardServiceUuid = QStringLiteral("61353090-8231-49cc-b57a-886370740041");
const QString kWriteCharUuid = QStringLiteral("17816557-5652-417f-909f-3aee61e5fa85");
const QString kNotifyCharUuid = QStringLiteral("34802252-7185-4d5d-b431-630e7050e8f0");

constexpr int kRequestTimeoutMs = 10000;

} // namespace

MdsWhiteboardClient::MdsWhiteboardClient(QObject *parent)
    : QObject(parent)
{
    registerBluezManagedObjectTypes();
}

void MdsWhiteboardClient::attachToDevice(const QString &deviceObjectPath)
{
    detach();
    m_deviceObjectPath = deviceObjectPath;

    QDBusConnection::systemBus().connect(
            kBluezService, m_deviceObjectPath, kPropertiesInterface,
            QStringLiteral("PropertiesChanged"), this,
            SLOT(onDevicePropertiesChanged(QString, QVariantMap, QStringList)));

    // ServicesResolved may already be true (e.g. we were already connected
    // before this client attached) - try immediately rather than only ever
    // reacting to a future PropertiesChanged.
    tryDiscoverGattObjects();
}

void MdsWhiteboardClient::detach()
{
    if (!m_deviceObjectPath.isEmpty()) {
        QDBusConnection::systemBus().disconnect(
                kBluezService, m_deviceObjectPath, kPropertiesInterface,
                QStringLiteral("PropertiesChanged"), this,
                SLOT(onDevicePropertiesChanged(QString, QVariantMap, QStringList)));
    }
    if (!m_notifyCharPath.isEmpty()) {
        QDBusConnection::systemBus().disconnect(
                kBluezService, m_notifyCharPath, kPropertiesInterface,
                QStringLiteral("PropertiesChanged"), this,
                SLOT(onNotifyPropertiesChanged(QString, QVariantMap, QStringList)));
    }

    const bool wasReady = isReady();
    m_deviceObjectPath.clear();
    m_servicePath.clear();
    m_writeCharPath.clear();
    m_notifyCharPath.clear();
    m_handshakeAcked = false;

    // Nothing queued or in flight will ever resolve now - fail everything
    // rather than leaking callbacks that silently never fire.
    if (m_inFlightRequestId != 0) {
        const ResponseCallback callback = m_inFlightCallback;
        m_inFlightRequestId = 0;
        m_inFlightCallback = nullptr;
        callback(false, Mds::Frame(), tr("Disconnected"));
    }
    while (!m_queue.isEmpty())
        m_queue.dequeue().callback(false, Mds::Frame(), tr("Disconnected"));

    if (wasReady)
        emit readyChanged(false);
}

void MdsWhiteboardClient::onDevicePropertiesChanged(const QString &interface,
                                                     const QVariantMap &changed,
                                                     const QStringList &invalidated)
{
    Q_UNUSED(invalidated);
    if (interface != kDeviceInterface)
        return;
    if (changed.value(QStringLiteral("ServicesResolved")).toBool())
        tryDiscoverGattObjects();
}

void MdsWhiteboardClient::tryDiscoverGattObjects()
{
    if (isReady() || m_deviceObjectPath.isEmpty())
        return;

    QDBusInterface manager(kBluezService, QStringLiteral("/"), kObjectManagerInterface,
                            QDBusConnection::systemBus());
    QDBusReply<BluezObjectMap> reply = manager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid()) {
        emit errorOccurred(tr("Could not list GATT objects: %1").arg(reply.error().message()));
        return;
    }

    const QString devicePrefix = m_deviceObjectPath + QLatin1Char('/');
    const BluezObjectMap objects = reply.value();

    QString servicePath;
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const QString path = it.key().path();
        if (!path.startsWith(devicePrefix) || !it.value().contains(kServiceInterface))
            continue;
        const QString uuid = it.value().value(kServiceInterface)
                                      .value(QStringLiteral("UUID")).toString();
        if (uuid.compare(kWhiteboardServiceUuid, Qt::CaseInsensitive) == 0) {
            servicePath = path;
            break;
        }
    }
    if (servicePath.isEmpty())
        return; // not resolved yet (or this isn't a Suunto/Movesense watch) - wait for the next signal

    QString writePath;
    QString notifyPath;
    const QString servicePrefix = servicePath + QLatin1Char('/');
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const QString path = it.key().path();
        if (!path.startsWith(servicePrefix) || !it.value().contains(kCharInterface))
            continue;
        const QString uuid = it.value().value(kCharInterface)
                                      .value(QStringLiteral("UUID")).toString();
        if (uuid.compare(kWriteCharUuid, Qt::CaseInsensitive) == 0)
            writePath = path;
        else if (uuid.compare(kNotifyCharUuid, Qt::CaseInsensitive) == 0)
            notifyPath = path;
    }

    if (writePath.isEmpty() || notifyPath.isEmpty()) {
        emit errorOccurred(tr("Whiteboard service found but write/notify characteristics "
                               "are missing"));
        return;
    }

    m_servicePath = servicePath;
    m_writeCharPath = writePath;
    m_notifyCharPath = notifyPath;
    subscribeToNotify();
}

void MdsWhiteboardClient::subscribeToNotify()
{
    QDBusConnection::systemBus().connect(
            kBluezService, m_notifyCharPath, kPropertiesInterface,
            QStringLiteral("PropertiesChanged"), this,
            SLOT(onNotifyPropertiesChanged(QString, QVariantMap, QStringList)));

    QDBusInterface notifyChar(kBluezService, m_notifyCharPath, kCharInterface,
                               QDBusConnection::systemBus());
    QDBusReply<void> reply = notifyChar.call(QStringLiteral("StartNotify"));
    if (!reply.isValid()) {
        emit errorOccurred(tr("Could not subscribe to watch notifications: %1")
                                    .arg(reply.error().message()));
        return;
    }

    sendHandshake();
}

void MdsWhiteboardClient::sendHandshake()
{
    QString error;
    if (!writeChunked(Mds::literalSessionHandshakeRequest(), &error)) {
        emit errorOccurred(tr("Could not send session handshake: %1").arg(error));
        return;
    }

    QTimer::singleShot(kRequestTimeoutMs, this, [this]() {
        if (!m_handshakeAcked) {
            emit errorOccurred(tr("Watch didn't acknowledge the session handshake "
                                   "(TYPE=0x13) - GET requests would time out too"));
        }
    });
}

void MdsWhiteboardClient::onNotifyPropertiesChanged(const QString &interface,
                                                     const QVariantMap &changed,
                                                     const QStringList &invalidated)
{
    Q_UNUSED(invalidated);
    if (interface != kCharInterface || !changed.contains(QStringLiteral("Value")))
        return;

    const QByteArray value = changed.value(QStringLiteral("Value")).toByteArray();
    const auto frames = m_decoder.feed(reinterpret_cast<const uint8_t *>(value.constData()),
                                        static_cast<size_t>(value.size()));
    for (const Mds::Frame &frame : frames) {
        // TYPE=0x13, reqId=0 is specifically the session handshake's reply
        // (see Mds::literalSessionHandshakeRequest()) - reqId 0 is otherwise
        // unused since get() starts its own counter at 1, so this can't
        // collide with a real pending GET response.
        if (frame.type == 0x13 && frame.requestId == 0 && !m_handshakeAcked) {
            m_handshakeAcked = true;
            emit readyChanged(true);
            continue;
        }

        if (frame.requestId != m_inFlightRequestId)
            continue; // response to a request we've already timed out, or a spontaneous event
        resolveInFlight(frame.requestId, true, frame, QString());
    }
}

void MdsWhiteboardClient::resolveInFlight(uint16_t requestId, bool ok, const Mds::Frame &frame,
                                           const QString &error)
{
    if (requestId != m_inFlightRequestId)
        return; // already resolved (e.g. a timeout firing after the real response arrived)

    const ResponseCallback callback = m_inFlightCallback;
    m_inFlightRequestId = 0;
    m_inFlightCallback = nullptr;
    callback(ok, frame, error);
    dispatchNext();
}

namespace {
// Chosen to fit inside the BLE-mandated minimum ATT_MTU (23 bytes, 20
// usable after the 3-byte ATT header) with no MTU negotiation required at
// all - see writeChunked()'s doc comment in the header for the full
// reasoning (confirmed via a real HCI capture: Android's captured session
// had negotiated a 127-byte MTU, so it never needed to split a write; ours
// can't assume that).
constexpr int kWriteChunkSize = 20;
} // namespace

bool MdsWhiteboardClient::writeChunked(const std::vector<uint8_t> &framed, QString *error)
{
    // Empty options, not an explicit {"type": "command"}: cross-checked
    // against sznowicki-sailfish/vapaamin (a real, working Sailfish BLE
    // companion app using the exact same BlueZ WriteValue mechanism, via
    // KF5BluezQt rather than raw D-Bus, but the same underlying
    // GattCharacteristic1.WriteValue call) - it passes QVariantMap() and
    // lets BlueZ pick the write type from the characteristic's own
    // supported properties, which is "command" here regardless since ours
    // only advertises "Write without Response" (see mdswirecodec.h).
    QVariantMap options;

    QDBusInterface writeChar(kBluezService, m_writeCharPath, kCharInterface,
                              QDBusConnection::systemBus());

    for (size_t offset = 0; offset < framed.size(); offset += kWriteChunkSize) {
        const size_t chunkLen = std::min<size_t>(kWriteChunkSize, framed.size() - offset);
        const QByteArray chunk(reinterpret_cast<const char *>(framed.data() + offset),
                                static_cast<int>(chunkLen));
        QDBusReply<void> reply = writeChar.call(QStringLiteral("WriteValue"), chunk, options);
        if (!reply.isValid()) {
            if (error)
                *error = reply.error().message();
            return false;
        }
    }
    return true;
}

void MdsWhiteboardClient::get(const QString &path, ResponseCallback callback)
{
    if (!isReady()) {
        callback(false, Mds::Frame(), tr("Whiteboard channel isn't ready yet"));
        return;
    }

    m_queue.enqueue({ path, callback });
    dispatchNext();
}

void MdsWhiteboardClient::dispatchNext()
{
    if (m_inFlightRequestId != 0 || m_queue.isEmpty())
        return;

    const QueuedRequest next = m_queue.dequeue();
    const uint16_t requestId = m_nextRequestId++;
    const std::vector<uint8_t> framed = Mds::encodeGetRequest(requestId, next.path.toStdString());

    QString error;
    if (!writeChunked(framed, &error)) {
        next.callback(false, Mds::Frame(), tr("Write failed: %1").arg(error));
        dispatchNext(); // this one failed outright - try the next queued request, if any
        return;
    }

    m_inFlightRequestId = requestId;
    m_inFlightCallback = next.callback;

    QTimer::singleShot(kRequestTimeoutMs, this, [this, requestId]() {
        resolveInFlight(requestId, false, Mds::Frame(),
                         tr("Timed out waiting for the watch to respond"));
    });
}
