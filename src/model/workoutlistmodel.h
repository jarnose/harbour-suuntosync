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
