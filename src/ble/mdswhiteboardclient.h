#pragma once

#include "mdswirecodec.h"

#include <QObject>
#include <QQueue>
#include <QString>
#include <functional>

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
    // Sends the next queued request if none is currently in flight. No-op
    // if the queue is empty or a request is already outstanding.
    void dispatchNext();
    // Resolves the in-flight request (if its requestId still matches - a
    // request already resolved before its timeout fires is a safe no-op)
    // and advances to the next queued one.
    void resolveInFlight(uint16_t requestId, bool ok, const Mds::Frame &frame,
                          const QString &error);

    QString m_deviceObjectPath;
    QString m_servicePath;
    QString m_writeCharPath;
    QString m_notifyCharPath;

    Mds::Decoder m_decoder;
    uint16_t m_nextRequestId = 1;
    bool m_handshakeAcked = false;

    struct QueuedRequest
    {
        QString path;
        ResponseCallback callback;
    };
    QQueue<QueuedRequest> m_queue;
    uint16_t m_inFlightRequestId = 0; // 0 = none (real request IDs start at 1)
    ResponseCallback m_inFlightCallback;
};
