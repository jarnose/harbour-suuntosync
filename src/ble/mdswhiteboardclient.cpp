#include "mdswhiteboardclient.h"
#include "bluezmanagedobjects.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
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
    // Idempotent, and it has to be: AppController attaches both when a
    // Connect() finishes and when an already-connected paired watch turns
    // up, and either can come first. Re-attaching used to tear the session
    // down and build it again, which is wasteful at best - and if
    // QDBusConnection::disconnect() does not match the hook that connect()
    // made, it leaks one hook per attach, and every notification is then
    // delivered as many times as there are hooks. That is fatal for a reply
    // spanning several notifications: the decoder sees each chunk twice and
    // no frame ever satisfies its length and CRC check.
    if (deviceObjectPath == m_deviceObjectPath && m_notifySubscribed) {
        qDebug() << "attachToDevice: already attached to" << deviceObjectPath;
        return;
    }
    qDebug() << "attachToDevice" << deviceObjectPath;
    detach();
    ++m_sessionGeneration;
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

        // StopNotify, and it is not optional. Disconnecting the signal only
        // stops us listening; BlueZ still counts this connection as one of
        // the characteristic's notifying clients until it is told otherwise,
        // and it emits PropertiesChanged once per notifying client. So a
        // daemon that had attached once and then let go left the
        // application receiving every notification twice - survivable for a
        // reply that fits in one notification, because the duplicate lands
        // after the real one resolved the request and the request-id guard
        // drops it, and fatal for one spanning several, because the decoder
        // then sees each chunk twice and no frame satisfies its length and
        // CRC check. That is what made every paged read time out while an
        // HCI capture showed the watch answering correctly.
        //
        // Errors are ignored on purpose: "no notify session" is the state we
        // want, and that is also what the failure means.
        if (m_notifySubscribed) {
            QDBusInterface notifyChar(kBluezService, m_notifyCharPath, kCharInterface,
                                       QDBusConnection::systemBus());
            notifyChar.call(QStringLiteral("StopNotify"));
        }
    }
    m_notifySubscribed = false;

    const bool wasReady = isReady();
    m_deviceObjectPath.clear();
    m_servicePath.clear();
    m_writeCharPath.clear();
    m_notifyCharPath.clear();
    m_handshakeAcked = false;
    // Session-scoped: the handle inside it means nothing to the next session.
    m_removalAck.clear();
    ++m_sessionGeneration;

    // Nothing queued or in flight will ever resolve now - fail everything
    // rather than leaking callbacks that silently never fire.
    if (m_inFlightRequestId != 0) {
        const ResponseCallback callback = m_inFlightCallback;
        m_inFlightRequestId = 0;
        m_inFlightCallback = nullptr;
        if (callback)
            callback(false, Mds::Frame(), tr("Disconnected"));
    }
    while (!m_queue.isEmpty()) {
        const ResponseCallback callback = m_queue.dequeue().callback;
        if (callback)
            callback(false, Mds::Frame(), tr("Disconnected"));
    }
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
    // Not isReady(): that stays false until the handshake reply arrives, so
    // a second ServicesResolved landing inside that window - tens of
    // milliseconds, and BlueZ emits more than one - used to run the whole
    // discovery again, subscribe a second time and send a second handshake.
    // m_notifySubscribed is the honest question: is this session already
    // set up? Discovery stays repeatable while it is not, so a StartNotify
    // that failed can still be retried by the next ServicesResolved.
    if (m_notifySubscribed || m_deviceObjectPath.isEmpty())
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
    // Idempotent on purpose. QDBusConnection::connect() adds another hook
    // rather than recognising the one already there, and a doubled hook
    // delivers every notification twice. A reply that fits in one
    // notification survives that - the duplicate arrives after the real one
    // resolved the request, so the request-id guard drops it - but a reply
    // spanning several does not: the decoder sees chunk1 chunk1 chunk2
    // chunk2, and no frame ever satisfies its length and CRC check, so the
    // request times out with the watch having answered correctly. That is
    // what broke every paged read and every multi-notification response.
    if (!m_notifySubscribed) {
        QDBusConnection::systemBus().connect(
                kBluezService, m_notifyCharPath, kPropertiesInterface,
                QStringLiteral("PropertiesChanged"), this,
                SLOT(onNotifyPropertiesChanged(QString, QVariantMap, QStringList)));
        m_notifySubscribed = true;
        qDebug() << "subscribed to" << m_notifyCharPath;
    }

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

    const quint32 generation = m_sessionGeneration;
    QTimer::singleShot(kRequestTimeoutMs, this, [this, generation]() {
        if (generation != m_sessionGeneration)
            return;  // detached since; this timer is about a session that is gone
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
        // Kept deliberately. A doubled D-Bus subscription showed up here as
        // every frame being logged twice, and nothing else in this class was
        // visible from outside; an HCI capture proves what the watch sent,
        // not what we made of it. Bulk chunks are the one high-volume case
        // and are counted elsewhere, so they stay out.
        if (frame.type != 0x01) {
            qDebug() << "frame type" << int(frame.type) << "reqId" << int(frame.requestId)
                     << "body" << int(frame.body.size())
                     << "in flight" << int(m_inFlightRequestId);
        }
        // TYPE=0x13, reqId=0 is specifically the session handshake's reply
        // (see Mds::literalSessionHandshakeRequest()).
        if (frame.type == 0x13 && frame.requestId == 0) {
            if (!m_handshakeAcked) {
                m_handshakeAcked = true;
                emit readyChanged(true);
            }
            // A second one is not an error worth shouting about - and it
            // must not fall through, which is what used to crash the whole
            // phone. See the reqId-0 note below.
            continue;
        }

        // Bulk-transfer chunks (fetchLogbookData()) - unsolicited, like the
        // handshake reply, so matched the same way (type+reqId) rather than
        // through the request queue's m_inFlightRequestId.
        if (frame.type == 0x01 && frame.requestId == 0) {
            if (m_bulkFetchActive) {
                appendBulkChunk(frame.body);
                armBulkSilenceTimer();
            }
            continue;
        }

        // **Nothing in flight resolves nothing.** This used to read only
        // `frame.requestId != m_inFlightRequestId`, which is correct for
        // every request this client makes - get() numbers them from 1 - and
        // wrong for reqId 0, because an idle client's m_inFlightRequestId
        // *is* 0. Any unsolicited reqId-0 frame therefore passed the test,
        // reached resolveInFlight() with no callback set, and threw
        // std::bad_function_call - which aborts the process, and on this
        // device an aborting GL client takes the compositor down with it, so
        // the whole UI restarted. Caught with gdb; see docs/notifications.md.
        //
        // The watch sends plenty of reqId-0 traffic: the handshake reply
        // above, bulk chunks, and its own questions at the start of every
        // connection.
        if (m_inFlightRequestId == 0 || frame.requestId != m_inFlightRequestId)
            continue; // a response we already timed out on, or the watch talking unprompted
        resolveInFlight(frame.requestId, true, frame, QString());
    }
}

