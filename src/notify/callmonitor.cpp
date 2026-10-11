#include "callmonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDebug>
#include <QVariant>

namespace {

const char *const kService = "org.nemomobile.voicecall";
const char *const kCallInterface = "org.nemomobile.voicecall.VoiceCall";

// The manager also publishes the current call under this fixed path, so a
// statusChanged can arrive twice - once on the call's own object and once on
// the alias.
//
// **Can, not does.** Measured across three captures of real calls: one had
// `incoming` on both paths and two on the concrete path only, while all
// three had the end on both. So the duplication cannot be relied on, which
// is why the two cases are handled differently below: a ring is taken from
// the concrete path only, so the watch cannot be rung twice, but an end is
// accepted from either.
//
// To be exact about what that second half is: every capture so far has had
// the end on the concrete path too, so the alias branch has never been the
// thing that noticed. It is there because an end that arrived only on the
// alias would otherwise be thrown away and leave the ring on the watch for
// ever - and a ring was twice seen to stay, without the captures ever
// catching the call that did it. The real guard against that is the timeout
// in NotifierDaemon; this is the cheap half.
const char *const kActiveAlias = "/calls/active";

// The status text, not the number. Both are in the signal, and the text is
// the one that cannot be misread: the numbers were observed as 5 "incoming",
// 7 "disconnected" and 0 "null", which is three of an enum this project has
// never seen a definition of.
const char *const kIncoming = "incoming";

// The property that holds the caller's number. Confirmed by reading the
// symbols out of the device's own libvoicecall.so.1.0.0 - lineId, alongside
// isIncoming, isForwarded, statusText - rather than guessed from the shape of
// the interface.
const char *const kLineIdProperty = "lineId";

} // namespace

CallMonitor::CallMonitor(QObject *parent)
    : QObject(parent)
{
}

bool CallMonitor::start(QString *error)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        if (error)
            *error = QStringLiteral("no session bus: %1").arg(bus.lastError().message());
        return false;
    }

    // An empty path matches every object, which is what is wanted: a call's
    // object path contains a fresh identifier each time, so there is nothing
    // to subscribe to specifically. The signature is given explicitly so a
    // future statusChanged with different arguments does not silently bind to
    // this slot.
    const bool ok = bus.connect(QString::fromLatin1(kService), QString(),
                                 QString::fromLatin1(kCallInterface),
                                 QStringLiteral("statusChanged"), QStringLiteral("is"), this,
                                 SLOT(onStatusChanged(int, QString, QDBusMessage)));
    if (!ok) {
        if (error)
            *error = QStringLiteral("could not subscribe to call status changes");
        return false;
    }
    qInfo().noquote() << "watching for incoming calls on" << kCallInterface;
    return true;
}

void CallMonitor::onStatusChanged(int status, const QString &statusText,
                                  const QDBusMessage &message)
{
    const QString path = message.path();
    const bool alias = path == QLatin1String(kActiveAlias);
    const bool ringing = statusText.compare(QLatin1String(kIncoming), Qt::CaseInsensitive) == 0;

    if (!ringing) {
        // Anything that is not ringing ends the ring, including being
        // answered: the watch should stop showing an incoming call the moment
        // it stops being one.
        //
        // The alias ends whatever is ringing rather than a path of its own,
        // because it does not say which call it speaks for - and in practice
        // only one call rings at a time. Emptying the set as it goes means
        // the concrete path arriving afterwards finds nothing left and emits
        // nothing, so the watch is never asked to remove the same
        // notification twice.
        if (alias) {
            const QSet<QString> ending = m_ringing;
            m_ringing.clear();
            m_pending.clear();
            for (const QString &ringingPath : ending) {
                qInfo().noquote()
                        << QStringLiteral("call %1 is now %2 (%3), seen on the alias "
                                           "- clearing the watch")
                                   .arg(ringingPath, statusText)
                                   .arg(status);
                emit callEnded(ringingPath);
            }
            return;
        }

        if (m_pending.remove(path))
            m_abandoned.insert(path);
        if (m_ringing.remove(path)) {
            qInfo().noquote() << QStringLiteral("call %1 is now %2 (%3) - clearing the watch")
                                         .arg(path, statusText)
                                         .arg(status);
            emit callEnded(path);
        }
        return;
    }

    // A ring is only ever taken from the call's own object. The alias would
    // ring the watch a second time for the same call.
    if (alias)
        return;

    if (m_ringing.contains(path) || m_pending.contains(path))
        return;

    // The number comes from a property read, which is a second round trip -
    // done asynchronously because a daemon that blocks on D-Bus while a
    // phone is ringing is a daemon that stops answering its own bus.
    m_pending.insert(path);
    QDBusMessage request = QDBusMessage::createMethodCall(
            QString::fromLatin1(kService), path,
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    request << QString::fromLatin1(kCallInterface) << QString::fromLatin1(kLineIdProperty);

    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(request);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    watcher->setProperty("callPath", path);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &CallMonitor::onLineIdReady);
}

void CallMonitor::onLineIdReady()
{
    QDBusPendingCallWatcher *watcher = qobject_cast<QDBusPendingCallWatcher *>(sender());
    if (!watcher)
        return;
    watcher->deleteLater();

    const QString path = watcher->property("callPath").toString();
    m_pending.remove(path);

    // Ended while the property read was in flight.
    if (m_abandoned.remove(path)) {
        qInfo().noquote() << QStringLiteral("call %1 ended before its number arrived").arg(path);
        return;
    }

    QDBusPendingReply<QVariant> reply = *watcher;
    QString lineId;
    if (reply.isError()) {
        // Not a reason to stay silent: a call with no number is still a call,
        // and "Unknown caller" is a better answer than nothing.
        qWarning().noquote() << QStringLiteral("could not read %1 for %2: %3")
                                        .arg(QString::fromLatin1(kLineIdProperty), path,
                                              reply.error().message());
    } else {
        QVariant value = reply.value();
        // Properties.Get answers with a variant inside a variant. QtDBus
        // usually unwraps it; when it does not, the outer one stringifies to
        // nothing at all, which would read as a withheld number.
        if (value.canConvert<QDBusVariant>())
            value = value.value<QDBusVariant>().variant();
        lineId = value.toString();
    }

    m_ringing.insert(path);
    qInfo().noquote() << QStringLiteral("incoming call on %1 from %2")
                                 .arg(path, lineId.isEmpty() ? QStringLiteral("(withheld)")
                                                              : lineId);
    emit incomingCall(path, lineId);
}
