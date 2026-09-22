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
    case MaxSpeedRole:
        return w.maxSpeed;
    case EnergyConsumptionRole:
        return w.energyConsumption;
    case StepCountRole:
        return w.stepCount;
    case AvgHeartRateRole:
        return w.avgHeartRate;
    case MaxHeartRateRole:
        return w.maxHeartRate;
    case EpocRole:
        return w.epoc;
    case PeakTrainingEffectRole:
        return w.peakTrainingEffect;
    case RecoveryTimeRole:
        return w.recoveryTime;
    case MaxVo2Role:
        return w.maxVo2;
    case TrainingLoadRole:
        return w.trainingLoad;
    case TrainingStressScoreRole:
        return w.trainingStressScore;
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
        { MaxSpeedRole, "maxSpeed" },
        { EnergyConsumptionRole, "energyConsumption" },
        { StepCountRole, "stepCount" },
        { AvgHeartRateRole, "avgHeartRate" },
        { MaxHeartRateRole, "maxHeartRate" },
        { EpocRole, "epoc" },
        { PeakTrainingEffectRole, "peakTrainingEffect" },
        { RecoveryTimeRole, "recoveryTime" },
        { MaxVo2Role, "maxVo2" },
        { TrainingLoadRole, "trainingLoad" },
        { TrainingStressScoreRole, "trainingStressScore" },
    };
}

void WorkoutListModel::setWorkouts(const QVector<Workout> &workouts)
{
    beginResetModel();
    m_workouts = workouts;
    endResetModel();
    emit countChanged();
}