void MdsWhiteboardClient::resolveInFlight(uint16_t requestId, bool ok, const Mds::Frame &frame,
                                           const QString &error)
{
    // Both halves matter. The id check catches a timeout firing after the
    // real response arrived; the zero check catches there being nothing in
    // flight at all, which is the state an idle client is in and which an
    // unsolicited reqId-0 frame used to walk straight into.
    if (m_inFlightRequestId == 0 || requestId != m_inFlightRequestId)
        return;

    const ResponseCallback callback = m_inFlightCallback;
    m_inFlightRequestId = 0;
    m_inFlightCallback = nullptr;
    // Belt as well as braces: an in-flight id without a callback is a bug
    // somewhere above, and calling an empty std::function aborts the
    // process. Returning quietly is wrong but survivable; aborting is not.
    if (!callback) {
        qWarning() << "a request was in flight with no callback - ignoring its response";
        dispatchNext();
        return;
    }
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
    readPages([ackBody](uint16_t requestId, uint32_t at) {
        return Mds::encodePagedReadRequest(requestId, ackBody, at);
    }, offset, collected, callback);
}

void MdsWhiteboardClient::readPages(PageRequestEncoder encoder, uint32_t offset,
                                     std::vector<uint8_t> collected, DataCallback callback)
{
    getRaw([encoder, offset](uint16_t requestId) {
        return encoder(requestId, offset);
    }, [this, encoder, offset, collected, callback]
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
        readPages(encoder, offset + static_cast<uint32_t>(payloadSize), collected, callback);
    });
}

