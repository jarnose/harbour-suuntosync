#pragma once

#include "logentriesdecoder.h"
#include "mdswirecodec.h"

#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <functional>
#include <vector>

// Speaks the Movesense/Suunto Whiteboard protocol (see mdswirecodec.h for
// the wire format) over a BlueZ-connected device's GATT objects, using
// org.bluez.GattCharacteristic1's WriteValue/StartNotify - not Qt Bluetooth
// (unavailable on this SDK target, see BluezAdapter's header comment).
//
// BlueZ only creates GattService1/GattCharacteristic1 D-Bus objects once the
// device's Device1.ServicesResolved property becomes true, which can lag a
// moment behind Connect() finishing - attachToDevice() accounts for that by
// checking immediately and also listening for the property to flip.
//
// Requests are strictly serialized (one in flight at a time, FIFO queue) -
// confirmed necessary, not just cautious, by an independent reverse-
// engineering of this exact protocol (wylandplex/zappctl, docs/PROTOCOL.md,
// which also independently confirmed this project's GATT UUIDs and SLIP/CRC
// framing byte-for-byte): "Ack model: strictly 1:1 - send one frame, wait
// for exactly one notify, send the next."
class MdsWhiteboardClient : public QObject
{
    Q_OBJECT
public:
    using ResponseCallback = std::function<void(bool ok, const Mds::Frame &frame,
                                                  const QString &error)>;
    using DataCallback = std::function<void(bool ok, const std::vector<uint8_t> &data,
                                              const QString &error)>;
    using EntriesCallback = std::function<void(bool ok,
                                                 const std::vector<LogEntries::Entry> &entries,
                                                 const QString &error)>;

    explicit MdsWhiteboardClient(QObject *parent = nullptr);

    // Starts watching the given (already BlueZ Connect()ed) device for its
    // Whiteboard GATT service/characteristics to become available, and
    // subscribes to the notify characteristic once found. readyChanged()
    // fires once get() can actually be used.
    void attachToDevice(const QString &deviceObjectPath);
    void detach();

    // Gated on the session handshake completing (see
    // Mds::literalSessionHandshakeRequest()'s doc comment), not just on the
    // GATT characteristics being found - a first attempt at this without
    // the handshake sent every GET request correctly (confirmed via a real
    // HCI capture) and never got a single response back.
    bool isReady() const
    {
        return !m_writeCharPath.isEmpty() && !m_notifyCharPath.isEmpty() && m_handshakeAcked;
    }

    // Issues a Whiteboard GET request (e.g. path = "/Logbook/Entries") and
    // calls callback exactly once, either with the matching response Frame
    // (ok=true) or after a timeout with ok=false. Safe to call while another
    // get() is still outstanding - queued and sent strictly one at a time
    // (see the class comment on why).
    void get(const QString &path, ResponseCallback callback);

    // Experimental - see Mds::encodeStreamStartTrigger()'s doc comment for
    // exactly what's confirmed vs. not about this sequence, and
    // docs/logbook-data-format.md for the pipeline the result feeds into
    // (Logbook::decode(), src/ble/logbookdecoder.h). Fetches a paginated/
    // streamed resource such as "/Logbook/byId/<id>/Data": issues the
    // ordinary GET, and if the ack looks right, sends the TYPE=0x10 trigger
    // built from it, then collects the flood of TYPE=0x01/reqId=0
    // notifications that should follow - stripping each one's 28-byte MDS
    // header (see docs/logbook-data-format.md's Stage 1) and concatenating
    // the rest - until kBulkStreamSilenceMs passes with nothing new. Calls
    // callback exactly once with the assembled (still Heatshrink-
    // compressed) bytes, or ok=false if the GET/trigger step itself failed
    // or nothing at all was collected. Like get(), queued behind anything
    // else already in flight.
    void fetchLogbookData(const QString &path, DataCallback callback);

    // Experimental - see Mds::encodeEntriesFetchTrigger()'s doc comment for
    // exactly what's confirmed vs. not. Fetches a structured (non-bulk)
    // resource such as "/Logbook/Entries" by issuing the ordinary GET, then
    // - unlike fetchLogbookData()'s TYPE=0x10 stream trigger - a single
    // TYPE=0x0D handle-fetch request built from the ack, decoded via
    // LogEntries::decode() (src/ble/logentriesdecoder.h). This is the
    // shortcut that replaced the earlier (confirmed-not-working on real
    // hardware, see docs/logbook-data-format.md) attempt to reuse
    // fetchLogbookData()'s own stream-trigger mechanism for /Entries.
    void fetchLogEntries(const QString &path, EntriesCallback callback);

signals:
    void readyChanged(bool ready);
    void errorOccurred(const QString &message);

private slots:
    void onDevicePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                    const QStringList &invalidated);
    void onNotifyPropertiesChanged(const QString &interface, const QVariantMap &changed,
                                    const QStringList &invalidated);

