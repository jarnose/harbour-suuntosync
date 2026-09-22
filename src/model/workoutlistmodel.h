#pragma once

#include "../store/workout.h"

#include <QAbstractListModel>
#include <QVector>

// Presentation model for WorkoutListPage - a thin QAbstractListModel wrapper
// around whatever AppController loaded from WorkoutStore, same pattern as
// harbour-otpcove's AccountListModel.
class WorkoutListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles {
        KeyRole = Qt::UserRole + 1,
        SourceRole,
        ActivityIdRole,
        StartTimeRole, // unix ms, QML formats it
        TotalTimeRole,      // seconds
        TotalDistanceRole,  // meters
        TotalAscentRole,    // meters
        TotalDescentRole,   // meters
        MaxSpeedRole,          // m/s, 0 = absent
        EnergyConsumptionRole, // kcal, 0 = absent
        StepCountRole,         // 0 = absent
        AvgHeartRateRole,      // bpm, 0 = absent
        MaxHeartRateRole,      // bpm, 0 = absent
        // Training metrics, same 0 = absent convention as above - which is
        // also how the watch's own schema marks them. Mostly BLE-only (see
        // src/ble/summarydecoder.h); recovery time and the training stress
        // score come from the cloud list as well.
        EpocRole,               // ml/kg
        PeakTrainingEffectRole, // 1.0-5.0
        RecoveryTimeRole,       // seconds
        MaxVo2Role,             // ml/kg/min
        TrainingLoadRole,
        TrainingStressScoreRole, // cloud sync only
    };
    Q_ENUM(Roles)

    explicit WorkoutListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setWorkouts(const QVector<Workout> &workouts);

signals:
    void countChanged();

private:
    QVector<Workout> m_workouts;
};
