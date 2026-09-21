#include "bluezmanagedobjects.h"

#include <QDBusMetaType>

QDBusArgument &operator<<(QDBusArgument &arg, const BluezInterfaceMap &map)
{
    arg.beginMap(QVariant::String, qMetaTypeId<QVariantMap>());
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        arg.beginMapEntry();
        arg << it.key() << it.value();
        arg.endMapEntry();
    }
    arg.endMap();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BluezInterfaceMap &map)
{
    map.clear();
    arg.beginMap();
    while (!arg.atEnd()) {
        QString key;
        QVariantMap value;
        arg.beginMapEntry();
        arg >> key >> value;
        arg.endMapEntry();
        map.insert(key, value);
    }
    arg.endMap();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const BluezObjectMap &map)
{
    arg.beginMap(qMetaTypeId<QDBusObjectPath>(), qMetaTypeId<BluezInterfaceMap>());
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        arg.beginMapEntry();
        arg << it.key() << it.value();
        arg.endMapEntry();
    }
    arg.endMap();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, BluezObjectMap &map)
{
    map.clear();
    arg.beginMap();
    while (!arg.atEnd()) {
        QDBusObjectPath key;
        BluezInterfaceMap value;
        arg.beginMapEntry();
        arg >> key >> value;
        arg.endMapEntry();
        map.insert(key, value);
    }
    arg.endMap();
    return arg;
}

void registerBluezManagedObjectTypes()
{
    static bool registered = false;
    if (registered)
        return;
    qDBusRegisterMetaType<BluezInterfaceMap>();
    qDBusRegisterMetaType<BluezObjectMap>();
    registered = true;
}
