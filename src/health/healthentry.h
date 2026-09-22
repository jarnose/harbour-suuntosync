#pragma once

#include <QByteArray>
#include <QString>

// One round-the-clock reading from the watch, as the cloud stores it.
//
// The cloud's own shape is {"timestamp": <ISO 8601>, "entryData": {...}} and
// entryData differs per kind - a night of sleep has eighteen fields, a
// recovery sample has two (see docs/workout-upload.md). Rather than four
// tables with four column sets, the payload is kept as the JSON it arrived
// as and the kind is a column. That also means a firmware that starts
// reporting a new field needs no migration here, which matters for a format
// nobody documents to us.
struct HealthEntry
{
    QString kind;      // "sleep", "sleepstages", "recovery", "activity"
    qint64 timestamp = 0; // unix ms, parsed from the entry's own ISO 8601
    QByteArray data;   // the entryData object, verbatim JSON
};
