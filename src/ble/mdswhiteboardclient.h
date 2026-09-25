#pragma once

#include "activitydecoder.h"
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

    // The same two-step exchange fetchLogEntries() uses - GET, then a
    // TYPE=0x0D handle fetch built from the ack - but handing back the raw
    // response body instead of running it through LogEntries::decode().
    //
    // For probing resources whose structure isn't known yet:
    // /Sleep/<serial>/Entries and /Activity/<serial>/Entries are real
    // resources (they're in the APK's string table, see
    // docs/watch-push-resources.md) but nothing here knows what they reply
    // with. Decoding them as logbook entries would either fail or, worse,
    // succeed with nonsense.
    void fetchStructuredRaw(const QString &path, DataCallback callback);

    // Reads one of the watch's rendered timeline files - how sleep and
    // daily activity actually come off the device (docs/watch-push-
    // resources.md). Three steps, all confirmed against the 2026-09-23
    // capture:
    //
    //   1. GET `resourcePath` (e.g. "/Daily/Sleep/Timeline/Data")
    //   2. a parameterised handle fetch carrying `newerThanMs` and
    //      `filename` - this is what makes the watch render the file
    //   3. GET /Dev/FileSystem/Stream, then paged reads of `filename` at
    //      increasing offsets until the watch stops saying "continue"
    //
    // The payload is an SBEM container, but version 0102 rather than the
    // 0103 a workout carries - close, and deliberately not assumed
    // identical.
    //
    // The rendered file is deleted afterwards, as the official app does.
    // That cleanup is best effort: the data is already in hand by then, so
    // a failed delete does not fail the fetch.
    void fetchTimelineFile(const QString &resourcePath, const QString &filename,
                            qint64 newerThanMs, DataCallback callback);

    // Writes a string to a watch resource: GET the path for a handle, then
    // PUT through it. This is how the official app configures the watch -
    // see docs/watch-push-resources.md.
    //
    // Deliberately narrow. A write to the wrong resource on someone's
    // watch is not something to offer a generic API for, so callers name
    // the path explicitly and there is no "write anything anywhere" entry
    // point.
    using SimpleCallback = std::function<void(bool ok, const QString &error)>;
    void putString(const QString &path, const QString &value, SimpleCallback callback);

    // A PUT with no value - used for the "do this now" resources, and for
    // deleting a rendered file once its name has been written.
    void putEmpty(const QString &path, SimpleCallback callback);

    // Removes a file the watch rendered for us. fetchTimelineFile() calls
    // this itself once the data is read.
    void deleteWatchFile(const QString &filename, SimpleCallback callback);

    // The recovery series from /Activity/Moments/Sync/Data. Unlike sleep
    // this needs no rendered file - the reply carries the records directly,
    // in the same paged framing /Summary uses.
    //
    // `newerThanSeconds` is unix SECONDS: the captured request passes them
    // that way, where the sleep fetch passes milliseconds. Same parameter
    // type code, different unit, which is exactly the kind of thing worth
    // not guessing.
    void fetchRecoveryMoments(qint64 newerThanSeconds, DataCallback callback);

    // One GET plus one parameterised handle fetch carrying a single
    // integer cursor, handing back the reply body **with its paged header
    // still on the front**. Everything else here strips that header; this
    // one keeps it because a probe needs to read the status code in it,
    // which is how the watch says whether more fragments follow.
    //
    // `typeCode` is Mds::kParamInt32 or kParamInt64, and the unit of
    // `value` is the resource's business, not this function's: sleep
    // counts milliseconds, recovery counts seconds, and neither announces
    // which. Callers say what they mean.
    void fetchWithCursor(const QString &path, uint16_t typeCode, qint64 value,
                          DataCallback callback);

    // The daily-activity series from /Activity/TrendData - ten-minute
    // buckets of steps, energy and heart rate (docs/watch-push-
    // resources.md). Records come straight back in the reply, like
    // recovery and unlike sleep.
    //
    // Fragmented, and not in the way everything else here is: the watch
    // sends about ten records at a time and says **202** to mean "ask
    // again", where /Summary and the rest use 100. It is also not a byte
    // offset that advances - the next request carries a new timestamp
    // cursor, taken from the last record received - so this cannot go
    // through readPages() and has its own loop.
    //
    // `newerThanMs` is unix milliseconds. It is NOT a strict lower bound -
    // the one observed reply started fifty-four minutes before the cursor
    // it was given (docs/watch-push-resources.md) - so the caller may get
    // back buckets it already has. Harmless: health_entries upserts on
    // (kind, timestamp). What stops the loop is a cursor that stopped
    // advancing, not the watch promising anything about the window.
    using ActivityCallback = std::function<void(bool ok,
                                                 const std::vector<ActivityTrend::Sample> &samples,
                                                 const QString &error)>;
    void fetchActivityTrend(qint64 newerThanMs, ActivityCallback callback);

    // Fetches a paged resource such as "/Logbook/byId/<id>/Summary": the
    // ordinary GET, then repeated Mds::encodePagedReadRequest() reads at
    // increasing byte offsets until a page comes back marked "last" (see
    // that function's doc comment for the paging protocol). Calls back once
    // with the pages' payloads concatenated - an SBEM0103 container, ready
    // for Summary::decode(). Unlike fetchLogbookData() there is no bulk
    // stream and no Heatshrink layer involved.
    void fetchSummary(const QString &path, DataCallback callback);

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
    // One step of fetchSummary()'s page loop: reads at the given offset,
    // appends the payload to collected, and either recurses for the next
    // page or hands the whole thing to callback.
    // Shared by /Summary's paged reads and the timeline-file reads: the
    // reply framing is identical (19-byte header, 100 = continue,
    // 200 = last), only the request differs, so the encoder is a parameter.
    using PageRequestEncoder = std::function<std::vector<uint8_t>(uint16_t, uint32_t)>;
    // One step of fetchActivityTrend()'s fragment loop.
    void fetchActivityFragment(qint64 cursorMs, std::vector<ActivityTrend::Sample> collected,
                                int fragments, ActivityCallback callback);
    void readPages(PageRequestEncoder encoder, uint32_t offset,
                    std::vector<uint8_t> collected, DataCallback callback);

    void readNextPage(const std::vector<uint8_t> &ackBody, uint32_t offset,
                       std::vector<uint8_t> collected, DataCallback callback);

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