void MdsWhiteboardClient::fetchRecoveryMoments(qint64 newerThanSeconds, DataCallback callback)
{
    const QString path = QStringLiteral("/Activity/Moments/Sync/Data");
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, newerThanSeconds, callback]
            (bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, {}, tr("GET %1 acked with an unusably short body").arg(path));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        std::vector<uint8_t> since(8);
        for (int i = 0; i < 8; ++i) {
            since[i] = static_cast<uint8_t>(
                    (static_cast<uint64_t>(newerThanSeconds) >> (8 * i)) & 0xFF);
        }
        readPages([ackBody, since](uint16_t requestId, uint32_t) {
            // The offset is carried by the cursor parameter rather than a
            // separate one, so every page repeats the same request; the
            // paging loop's own offset is unused here.
            return Mds::encodeParameterisedFetch(requestId, ackBody,
                                                   { { Mds::kParamInt64, since } });
        }, 0, {}, callback);
    });
}

void MdsWhiteboardClient::readValue(const QString &path, DataCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        // f5 in six bytes or fewer is this protocol's rejection - a
        // resource this watch does not have - not a short value. Confirmed
        // against two non-existent resources, see
        // docs/watch-push-resources.md.
        if (ack.body.size() <= 6
                && (ack.body.empty() || ack.body[0] == 0xF5)) {
            callback(false, ack.body, tr("not on this watch"));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody](uint16_t requestId) {
            return Mds::encodeParameterisedFetch(requestId, ackBody, {});
        }, [path, callback](bool fetchOk, const Mds::Frame &frame, const QString &fetchError) {
            if (!fetchOk) {
                callback(false, {}, tr("Reading %1 failed: %2").arg(path, fetchError));
                return;
            }
            callback(true, frame.body, QString());
        });
    });
}

void MdsWhiteboardClient::fetchWithCursor(const QString &path, uint16_t typeCode, qint64 value,
                                           DataCallback callback)
{
    const size_t width = (typeCode == Mds::kParamInt32) ? 4 : 8;
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, typeCode, value, width, callback]
            (bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, {}, tr("GET %1 acked with an unusably short body").arg(path));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        std::vector<uint8_t> cursor(width);
        for (size_t i = 0; i < width; ++i)
            cursor[i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8 * i)) & 0xFF);

        getRaw([ackBody, typeCode, cursor](uint16_t requestId) {
            return Mds::encodeParameterisedFetch(requestId, ackBody, { { typeCode, cursor } });
        }, [path, callback](bool fetchOk, const Mds::Frame &frame, const QString &fetchError) {
            if (!fetchOk) {
                callback(false, {}, tr("Fetch on %1 failed: %2").arg(path, fetchError));
                return;
            }
            callback(true, frame.body, QString());
        });
    });
}

namespace {

// The watch's fragment marker. 200 is the last fragment, 202 means there
// is more - the opposite convention to the 100/200 every other paged
// resource here uses, which is exactly the kind of difference that would
// otherwise show up as a short answer looking like a complete one.
constexpr uint16_t kActivityStatusMore = 202;
constexpr int kActivityStatusOffset = 6;

// Two weeks of ten-minute buckets at roughly ten per fragment. The cap is
// a guard against a watch that never stops saying 202, not a real limit on
// how much history can be read.
constexpr int kMaxActivityFragments = 250;

} // namespace

void MdsWhiteboardClient::fetchActivityTrend(qint64 newerThanMs, ActivityCallback callback)
{
    fetchActivityFragment(newerThanMs, {}, 0, callback);
}

