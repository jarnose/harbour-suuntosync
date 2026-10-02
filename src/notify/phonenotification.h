#pragma once

#include <QString>
#include <QStringList>

// One notification as the phone's own notification server saw it, before
// anything watch-specific is decided about it.
//
// The field names follow org.freedesktop.Notifications' Notify call, whose
// signature is susssasa{sv}i, plus the three Sailfish-specific hints that
// are where the text a user actually reads tends to live.
struct PhoneNotification
{
    QString appName;        // Notify's app_name - often a binary name
    QString appIcon;
    QString summary;
    QString body;
    QStringList actions;    // id, label, id, label, ...

    // Sailfish's own hints. x-nemo-owner names the package, which is a
    // better appId than app_name; the two preview hints carry the text the
    // lock screen shows, which is sometimes the only text there is.
    QString owner;
    QString previewSummary;
    QString previewBody;
    QString category;       // freedesktop "category" hint, e.g. x-nemo.messaging.im

    quint32 replacesId = 0;
    int expireTimeout = -1;

    // The serial of the Notify call, used to recognise the reply that
    // carries the id the server assigned. Not useful afterwards.
    quint32 serial = 0;
    // Filled in from that reply. Zero until it arrives.
    quint32 id = 0;

    // The best title and text available: the preview hints when present,
    // the plain summary and body otherwise.
    QString title() const { return previewSummary.isEmpty() ? summary : previewSummary; }
    QString text() const { return previewBody.isEmpty() ? body : previewBody; }
};
