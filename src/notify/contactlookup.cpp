#include "contactlookup.h"

#include <QDebug>
#include <QtContacts/QContact>
#include <QtContacts/QContactDetailFilter>
#include <QtContacts/QContactDisplayLabel>
#include <QtContacts/QContactPhoneNumber>

using namespace QtContacts;

ContactLookup::ContactLookup(QObject *parent)
    : QObject(parent)
{
}

bool ContactLookup::open(QString *error)
{
    if (m_manager)
        return true;

    // The default manager, which on Sailfish is qtcontacts-sqlite against the
    // privileged store. `mergePresenceChanges` off because nothing here cares
    // about presence and it saves the backend some work - the same parameter,
    // and the same empty manager name, that libwatchfish passes.
    QMap<QString, QString> parameters;
    parameters.insert(QStringLiteral("mergePresenceChanges"), QStringLiteral("false"));
    m_manager = new QContactManager(QString(), parameters, this);

    // Constructing a manager does not fail loudly, so this asks it something
    // real. An empty address book and an unreadable one both answer with no
    // contacts, which is why the error code is what gets checked rather than
    // the count: without the setgid group the backend cannot open its
    // database and says so here.
    m_manager->contactIds(QContactFilter());
    if (m_manager->error() != QContactManager::NoError) {
        if (error) {
            *error = QStringLiteral("%1 (error %2) - is the binary setgid privileged?")
                             .arg(m_manager->managerName())
                             .arg(int(m_manager->error()));
        }
        delete m_manager;
        m_manager = nullptr;
        return false;
    }

    qInfo().noquote() << "contacts available via" << m_manager->managerName();
    return true;
}

QString ContactLookup::nameFor(const QString &number)
{
    if (!m_manager || number.trimmed().isEmpty())
        return QString();

    const auto cached = m_cache.constFind(number);
    if (cached != m_cache.constEnd())
        return cached.value();

    // MatchPhoneNumber is the whole point: the backend compares numbers the
    // way an address book does, so a call from +358401234567 finds a contact
    // saved as 040 123 4567. Normalising by hand here would be a second,
    // worse implementation of that.
    QContactDetailFilter filter;
    filter.setDetailType(QContactDetail::TypePhoneNumber, QContactPhoneNumber::FieldNumber);
    filter.setMatchFlags(QContactFilter::MatchPhoneNumber);
    filter.setValue(number);

    QString name;
    const QList<QContact> found = m_manager->contacts(filter);
    if (!found.isEmpty()) {
        name = found.first()
                       .detail(QContactDetail::TypeDisplayLabel)
                       .value(QContactDisplayLabel::FieldLabel)
                       .toString();
    } else if (m_manager->error() != QContactManager::NoError) {
        // Logged, not cached: a query that failed may succeed later, and
        // caching the failure would make one bad moment permanent.
        qWarning().noquote() << "contact lookup failed, error" << int(m_manager->error());
        return QString();
    }

    // An empty answer is cached as well. Most calls are from numbers nobody
    // has saved, and asking the database again for each ring buys nothing.
    m_cache.insert(number, name);
    return name;
}