private:
    void tryDiscoverGattObjects();
    void subscribeToNotify();
    void sendHandshake();
    // Writes a fully-framed message (as produced by Mds::encodeGetRequest()
    // or Mds::literalSessionHandshakeRequest()) to the write characteristic
    // in kWriteChunkSize-byte pieces. Returns false (with *error set) if any
    // chunk's D-Bus WriteValue call itself fails - a value making it onto
    // the air at all is not verified either way, "Write without Response"
    // has no ATT-level acknowledgement to check.
    bool writeChunked(const std::vector<uint8_t> &framed, QString *error);
    // Common to get()/fetchLogbookData()'s first two steps: queues a
    // request whose framed bytes are built lazily (once a requestId has
    // been assigned) rather than always via Mds::encodeGetRequest() - get()
    // is a thin wrapper around this.
    using FrameBuilder = std::function<std::vector<uint8_t>(uint16_t requestId)>;
    void getRaw(FrameBuilder builder, ResponseCallback callback);
    // Sends the next queued request if none is currently in flight. No-op
    // if the queue is empty or a request is already outstanding.
    void dispatchNext();
    // Resolves the in-flight request (if its requestId still matches - a
    // request already resolved before its timeout fires is a safe no-op)
    // and advances to the next queued one.
    void resolveInFlight(uint16_t requestId, bool ok, const Mds::Frame &frame,
                          const QString &error);
    // Strips a single TYPE=0x01/reqId=0 bulk chunk's 28-byte MDS header
    // (see docs/logbook-data-format.md's Stage 1 - MDS_HEADER_SIZE=28 from
    // the wire packet's start, i.e. this frame's *body* here since the
    // outer SYNC/TYPE/LEN/REQID envelope is already stripped by Mds::Frame,
    // so body offset 22) and appends the chunk_size-byte payload (LE u16 at
    // body offset 14) to m_bulkBuffer. Ignores a frame too short to contain
    // a full header, or whose claimed chunk_size doesn't fit.
    void appendBulkChunk(const std::vector<uint8_t> &body);
    // (Re)arms m_bulkSilenceTimer; when it fires with no further bulk chunk
    // having reset it, the fetch is considered complete.
    void armBulkSilenceTimer();
    // Sends the TYPE=0x11 stop for the stream just collected, then
    // finishes the fetch with the given result regardless of whether the
    // watch acked the stop - the collected data is what the caller asked
    // for, and a failed stop shouldn't throw it away. Sending this at all
    // is what makes a *second* fetch on the same connection possible; see
    // Mds::encodeStreamStopTrigger()'s doc comment.
    void endBulkStream(bool ok, const QString &error);
    void finishBulkFetch(bool ok, const QString &error);

    QString m_deviceObjectPath;
    QString m_servicePath;
    QString m_writeCharPath;
    QString m_notifyCharPath;

    Mds::Decoder m_decoder;
    uint16_t m_nextRequestId = 1;
    bool m_handshakeAcked = false;

    struct QueuedRequest
    {
        FrameBuilder builder;
        ResponseCallback callback;
    };
    QQueue<QueuedRequest> m_queue;
    uint16_t m_inFlightRequestId = 0; // 0 = none (real request IDs start at 1)
    ResponseCallback m_inFlightCallback;

    // Bulk-transfer collection state (fetchLogbookData()) - only one such
    // fetch can be active at a time, same "strictly serialized" reasoning
    // as the request queue above; a second call while one is active fails
    // immediately rather than queueing, since interleaving two bulk streams
    // isn't something this project has ever observed and guessing at how
    // it would behave isn't worth the risk.
    bool m_bulkFetchActive = false;
    std::vector<uint8_t> m_bulkBuffer;
    DataCallback m_bulkCallback;
    QTimer m_bulkSilenceTimer;
    // The GET ack the stream was started from, kept so the matching stop
    // can be built from it. Cleared once the stop has been sent, so a late
    // chunk re-arming the silence timer can't send a second one.
    std::vector<uint8_t> m_bulkAckBody;
};
