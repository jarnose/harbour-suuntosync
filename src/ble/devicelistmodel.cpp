#include "devicelistmodel.h"

DeviceListModel::DeviceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DeviceListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_devices.size();
}

QVariant DeviceListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size())
        return QVariant();

    const BluezAdapter::Device &device = m_devices.at(index.row());
    switch (role) {
    case ObjectPathRole:
        return device.objectPath;
    case AddressRole:
        return device.address;
    case NameRole:
        return device.name;
    case PairedRole:
        return device.paired;
    case ConnectedRole:
        return device.connected;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DeviceListModel::roleNames() const
{
    return {
        { ObjectPathRole, "objectPath" },
        { AddressRole, "address" },
        { NameRole, "name" },
        { PairedRole, "paired" },
        { ConnectedRole, "connected" },
    };
}

int DeviceListModel::indexOfPath(const QString &objectPath) const
{
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices.at(i).objectPath == objectPath)
            return i;
    }
    return -1;
}

void DeviceListModel::upsert(const BluezAdapter::Device &device)
{
    const int row = indexOfPath(device.objectPath);
    if (row >= 0) {
        m_devices[row] = device;
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx);
        return;
    }

    beginInsertRows(QModelIndex(), m_devices.size(), m_devices.size());
    m_devices.append(device);
    endInsertRows();
    emit countChanged();
}

void DeviceListModel::clear()
{
    if (m_devices.isEmpty())
        return;
    beginResetModel();
    m_devices.clear();
    endResetModel();
    emit countChanged();
}
