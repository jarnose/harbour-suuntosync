#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class QDBusMessage;

// Watches the session bus for an incoming call.
//
// This exists because an incoming call is **not a notification** on Sailfish,
// which is why the daemon could tell the watch about a missed call and not
// about the call it missed. Measured on the device: a call arriving raises
//
//   path=/calls/<hex>  org.nemomobile.voicecall.VoiceCall.statusChanged
//     int32 5
//     string "incoming"
//
// followed by VoiceCallManager.playRingtone, and the first Notify of the
// whole exchange arrives ten seconds later - when the call has already been
// missed. There is no `x-nemo.call.incoming` notification category on the
// device at all, only `missed`.
//
// So the ringing has to be watched where it actually happens. Ordinary
// signals, unlike the notification half: these are broadcasts, so plain
// QtDBus receives them and no monitor connection is needed.
class CallMonitor : public QObject
{
    Q_OBJECT

public:
    explicit CallMonitor(QObject *parent = nullptr);

    // False and a reason when the session bus is not there to connect to.
    bool start(QString *error);

signals:
    // Once per call that starts ringing. `lineId` is the caller's number, and
    // is empty when withheld - the receiver decides what to show for that.
    void incomingCall(const QString &callPath, const QString &lineId);
    // Once per call previously announced, whether it was answered, rejected
    // or missed. All three mean the same thing here: stop ringing on the
    // watch. A missed call then arrives separately as a real notification,
    // which is the right division - the ring is transient, the miss is not.
    void callEnded(const QString &callPath);

private slots:
    void onStatusChanged(int status, const QString &statusText, const QDBusMessage &message);
    void onLineIdReady();

private:
    // Calls announced and not yet ended, so callEnded() is never emitted for
    // a call nobody was told about - an outgoing call runs through the same
    // signal and must stay silent.
    QSet<QString> m_ringing;
    // Paths with a lineId lookup in flight, so a second statusChanged for the
    // same call cannot start a second one.
    QSet<QString> m_pending;
    // Calls that ended while their number was still being looked up - a
    // rejected call is a second or two, and announcing one after it is over
    // would leave a ring on the watch that nothing is going to clear.
    QSet<QString> m_abandoned;
};
