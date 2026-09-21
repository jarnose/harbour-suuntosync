#include "workoutlistmodel.h"

WorkoutListModel::WorkoutListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int WorkoutListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_workouts.size();
}

QVariant WorkoutListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_workouts.size())
        return QVariant();

    const Workout &w = m_workouts.at(index.row());
    switch (role) {
    case KeyRole:
        return w.key;
    case SourceRole:
        return w.source;
    case ActivityIdRole:
        return w.activityId;
    case StartTimeRole:
        return w.startTime;
    case TotalTimeRole:
        return w.totalTime;
    case TotalDistanceRole:
        return w.totalDistance;
    case TotalAscentRole:
        return w.totalAscent;
    case TotalDescentRole:
        return w.totalDescent;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> WorkoutListModel::roleNames() const
{
    return {
        { KeyRole, "key" },
        { SourceRole, "source" },
        { ActivityIdRole, "activityId" },
        { StartTimeRole, "startTime" },
        { TotalTimeRole, "totalTime" },
        { TotalDistanceRole, "totalDistance" },
        { TotalAscentRole, "totalAscent" },
        { TotalDescentRole, "totalDescent" },
    };
}

void WorkoutListModel::setWorkouts(const QVector<Workout> &workouts)
{
    beginResetModel();
    m_workouts = workouts;
    endResetModel();
    emit countChanged();
}
