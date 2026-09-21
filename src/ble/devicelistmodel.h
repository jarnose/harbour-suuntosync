#pragma once

#include "bluezadapter.h"

#include <QAbstractListModel>
#include <QVector>

// Presentation model for PairingPage - holds whatever devices AppController
// forwards to it (already filtered down to ones that look like a Suunto
// watch; this model itself is generic and doesn't know about that filter).
class DeviceListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles {
        ObjectPathRole = Qt::UserRole + 1,
        AddressRole,
        NameRole,
        PairedRole,
        ConnectedRole,
    };
    Q_ENUM(Roles)

    explicit DeviceListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Adds the device if its objectPath is new, otherwise updates the
    // existing row in place (BlueZ may report the same device repeatedly -
    // once from refresh()'s GetManagedObjects and again from a later
    // InterfacesAdded during an active scan).
    void upsert(const BluezAdapter::Device &device);
    void clear();

signals:
    void countChanged();

private:
    int indexOfPath(const QString &objectPath) const;

    QVector<BluezAdapter::Device> m_devices;
};