void MdsWhiteboardClient::fetchActivityFragment(qint64 cursorMs,
                                                 std::vector<ActivityTrend::Sample> collected,
                                                 int fragments, ActivityCallback callback)
{
    const QString path = QStringLiteral("/Activity/TrendData");
    fetchWithCursor(path, Mds::kParamInt64, cursorMs,
            [this, cursorMs, collected, fragments, callback, path]
            (bool ok, const std::vector<uint8_t> &body, const QString &error) mutable {
        if (!ok) {
            // Whatever was already read is worth keeping - this is a
            // fragment loop, and losing eleven good fragments because the
            // twelfth timed out helps nobody.
            if (collected.empty())
                callback(false, {}, error);
            else
                callback(true, collected, QString());
            return;
        }

        const std::vector<ActivityTrend::Sample> fragment = ActivityTrend::decode(body);
        uint16_t status = 0;
        if (body.size() >= static_cast<size_t>(kActivityStatusOffset) + 2) {
            status = static_cast<uint16_t>(body[kActivityStatusOffset]
                    | (static_cast<uint16_t>(body[kActivityStatusOffset + 1]) << 8));
        }

        for (const ActivityTrend::Sample &sample : fragment)
            collected.push_back(sample);

        // The cursor is a timestamp, not a byte offset, so "no progress"
        // is the loop's real stop condition: an empty fragment, a watch
        // that stopped saying 202, or a cursor that didn't move.
        const qint64 next = fragment.empty()
                ? cursorMs
                : fragment.back().rawTimestampMs + 1;
        if (status != kActivityStatusMore || fragment.empty() || next <= cursorMs
                || fragments + 1 >= kMaxActivityFragments) {
            callback(true, collected, QString());
            return;
        }

        fetchActivityFragment(next, collected, fragments + 1, callback);
    });
}

namespace {
// What the official app sends. Nothing in the framing requires it - each
// chunk declares its own length - but staying with the observed value
// keeps this as close to a replay as it can be.
constexpr size_t kEphemerisChunkSize = 453;
// What the watch answers to each chunk's PUT, in the status halfword at
// body offset 6 - the same 100-means-more/200-means-last convention the
// paged reads use, confirmed across all 136 captured chunks. The commit
// answers 202 Accepted, which is the watch saying it has taken the data
// and not that it has finished with it.
constexpr uint16_t kPutStatusContinue = 100;
constexpr uint16_t kPutStatusOk = 200;
constexpr uint16_t kPutStatusAccepted = 202;

uint16_t statusOf(const Mds::Frame &frame)
{
    if (frame.body.size() < 8)
        return 0;
    return static_cast<uint16_t>(frame.body[6] | (static_cast<uint16_t>(frame.body[7]) << 8));
}

constexpr const char *kEphemerisUploadPath = "/Device/GNSS/ExtendedEphemerisData/Upload/0";
constexpr const char *kEphemerisLoadPath = "/Device/GNSS/ExtendedEphemerisData/Load";
} // namespace

void MdsWhiteboardClient::putEphemeris(const std::vector<uint8_t> &blob,
                                        ProgressCallback progress, SimpleCallback callback)
{
    if (blob.empty()) {
        callback(false, tr("No ephemeris data to send"));
        return;
    }

    const QString uploadPath = QString::fromLatin1(kEphemerisUploadPath);
    getRaw([uploadPath](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, uploadPath.toStdString());
    }, [this, blob, progress, callback, uploadPath]
            (bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, tr("GET %1 failed: %2").arg(uploadPath, error));
            return;
        }
        if (ack.body.size() < 6 || ack.body[0] == 0xF5) {
            callback(false, tr("This watch does not accept an ephemeris upload"));
            return;
        }

        const std::vector<uint8_t> ackBody = ack.body;
        const auto shared = std::make_shared<const std::vector<uint8_t>>(blob);
        // The "begin": a PUT with no parameters at all, on the same handle.
        getRaw([ackBody](uint16_t requestId) {
            return Mds::encodePut(requestId, ackBody, {});
        }, [this, ackBody, shared, progress, callback]
                (bool beginOk, const Mds::Frame &, const QString &beginError) {
            if (!beginOk) {
                callback(false, tr("The watch refused to start the upload: %1").arg(beginError));
                return;
            }
            sendEphemerisChunk(ackBody, shared, 0, progress, callback);
        });
    });
}

