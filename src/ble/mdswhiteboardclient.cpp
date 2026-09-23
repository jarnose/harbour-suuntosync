#include "mdswhiteboardclient.h"
#include "bluezmanagedobjects.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>

#include <algorithm>
#include <stdexcept>

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

// The MDS bulk-chunk header (see docs/logbook-data-format.md's Stage 1):
// 28 bytes from the raw wire packet's start, which is 22 bytes into this
// project's own Mds::Frame::body (packet = 6-byte SYNC/TYPE/LEN/REQID
// envelope, already stripped, + body). chunk_size is a LE u16 at wire
// offset 20, i.e. body offset 14.
constexpr size_t kMdsChunkHeaderSize = 22;
constexpr size_t kMdsChunkSizeOffset = 14;

// How long to wait after the last bulk chunk before deciding the transfer
// is over - libdivecomputer issue #70's independent reverse-engineering of
// the same protocol on a related device settled on 2.0s; not independently
// re-measured against this project's own capture (the transfer there just
// ran to completion without a gap to time), so kept as a documented
// starting point rather than a confirmed value.
constexpr int kBulkStreamSilenceMs = 2000;

// Paged resources (/Summary, /Descriptors) - see
// Mds::encodePagedReadRequest()'s doc comment. Every captured page carried
// a fixed 19-byte header with a status at offset 6: 100 while more pages
// follow, 200 on the last one.
constexpr size_t kPagedHeaderSize = 19;
constexpr size_t kPagedStatusOffset = 6;
constexpr uint16_t kPageStatusContinue = 100;
// A real Summary is ~1 KB; this only exists so a watch that never marks a
// last page can't loop forever.
constexpr size_t kMaxPagedBytes = 1024 * 1024;

} // namespace

MdsWhiteboardClient::MdsWhiteboardClient(QObject *parent)
    : QObject(parent)
{
    registerBluezManagedObjectTypes();

    m_bulkSilenceTimer.setSingleShot(true);
    connect(&m_bulkSilenceTimer, &QTimer::timeout, this, [this]() {
        endBulkStream(!m_bulkBuffer.empty(),
                m_bulkBuffer.empty() ? tr("No bulk data arrived before the silence timeout")
                                      : QString());
    });
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
    if (m_bulkFetchActive)
        finishBulkFetch(false, tr("Disconnected"));

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

        // Bulk-transfer chunks (fetchLogbookData()) - unsolicited, like the
        // handshake reply, so matched the same way (type+reqId) rather than
        // through the request queue's m_inFlightRequestId.
        if (frame.type == 0x01 && frame.requestId == 0 && m_bulkFetchActive) {
            appendBulkChunk(frame.body);
            armBulkSilenceTimer();
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
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, callback);
}

void MdsWhiteboardClient::getRaw(FrameBuilder builder, ResponseCallback callback)
{
    if (!isReady()) {
        callback(false, Mds::Frame(), tr("Whiteboard channel isn't ready yet"));
        return;
    }

    m_queue.enqueue({ std::move(builder), callback });
    dispatchNext();
}

void MdsWhiteboardClient::dispatchNext()
{
    if (m_inFlightRequestId != 0 || m_queue.isEmpty())
        return;

    const QueuedRequest next = m_queue.dequeue();
    const uint16_t requestId = m_nextRequestId++;
    const std::vector<uint8_t> framed = next.builder(requestId);

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

void MdsWhiteboardClient::fetchLogbookData(const QString &path, DataCallback callback)
{
    if (m_bulkFetchActive) {
        callback(false, {}, tr("Another bulk fetch is already in progress"));
        return;
    }

    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ackFrame, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }

        try {
            const std::vector<uint8_t> ackBody = ackFrame.body;
            getRaw([ackBody](uint16_t requestId) {
                return Mds::encodeStreamStartTrigger(requestId, ackBody);
            }, [this, callback, ackBody](bool triggerOk, const Mds::Frame &,
                                           const QString &triggerError) {
                if (!triggerOk) {
                    callback(false, {}, tr("Stream-start trigger failed: %1").arg(triggerError));
                    return;
                }
                m_bulkFetchActive = true;
                m_bulkBuffer.clear();
                m_bulkCallback = callback;
                m_bulkAckBody = ackBody;
                armBulkSilenceTimer();
            });
        } catch (const std::exception &e) {
            callback(false, {}, tr("Could not build the stream-start trigger: %1").arg(e.what()));
        }
    });
}

void MdsWhiteboardClient::fetchLogEntries(const QString &path, EntriesCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ackFrame, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }

        try {
            const std::vector<uint8_t> ackBody = ackFrame.body;
            getRaw([ackBody](uint16_t requestId) {
                return Mds::encodeEntriesFetchTrigger(requestId, ackBody);
            }, [callback](bool fetchOk, const Mds::Frame &frame, const QString &fetchError) {
                if (!fetchOk) {
                    callback(false, {}, tr("Entries fetch failed: %1").arg(fetchError));
                    return;
                }
                const std::vector<LogEntries::Entry> entries = LogEntries::decode(frame.body);
                if (entries.empty()) {
                    callback(false, {}, tr("Entries response didn't decode to any entries "
                                            "(%1 byte body)").arg(frame.body.size()));
                    return;
                }
                callback(true, entries, QString());
            });
        } catch (const std::exception &e) {
            callback(false, {}, tr("Could not build the entries fetch trigger: %1").arg(e.what()));
        }
    });
}

