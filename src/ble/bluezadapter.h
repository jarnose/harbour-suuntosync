#pragma once

#include "bluezmanagedobjects.h"

#include <QObject>
#include <QString>

// Thin wrapper around BlueZ 5's own D-Bus API (org.bluez.*), used instead of
// Qt Bluetooth: Sailfish OS 5.1's SDK target has no pkgconfig(Qt5Bluetooth)
// at all (confirmed by a real failed build - see
// ~/.claude/plans/agile-hopping-harp.md, Phase 5 note), so
// QLowEnergyController/QBluetoothDeviceDiscoveryAgent aren't available here.
//
// Deliberately does NOT implement pairing/bonding (no org.bluez.Agent1
// registration): driving a fresh Just-Works-or-not BLE pairing flow
// correctly from a third-party app needs a registered pairing agent to
// answer any PIN/passkey/confirmation request BlueZ raises, which is a
// meaningfully large, easy-to-get-wrong, and (in this sandbox) completely
// untestable subsystem to add blind. Sailfish's own Settings > Bluetooth
// already has a working pairing UI with the OS's default agent - this class
// only lists devices (paired or not, via refresh()/startDiscovery()) and
// calls Device1.Connect() on ones the user already paired there. If
// Device1.Connect() turns out to also need agent interaction for the Race
// even post-pairing, that'll show up as connectFinished(ok=false) with a
// BlueZ error string - a real build/test cycle will tell us, not more
// guessing here.
class BluezAdapter : public QObject
{
    Q_OBJECT
public:
    struct Device
    {
        QString objectPath;
        QString address;
        QString name;
        bool paired = false;
        bool connected = false;
    };

    explicit BluezAdapter(QObject *parent = nullptr);

    // Re-reads every currently known org.bluez.Device1 object (bonded, or
    // previously seen during a scan) via a single GetManagedObjects call and
    // emits deviceUpdated() for each. Cheap - call whenever the pairing page
    // opens/is pulled to refresh.
    void refresh();

    // Starts/stops a BlueZ inquiry scan so not-yet-paired devices show up
    // too (so the UI can tell the user to go pair one in Settings first).
    // Newly-seen devices arrive via deviceUpdated() as InterfacesAdded
    // signals come in - call refresh() first to seed the list with anything
    // already known.
    void startDiscovery();
    void stopDiscovery();

    // Connects at the GATT level to an already-paired device. Does not
    // attempt to pair/bond - see the class comment above.
    void connectToDevice(const QString &objectPath);
    void disconnectFromDevice(const QString &objectPath);

signals:
    void deviceUpdated(const BluezAdapter::Device &device);
    void connectFinished(const QString &objectPath, bool ok, const QString &error);
    void errorOccurred(const QString &message);

private slots:
    // Connected via the old SIGNAL()/SLOT() string macros (QDBusConnection::
    // connect() requires that form, not the pointer-to-member-function
    // style) to org.freedesktop.DBus.ObjectManager's InterfacesAdded, so new
    // devices found mid-scan show up without a manual refresh().
    void onInterfacesAdded(const QDBusObjectPath &path, const BluezInterfaceMap &interfaces);

private:
    QString adapterPath();
    void emitDeviceIfPresent(const QString &objectPath, const BluezInterfaceMap &interfaces);

    QString m_adapterPath;
};