void MdsWhiteboardClient::sendEphemerisChunk(
        const std::vector<uint8_t> &ackBody,
        const std::shared_ptr<const std::vector<uint8_t>> &blob, size_t offset,
        ProgressCallback progress, SimpleCallback callback)
{
    if (offset >= blob->size()) {
        commitEphemeris(callback);
        return;
    }

    const size_t length = std::min(kEphemerisChunkSize, blob->size() - offset);
    const size_t through = offset + length;
    const uint32_t total = static_cast<uint32_t>(blob->size());

    getRaw([ackBody, blob, offset, length, through, total](uint16_t requestId) {
        return Mds::encodeEphemerisChunk(requestId, ackBody, total,
                                           static_cast<uint32_t>(through),
                                           blob->data() + offset, length);
    }, [this, ackBody, blob, through, progress, callback]
            (bool ok, const Mds::Frame &frame, const QString &error) {
        if (!ok) {
            callback(false, tr("Upload stopped after %1 of %2 bytes: %3")
                              .arg(through).arg(blob->size()).arg(error));
            return;
        }
        // The watch says 100 while it wants more and 200 on the last one.
        // Anything else is a refusal, and saying which byte it happened at
        // is the difference between a bug report and a shrug.
        const uint16_t status = statusOf(frame);
        if (status != kPutStatusContinue && status != kPutStatusOk) {
            callback(false, tr("The watch rejected the upload at %1 of %2 bytes (status %3)")
                              .arg(through).arg(blob->size()).arg(status));
            return;
        }
        if (progress)
            progress(static_cast<int>(through), static_cast<int>(blob->size()));
        sendEphemerisChunk(ackBody, blob, through, progress, callback);
    });
}

void MdsWhiteboardClient::commitEphemeris(SimpleCallback callback)
{
    const QString loadPath = QString::fromLatin1(kEphemerisLoadPath);
    getRaw([loadPath](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, loadPath.toStdString());
    }, [this, loadPath, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, tr("Data sent, but GET %1 failed: %2").arg(loadPath, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, tr("Data sent, but %1 acked oddly").arg(loadPath));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody](uint16_t requestId) {
            return Mds::encodePut(requestId, ackBody, {});
        }, [callback](bool putOk, const Mds::Frame &frame, const QString &putError) {
            if (!putOk) {
                callback(false, tr("Data sent, but the watch would not load it: %1")
                                  .arg(putError));
                return;
            }
            const uint16_t status = statusOf(frame);
            if (status != kPutStatusOk && status != kPutStatusAccepted) {
                callback(false, tr("Data sent, but the watch refused to load it (status %1)")
                                  .arg(status));
                return;
            }
            callback(true, QString());
        });
    });
}

void MdsWhiteboardClient::deleteWatchFile(const QString &filename, SimpleCallback callback)
{
    // Two steps, as captured: GET /Dev/FileSystem/FileDelete for a handle,
    // then PUT the filename through it.
    putString(QStringLiteral("/Dev/FileSystem/FileDelete"), filename, callback);
}

void MdsWhiteboardClient::putString(const QString &path, const QString &value,
                                     SimpleCallback callback)
{
    const std::string text = value.toStdString();
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, text, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, tr("GET %1 acked with an unusably short body").arg(path));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody, text](uint16_t requestId) {
            return Mds::encodePutString(requestId, ackBody, text);
        }, [path, callback](bool putOk, const Mds::Frame &, const QString &putError) {
            if (!putOk) {
                callback(false, tr("PUT %1 failed: %2").arg(path, putError));
                return;
            }
            callback(true, QString());
        });
    });
}

void MdsWhiteboardClient::putSmallEnum(const QString &path, quint8 value,
                                        SimpleCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, value, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, tr("GET %1 acked with an unusably short body").arg(path));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody, value](uint16_t requestId) {
            return Mds::encodePut(requestId, ackBody,
                                   { { Mds::kParamSmallEnum, { value } } });
        }, [path, callback](bool putOk, const Mds::Frame &, const QString &putError) {
            if (!putOk) {
                callback(false, tr("PUT %1 failed: %2").arg(path, putError));
                return;
            }
            callback(true, QString());
        });
    });
}

