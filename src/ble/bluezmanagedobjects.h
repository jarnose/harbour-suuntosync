#pragma once

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

// D-Bus marshalling glue for org.freedesktop.DBus.ObjectManager, as
// implemented by bluezd (interface signatures "a{sa{sv}}" and
// "a{oa{sa{sv}}}"). Qt5::DBus has built-in support for QVariantMap (a{sv})
// but not for maps *of* maps, so the two extra typedefs here need explicit
// operator<</operator>> and a one-time qDBusRegisterMetaType() call
// (BluezAdapter's constructor does this) before any GetManagedObjects call
// or InterfacesAdded/InterfacesRemoved signal connection will work.
//
// Shared between BluezAdapter (Phase 5, device discovery) and
// MdsWhiteboardClient (Phase 6, GATT service/characteristic discovery under
// a connected device's object path) - both are just "enumerate objects
// implementing interface X, read their properties" over the same
// ObjectManager API.

// interface name -> its properties (a{sa{sv}})
using BluezInterfaceMap = QMap<QString, QVariantMap>;
// object path -> interfaces implemented by that object (a{oa{sa{sv}}})
using BluezObjectMap = QMap<QDBusObjectPath, BluezInterfaceMap>;

Q_DECLARE_METATYPE(BluezInterfaceMap)
Q_DECLARE_METATYPE(BluezObjectMap)

QDBusArgument &operator<<(QDBusArgument &arg, const BluezInterfaceMap &map);
const QDBusArgument &operator>>(const QDBusArgument &arg, BluezInterfaceMap &map);
QDBusArgument &operator<<(QDBusArgument &arg, const BluezObjectMap &map);
const QDBusArgument &operator>>(const QDBusArgument &arg, BluezObjectMap &map);

// Registers the two operator>>/operator<< pairs above with Qt D-Bus's
// marshaller. Idempotent - safe to call from more than one class's
// constructor (BluezAdapter and, from Phase 6, MdsWhiteboardClient both do).
void registerBluezManagedObjectTypes();
