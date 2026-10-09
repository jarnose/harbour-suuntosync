#pragma once

#include <QHash>
#include <QObject>
#include <QString>

// Included rather than forward-declared: the namespace QtContacts lives in
// is itself a macro from this header, so there is nothing to declare without
// it. This class is daemon-only, so the include costs nothing that matters.
#include <QtContacts/QContactManager>

// Turns a phone number into the name the phone's own address book has for it.
//
// Two things about this are not obvious and both were learned by reading
// `piggz/libwatchfish`, which is what `harbour-amazfish` uses for the same
// job - see docs/notifications.md.
//
// **It needs a privilege, and the privilege lives in the RPM spec rather than
// in any code.** The address book is at
// `~/.local/share/system/privileged/Contacts/`, which is
// `drwxrwx--- privileged privileged`, and `defaultuser` is in neither. The
// daemon binary is therefore installed **setgid `privileged`** -
// `%attr(2755,root,privileged)` - exactly as amazfish installs its own. That
// is narrower than putting the user in the group, since only this one binary
// gains the access, but it is a real privilege: setgid privileged can read
// every privileged store, not only contacts.
//
// It was taken anyway, on this reasoning: the daemon already observes every
// notification on the session bus with its full content - every message,
// every mail, every alert. Read access to a list of names is not a
// meaningful increase in what it could learn.
//
// **The match flag does the hard part.** `MatchPhoneNumber` makes the
// backend compare numbers the way a phone book does, so `+358401234567` and
// `0401234567` find the same contact. Writing that normalisation here would
// have meant getting it wrong for some format nobody tested.
class ContactLookup : public QObject
{
    Q_OBJECT

public:
    explicit ContactLookup(QObject *parent = nullptr);

    // Opens the address book and says whether it could. Called once, at
    // startup, so the cost of opening it is not paid while a phone is
    // ringing. A false here is not fatal to anything: a call then shows its
    // number, which is what it did before this existed.
    bool open(QString *error);

    // The display name for `number`, or an empty string when there is no such
    // contact, when the number is empty, or when the address book never
    // opened. An empty answer is cached too - most calls are from numbers
    // nobody has saved, and re-querying for each of them would be a database
    // round trip per ring for no gain.
    QString nameFor(const QString &number);

private:
    QtContacts::QContactManager *m_manager = nullptr;
    QHash<QString, QString> m_cache;
};