void MdsWhiteboardClient::putEmpty(const QString &path, SimpleCallback callback)
{
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, tr("GET %1 acked with an unusably short body").arg(path));
            return;
        }
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody](uint16_t requestId) {
            return Mds::encodePut(requestId, ackBody, {});
        }, [path, callback](bool putOk, const Mds::Frame &, const QString &putError) {
            if (!putOk) {
                callback(false, tr("PUT %1 failed: %2").arg(path, putError));
                return;
            }
            callback(true, QString());
        });
    });
}

void MdsWhiteboardClient::fetchTimelineFile(const QString &resourcePath,
                                             const QString &filename,
                                             qint64 newerThanMs, DataCallback callback)
{
    const std::string name = filename.toStdString();

    // Step 1: GET the timeline resource.
    getRaw([resourcePath](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, resourcePath.toStdString());
    }, [this, resourcePath, name, newerThanMs, callback]
            (bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, {}, tr("GET %1 failed: %2").arg(resourcePath, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, {}, tr("GET %1 acked with an unusably short body").arg(resourcePath));
            return;
        }

        // Step 2: ask the watch to render the file.
        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody, newerThanMs, name](uint16_t requestId) {
            return Mds::encodeTimelineFileFetch(requestId, ackBody,
                                                 static_cast<int64_t>(newerThanMs), name);
        }, [this, name, callback](bool renderOk, const Mds::Frame &, const QString &renderError) {
            if (!renderOk) {
                callback(false, {}, tr("Timeline render request failed: %1").arg(renderError));
                return;
            }

            // Step 3: open the filesystem stream and page the file off.
            getRaw([](uint16_t requestId) {
                return Mds::encodeGetRequest(requestId, "/Dev/FileSystem/Stream");
            }, [this, name, callback]
                    (bool streamOk, const Mds::Frame &streamAck, const QString &streamError) {
                if (!streamOk) {
                    callback(false, {}, tr("GET /Dev/FileSystem/Stream failed: %1")
                                          .arg(streamError));
                    return;
                }
                if (streamAck.body.size() < 6) {
                    callback(false, {}, tr("Filesystem stream acked with a short body"));
                    return;
                }
                const std::vector<uint8_t> streamHandle = streamAck.body;
                readPages([streamHandle, name](uint16_t requestId, uint32_t at) {
                    return Mds::encodeFileReadRequest(requestId, streamHandle, name, at);
                }, 0, {},
                [this, name, callback](bool readOk, const std::vector<uint8_t> &data,
                                        const QString &readError) {
                    // Step 4: delete the rendered file, as the official app
                    // does. Best effort - the data is already in hand, so a
                    // failed cleanup must not fail the fetch. The watch
                    // overwrites the file next time either way.
                    deleteWatchFile(QString::fromStdString(name),
                            [callback, readOk, data, readError](bool, const QString &) {
                        callback(readOk, data, readError);
                    });
                });
            });
        });
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
    // Guarded for the same reason resolveInFlight() is: a member callback
    // that is empty when it should not be is a bug, and calling it aborts
    // the process rather than reporting one.
    if (callback)
        callback(ok, data, error);
}

namespace {
// The Add and Del resources, each with its own handle. Naming them here
// keeps the one place that writes to a watch's notification queue narrow -
// the same reason putString() makes callers name a path.
constexpr const char *kNotificationAddPath = "/Device/Connectivity/Ble/Ancs/Notification/Add";
constexpr const char *kNotificationDelPath = "/Device/Connectivity/Ble/Ancs/Notification/Del";

// The watch's answer to a notification PUT. 200 on both an add and a
// removal that found something; 403 on removing an id it does not have,
// which a capture measured rather than a document promised.
constexpr uint16_t kNotificationOk = 200;
constexpr uint16_t kNotificationNotFound = 403;
} // namespace