void MdsWhiteboardClient::fetchStructuredRaw(const QString &path, DataCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ackFrame, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        try {
            const std::vector<uint8_t> ackBody = ackFrame.body;
            getRaw([ackBody](uint16_t requestId) {
                return Mds::encodeEntriesFetchTrigger(requestId, ackBody);
            }, [callback](bool fetchOk, const Mds::Frame &frame, const QString &fetchError) {
                if (!fetchOk) {
                    callback(false, {}, tr("Fetch failed: %1").arg(fetchError));
                    return;
                }
                callback(true, frame.body, QString());
            });
        } catch (const std::exception &e) {
            callback(false, {}, tr("Could not build the fetch trigger: %1").arg(e.what()));
        }
    });
}

void MdsWhiteboardClient::fetchSummary(const QString &path, DataCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ackFrame, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ackFrame.body.size() < 6) {
            callback(false, {}, tr("GET %1 was acked with an unusably short body").arg(path));
            return;
        }
        readNextPage(ackFrame.body, 0, {}, callback);
    });
}

void MdsWhiteboardClient::readNextPage(const std::vector<uint8_t> &ackBody, uint32_t offset,
                                        std::vector<uint8_t> collected, DataCallback callback)
{
    getRaw([ackBody, offset](uint16_t requestId) {
        return Mds::encodePagedReadRequest(requestId, ackBody, offset);
    }, [this, ackBody, offset, collected, callback]
            (bool ok, const Mds::Frame &frame, const QString &error) mutable {
        if (!ok) {
            callback(false, {}, tr("Paged read at offset %1 failed: %2").arg(offset).arg(error));
            return;
        }
        if (frame.body.size() < kPagedHeaderSize) {
            callback(false, {}, tr("Paged read at offset %1 returned only %2 bytes")
                                         .arg(offset).arg(frame.body.size()));
            return;
        }

        const uint16_t status = static_cast<uint16_t>(frame.body[kPagedStatusOffset]
                | (static_cast<uint16_t>(frame.body[kPagedStatusOffset + 1]) << 8));
        collected.insert(collected.end(), frame.body.begin() + kPagedHeaderSize, frame.body.end());
        const size_t payloadSize = frame.body.size() - kPagedHeaderSize;

        // "Last page" is the watch saying so; the empty-page and size-cap
        // checks are belt and braces so a watch that never says so can't
        // spin this loop forever.
        if (status != kPageStatusContinue || payloadSize == 0
                || collected.size() >= kMaxPagedBytes) {
            callback(true, collected, QString());
            return;
        }
        readNextPage(ackBody, offset + static_cast<uint32_t>(payloadSize), collected, callback);
    });
}

void MdsWhiteboardClient::appendBulkChunk(const std::vector<uint8_t> &body)
{
    if (body.size() < kMdsChunkHeaderSize)
        return;
    const uint16_t chunkSize = static_cast<uint16_t>(body[kMdsChunkSizeOffset]
            | (static_cast<uint16_t>(body[kMdsChunkSizeOffset + 1]) << 8));
    if (kMdsChunkHeaderSize + chunkSize > body.size())
        return;
    m_bulkBuffer.insert(m_bulkBuffer.end(),
            body.begin() + kMdsChunkHeaderSize,
            body.begin() + kMdsChunkHeaderSize + chunkSize);
}

void MdsWhiteboardClient::armBulkSilenceTimer()
{
    m_bulkSilenceTimer.start(kBulkStreamSilenceMs);
}

void MdsWhiteboardClient::endBulkStream(bool ok, const QString &error)
{
    if (!m_bulkFetchActive)
        return;

    m_bulkSilenceTimer.stop();

    // No ack body (or the stop already went out for this stream) - nothing
    // to tear down, just report.
    if (m_bulkAckBody.size() < 6) {
        finishBulkFetch(ok, error);
        return;
    }

    const std::vector<uint8_t> ackBody = m_bulkAckBody;
    m_bulkAckBody.clear();

    getRaw([ackBody](uint16_t requestId) {
        return Mds::encodeStreamStopTrigger(requestId, ackBody);
    }, [this, ok, error](bool, const Mds::Frame &, const QString &) {
        // Deliberately ignoring the stop's own result: whatever the watch
        // said about it, the bytes already collected are what the caller
        // asked for.
        finishBulkFetch(ok, error);
    });
}

void MdsWhiteboardClient::finishBulkFetch(bool ok, const QString &error)
{
    if (!m_bulkFetchActive)
        return;
    m_bulkFetchActive = false;
    m_bulkSilenceTimer.stop();
    m_bulkAckBody.clear();
    const DataCallback callback = m_bulkCallback;
    const std::vector<uint8_t> data = std::move(m_bulkBuffer);
    m_bulkCallback = nullptr;
    m_bulkBuffer.clear();
    callback(ok, data, error);
}