void MdsWhiteboardClient::sendNotification(const Ancs::Notification &notification,
                                            DeliveryCallback callback)
{
    const QString path = QString::fromLatin1(kNotificationAddPath);
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, notification, callback](bool ok, const Mds::Frame &ack, const QString &error) {
        if (!ok) {
            callback(false, false, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            // An answer, just an unusable one: asking again would get the
            // same short body.
            callback(false, true, tr("The watch acked %1 with an unusably short body").arg(path));
            return;
        }

        // The encoder refuses a notification longer than the structure's
        // one-byte length can describe. Checking here keeps that from
        // becoming an exception thrown inside a BLE callback - callers are
        // expected to have run Ancs::truncateToFit() already.
        if (Ancs::encodedSize(notification) > Ancs::maximumEncodedSize()) {
            callback(false, true,
                      tr("The notification is %1 bytes, and the watch takes at most %2")
                              .arg(static_cast<int>(Ancs::encodedSize(notification)))
                              .arg(static_cast<int>(Ancs::maximumEncodedSize())));
            return;
        }

        const std::vector<uint8_t> ackBody = ack.body;
        getRaw([ackBody, notification](uint16_t requestId) {
            return Ancs::encodeAdd(requestId, ackBody, notification);
        }, [callback](bool putOk, const Mds::Frame &reply, const QString &putError) {
            if (!putOk) {
                callback(false, false,
                          tr("The watch did not answer the notification: %1").arg(putError));
                return;
            }
            const uint16_t status = statusOf(reply);
            if (status != kNotificationOk) {
                callback(false, true,
                          tr("The watch refused the notification (status %1)").arg(status));
                return;
            }
            callback(true, true, QString());
        });
    });
}

// The removal itself, once the path's ack body is in hand.
void MdsWhiteboardClient::putRemoval(quint32 notificationId, bool mayRetry,
                                      DeliveryCallback callback)
{
    const std::vector<uint8_t> ackBody = m_removalAck;
    getRaw([ackBody, notificationId](uint16_t requestId) {
        return Ancs::encodeRemove(requestId, ackBody, notificationId);
    }, [this, notificationId, mayRetry, callback](bool putOk, const Mds::Frame &reply,
                                                   const QString &putError) {
        if (!putOk) {
            // The cached registration is the first suspect, since its
            // lifetime is unknown: drop it and let one retry register the
            // path afresh. Only one, so a watch that is simply not
            // answering cannot turn each removal into two timeouts.
            m_removalAck.clear();
            if (mayRetry) {
                qWarning() << "removal did not answer; re-registering the path and retrying";
                removeNotification(notificationId, callback);
                return;
            }
            callback(false, false, tr("The watch did not answer the removal: %1").arg(putError));
            return;
        }
        const uint16_t status = statusOf(reply);
        if (status == kNotificationNotFound) {
            // The useful case to get right: it is gone, which is what was
            // wanted. An answer, and nothing to try again.
            callback(false, true, tr("The watch has no notification with that id"));
            return;
        }
        if (status != kNotificationOk) {
            callback(false, true, tr("The watch refused the removal (status %1)").arg(status));
            return;
        }
        callback(true, true, QString());
    });
}

void MdsWhiteboardClient::removeNotification(quint32 notificationId, DeliveryCallback callback)
{
    // Already registered in this session: go straight to the removal. This is
    // the whole point - see m_removalAck for what the watch does to a second
    // registration of the same path.
    if (m_removalAck.size() >= 6) {
        // The one place a stale handle is possible, so this is the attempt
        // allowed to fall back to registering again.
        putRemoval(notificationId, true, callback);
        return;
    }

    const QString path = QString::fromLatin1(kNotificationDelPath);
    getRaw([path](uint16_t requestId) {
        return Mds::encodeGetRequest(requestId, path.toStdString());
    }, [this, path, notificationId, callback](bool ok, const Mds::Frame &ack,
                                               const QString &error) {
        if (!ok) {
            callback(false, false, tr("GET %1 failed: %2").arg(path, error));
            return;
        }
        if (ack.body.size() < 6) {
            callback(false, true, tr("The watch acked %1 with an unusably short body").arg(path));
            return;
        }

        m_removalAck = ack.body;
        // Freshly registered, so a failure here is not a stale handle and
        // retrying would only register again - which is the thing the watch
        // refuses.
        putRemoval(notificationId, false, callback);
    });
}
