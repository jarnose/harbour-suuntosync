#include "appcontroller.h"
#include "../secrets/tokenvault.h"
#include "../cloud/cloudaccountstore.h"
#include "../ble/pairedwatchstore.h"
#include "../ble/devicelistmodel.h"
#include "../ble/mdswhiteboardclient.h"
#include "../cloud/suuntocloudclient.h"
#include "../store/workoutstore.h"
#include "../store/workout.h"
#include "../model/workoutlistmodel.h"
#include "../health/healthstore.h"
#include "../ble/logbookdecoder.h"
#include "../ble/summarydecoder.h"
#include "../ble/smldecoder.h"
#include "../ble/sleepdecoder.h"
#include "../ble/recoverydecoder.h"
#include "../ble/activitydecoder.h"
#include "../cloud/polyline.h"
#include "../cloud/smljson.h"
#include "../cloud/zipwriter.h"

#include <QStandardPaths>
#include <QtMath>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QTimer>
#include <QSettings>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <map>
#include <cstring>
#include <stdexcept>

namespace {

// Appends a probe result to a file next to the app's cache. See the
// connect() in the constructor for why.
void appendProbeLog(const QString &text)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    QFile file(dir + QStringLiteral("/probe-log.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n"
        << text << "\n\n";
}

QString dbPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QStringLiteral("suuntosync.sqlite"));
}

// "ble_" prefix keeps this in its own key namespace, separate from cloud
// workouts' own "wk_..." keys - deliberately not attempting to merge a
// BLE-synced and cloud-synced record of the same real workout into one row
// (no reliable cross-reference beyond timestamp proximity, which isn't
// solid enough to upsert-collide on automatically); they'll just show up
// as two entries in the list for now. See docs/logbook-data-format.md for
// which Workout fields Logbook::decode() can and can't populate.
// energyConsumption stays at 0 (absent): it's Header.Energy, which lives in
// a header chunk that /Data doesn't carry at all. Ascent/descent are now
// populated but are a ~10%-accurate derivation from the altitude series,
// not the watch's own figures - see logbookdecoder.h.
Workout workoutFromDecoded(const QString &logbookId, const Logbook::DecodedWorkout &decoded)
{
    Workout w;
    w.key = QStringLiteral("ble_%1").arg(logbookId);
    w.source = QStringLiteral("ble");
    w.activityId = decoded.activityId;
    w.startTime = static_cast<qint64>(decoded.startTimeMs);
    w.stopTime = static_cast<qint64>(decoded.stopTimeMs);
    w.totalTime = decoded.totalTimeSeconds;
    w.totalDistance = decoded.totalDistanceMeters;
    w.maxSpeed = decoded.maxSpeedMs;
    w.avgHeartRate = decoded.avgHeartRateBpm;
    w.maxHeartRate = decoded.maxHeartRateBpm;
    w.stepCount = decoded.stepCount;
    if (decoded.hasAltitude) {
        w.totalAscent = decoded.totalAscentMeters;
        w.totalDescent = decoded.totalDescentMeters;
    }
    return w;
}

// The GPS track, packed the way WorkoutStore stores it: pairs of
// little-endian int32, degrees x 1e7 - the watch's own on-wire form, so
// nothing is lost and nothing is re-scaled. Templated over the point type
// because a BLE track and a decoded cloud polyline are different structs
// that happen to agree on latitude/longitude.
template <typename PointList>
QByteArray packTrack(const PointList &track)
{
    QByteArray out;
    out.resize(static_cast<int>(track.size()) * 2 * static_cast<int>(sizeof(qint32)));
    char *p = out.data();
    for (const auto &point : track) {
        const qint32 lat = static_cast<qint32>(qRound(point.latitude * 1e7));
        const qint32 lon = static_cast<qint32>(qRound(point.longitude * 1e7));
        std::memcpy(p, &lat, sizeof(qint32));
        p += sizeof(qint32);
        std::memcpy(p, &lon, sizeof(qint32));
        p += sizeof(qint32);
    }
    return out;
}

// The schema's canonical units are SI-ish and not always what a person
// wants to read: heart rate and cadence come back in hertz, temperature in
// kelvin, energy in joules. Convert those here, once, and carry the unit
// along so the UI doesn't have to know the schema.
//
// Anything not matched keeps its canonical value, with an empty unit - that
// covers counts, flags, enums and the handful of fields whose unit this
// project hasn't established. Better a bare number than a confidently wrong
// label.
struct DisplayValue
{
    double value;
    QString unit;
};

DisplayValue toDisplayUnits(const QString &name, double value)
{
    if (name.contains(QStringLiteral("HR")) || name.endsWith(QStringLiteral("Cadence")))
        return { value * 60.0, QStringLiteral("bpm") };
    if (name.contains(QStringLiteral("Temperature")))
        return { value - 273.15, QStringLiteral("\u00b0C") };
    if (name.contains(QStringLiteral("Energy")))
        return { value / 4184.0, QStringLiteral("kcal") };
    if (name.contains(QStringLiteral("Duration")) || name.endsWith(QStringLiteral("Time")))
        return { value, QStringLiteral("s") };
    if (name.contains(QStringLiteral("Distance")) || name.contains(QStringLiteral("Altitude"))
            || name.contains(QStringLiteral("Ascent")) || name.contains(QStringLiteral("Descent")))
        return { value, QStringLiteral("m") };
    if (name.contains(QStringLiteral("Speed")))
        return { value, QStringLiteral("m/s") };
    if (name.contains(QStringLiteral("Pressure")))
        return { value, QStringLiteral("Pa") };
    return { value, QString() };
}

// Decodes every field in a /Summary payload - not just the dozen that get
// their own Workout column - into { name: { value, unit } } for storage.
// See WorkoutStore::saveDetails() for why this isn't a set of columns.
QByteArray summaryDetailsJson(const std::vector<uint8_t> &payload,
                               const SbemDescriptors::Table &table)
{
    QJsonObject fields;
    Sml::decode(Sbem::parseContainer(payload), table, [&fields](const Sml::Reading &reading) {
        const QString name = QString::fromLatin1(reading.descriptor->name);
        if (name.isEmpty())
            return;
        const DisplayValue display = toDisplayUnits(name, reading.value);
        QJsonObject entry;
        entry.insert(QStringLiteral("value"), display.value);
        if (!display.unit.isEmpty())
            entry.insert(QStringLiteral("unit"), display.unit);
        fields.insert(name, entry);
    });
    return QJsonDocument(fields).toJson(QJsonDocument::Compact);
}

// The series worth charting. Everything else the watch records is either
// monotonic (distance), a diagnostic (battery, satellite count) or a one-off
// event, none of which a line graph tells you anything about - they're all
// still in the "all recorded fields" list.
const char *const kChartedSeries[] = {
    "Sample.HR", "Sample.Altitude", "Sample.Speed",
    "Sample.Cadence", "Sample.Power", "Sample.Temperature",
};

// A chart on a phone screen is a few hundred pixels wide, and a workout can
// carry several thousand samples per series, so reduce to this many buckets
// and average within each. Averaging rather than sampling means a spike
// shows up as a bump instead of being missed entirely, and the true min and
// max are carried alongside so the axis labels stay honest even where the
// drawn line has been smoothed.
constexpr int kSeriesPoints = 200;
constexpr int kMinSamplesToChart = 20;

// Turns collected samples into the stored curve format. Shared by the BLE
// and cloud paths: both end up with values in the schema's canonical units
// keyed by SML-style name, they just get there differently.
QByteArray seriesJsonFromSamples(const std::map<QString, std::vector<double>> &samples)
{
    QJsonArray series;
    for (const char *name : kChartedSeries) {
        const QString qname = QString::fromLatin1(name);
        const auto it = samples.find(qname);
        if (it == samples.end() || static_cast<int>(it->second.size()) < kMinSamplesToChart)
            continue;
        const std::vector<double> &values = it->second;

        const auto range = std::minmax_element(values.begin(), values.end());
        const DisplayValue low = toDisplayUnits(qname, *range.first);
        const DisplayValue high = toDisplayUnits(qname, *range.second);
        if (qFuzzyCompare(low.value, high.value))
            continue; // a flat line says nothing

        QJsonArray points;
        for (int bucket = 0; bucket < kSeriesPoints; ++bucket) {
            const size_t from = values.size() * bucket / kSeriesPoints;
            size_t to = values.size() * (bucket + 1) / kSeriesPoints;
            if (to <= from)
                to = from + 1;
            double sum = 0;
            size_t count = 0;
            for (size_t i = from; i < to && i < values.size(); ++i, ++count)
                sum += values[i];
            if (count == 0)
                continue;
            points.append(toDisplayUnits(qname, sum / count).value);
        }
        if (points.isEmpty())
            continue;

        QJsonObject entry;
        entry.insert(QStringLiteral("name"), qname);
        entry.insert(QStringLiteral("unit"), low.unit);
        entry.insert(QStringLiteral("min"), low.value);
        entry.insert(QStringLiteral("max"), high.value);
        entry.insert(QStringLiteral("points"), points);
        series.append(entry);
    }
    return series.isEmpty() ? QByteArray() : QJsonDocument(series).toJson(QJsonDocument::Compact);
}

// Reduces each charted series from a /Data payload to a fixed-size curve.
// Returns {} if nothing chartable was found, which is normal - a workout
// with no GPS and no heart-rate strap has very little to draw.
QByteArray buildSeriesJson(const std::vector<uint8_t> &compressed,
                            const SbemDescriptors::Table &table)
{
    std::map<QString, std::vector<double>> samples;
    Sml::decode(Sbem::parseContainer(Sbem::heatshrinkDecompress(compressed)), table,
                [&samples](const Sml::Reading &reading) {
        samples[QString::fromLatin1(reading.descriptor->name)].push_back(reading.value);
    });
    return seriesJsonFromSamples(samples);
}

// Walks the cloud's /sml JSON collecting the leaves worth charting. Written
// shape-agnostically on purpose: the response hasn't been captured, so
// rather than assuming a nesting this looks for the leaf names the schema
// uses ("HR", "Altitude", ...) wherever they appear, in document order.
void collectSmlLeaves(const QJsonValue &value, std::map<QString, std::vector<double>> *samples)
{
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray())
            collectSmlLeaves(item, samples);
        return;
    }
    if (!value.isObject())
        return;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isDouble()) {
            const QString name = QStringLiteral("Sample.") + it.key();
            for (const char *charted : kChartedSeries) {
                if (name == QLatin1String(charted)) {
                    (*samples)[name].push_back(it.value().toDouble());
                    break;
                }
            }
        } else {
            collectSmlLeaves(it.value(), samples);
        }
    }
}

QByteArray buildCloudSeriesJson(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull())
        return QByteArray();

    std::map<QString, std::vector<double>> samples;
    collectSmlLeaves(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), &samples);

    // The watch's own SML stores heart rate and cadence in hertz, and this
    // project's unit mapping assumes that. Which convention the cloud's
    // JSON uses is still unknown - and deliberately doesn't need to be,
    // because this check is safe either way round: 10 Hz would be 600 bpm
    // and 10 bpm would be a corpse, so neither convention can land on the
    // wrong side of the threshold. Confirmed against a real response
    // (88-161 bpm, 0-9.5 m/s, 101-141 m), which read correctly without
    // establishing which branch it took - as designed.
    for (const char *name : { "Sample.HR", "Sample.Cadence" }) {
        auto it = samples.find(QString::fromLatin1(name));
        if (it == samples.end() || it->second.empty())
            continue;
        const double max = *std::max_element(it->second.begin(), it->second.end());
        if (max >= 10) {
            for (double &v : it->second)
                v /= 60.0; // already bpm/rpm; bring it back to the canonical unit
        }
    }
    return seriesJsonFromSamples(samples);
}

// Builds the sml.zip the cloud wants from the two payloads a watch sync
// already has in hand: /Data (still Heatshrink-compressed) and /Summary.
// Returns {} if neither produced anything worth sending.
//
// The offset is the phone's own UTC offset *at the time of the workout*,
// not right now, so a workout recorded before a DST change is stamped the
// way it was recorded. The watch's local64 carries its own offset, but
// Sbem::decodeLocal64 folds it into UTC and doesn't hand it back; using the
// phone's zone gets the common case right and is wrong only for a workout
// recorded in another timezone.
QByteArray buildSmlZip(const std::vector<uint8_t> &compressedData,
                        const std::vector<uint8_t> &summaryPayload,
                        const QString &source, qint64 startTimeMs, qint64 stopTimeMs,
                        const SbemDescriptors::Table &table)
{
    const int offsetMinutes =
            QDateTime::fromMSecsSinceEpoch(startTimeMs).offsetFromUtc() / 60;
    const std::string src = source.toStdString();

    const std::string samples = SmlJson::buildDocument(
            Sbem::parseContainer(Sbem::heatshrinkDecompress(compressedData)), table,
            src, offsetMinutes);
    // A /Summary payload has no clock chunk, so its entries need stamping
    // from outside. The captured upload puts summary.json's entries at the
    // end of the workout, so that is what goes in.
    const std::string summary = summaryPayload.empty()
            ? std::string()
            : SmlJson::buildDocument(Sbem::parseContainer(summaryPayload), table, src,
                                      offsetMinutes,
                                      stopTimeMs != 0 ? stopTimeMs : startTimeMs);

    std::vector<ZipWriter::Entry> entries;
    if (!samples.empty())
        entries.push_back({ "samples.json", samples });
    if (!summary.empty())
        entries.push_back({ "summary.json", summary });
    if (entries.empty())
        return QByteArray();

    const std::vector<uint8_t> zip = ZipWriter::build(entries);
    return QByteArray(reinterpret_cast<const char *>(zip.data()),
                       static_cast<int>(zip.size()));
}

// The watch's own identifier as the cloud spells it: "suunto-" plus the
// serial, which is the last word of the BlueZ device name ("Suunto Race
// 2352D0000247" -> "suunto-2352D0000247"). Matches a captured upload.
QString smlSourceFor(const QString &watchName)
{
    const QString serial = watchName.section(QLatin1Char(' '), -1).trimmed();
    return serial.isEmpty() ? QStringLiteral("suunto-unknown")
                             : QStringLiteral("suunto-") + serial;
}

// Lap markers, from the Lap event chunk. The schema's own enum, so a
// manual lap and an auto-lap read differently rather than all showing up
// as "lap".
QString lapTypeName(int type)
{
    switch (type) {
    case 0: return QStringLiteral("Start");
    case 1: return QStringLiteral("Stop");
    case 2: return QStringLiteral("Distance");
    case 3: return QStringLiteral("Manual");
    case 4: return QStringLiteral("Interval");
    case 5: return QStringLiteral("High interval");
    case 6: return QStringLiteral("Low interval");
    default: return QString::number(type);
    }
}

// Turns the lap markers in a /Data payload into laps: a marker is a point
// in time, a lap is the stretch between two of them, so each entry carries
// the split duration and distance as well as the marker that ended it.
// Sample.Distance is cumulative, which is what makes the distance split
// just a subtraction.
QByteArray buildLapsJson(const std::vector<uint8_t> &compressed,
                          const SbemDescriptors::Table &table)
{
    struct Marker { int type; int64_t timeMs; double distance; };
    std::vector<Marker> markers;
    double distance = 0;
    int64_t firstTimeMs = 0;
    int64_t lastTimeMs = 0;

    Sml::decode(Sbem::parseContainer(Sbem::heatshrinkDecompress(compressed)), table,
                [&](const Sml::Reading &reading) {
        const QLatin1String name(reading.descriptor->name);
        if (reading.timeMs > 0) {
            if (firstTimeMs == 0)
                firstTimeMs = reading.timeMs;
            lastTimeMs = reading.timeMs;
        }
        if (name == QLatin1String("Sample.Distance"))
            distance = reading.value;
        else if (name == QLatin1String("Sample.Events.Array.Lap.Type"))
            markers.push_back({ static_cast<int>(reading.value), reading.timeMs, distance });
    });

    if (markers.empty())
        return QByteArray();

    QJsonArray laps;
    int64_t fromTime = firstTimeMs;
    double fromDistance = 0;
    for (const Marker &marker : markers) {
        // A Start marker opens the workout rather than closing a lap.
        if (marker.type == 0 && laps.isEmpty() && marker.timeMs <= fromTime) {
            fromTime = marker.timeMs;
            fromDistance = marker.distance;
            continue;
        }
        QJsonObject lap;
        lap.insert(QStringLiteral("number"), laps.size() + 1);
        lap.insert(QStringLiteral("type"), lapTypeName(marker.type));
        lap.insert(QStringLiteral("durationSeconds"), (marker.timeMs - fromTime) / 1000.0);
        lap.insert(QStringLiteral("distanceMeters"), marker.distance - fromDistance);
        laps.append(lap);
        fromTime = marker.timeMs;
        fromDistance = marker.distance;
    }

    // Whatever came after the last marker is a lap too, unless the last
    // marker was the workout stopping.
    if (!markers.empty() && markers.back().type != 1 && lastTimeMs > fromTime) {
        QJsonObject lap;
        lap.insert(QStringLiteral("number"), laps.size() + 1);
        lap.insert(QStringLiteral("type"), QStringLiteral("End"));
        lap.insert(QStringLiteral("durationSeconds"), (lastTimeMs - fromTime) / 1000.0);
        lap.insert(QStringLiteral("distanceMeters"), distance - fromDistance);
        laps.append(lap);
    }

    return laps.isEmpty() ? QByteArray() : QJsonDocument(laps).toJson(QJsonDocument::Compact);
}

// Flattens an arbitrary JSON object into name -> number, joining nested
// keys with dots. Deliberately shape-agnostic: the cloud's workout
// "extensions" are typed by a discriminator this project has no captured
// example of, so rather than guessing at a model, whatever numbers arrive
// get shown under their own names. Strings and arrays are skipped - the
// details view is a numeric table.
void flattenJson(const QJsonObject &object, const QString &prefix, QJsonObject *out)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString name = prefix.isEmpty() ? it.key() : prefix + QLatin1Char('.') + it.key();
        const QJsonValue value = it.value();
        if (value.isObject()) {
            flattenJson(value.toObject(), name, out);
        } else if (value.isDouble() || value.isBool()) {
            double number = value.isBool() ? (value.toBool() ? 1 : 0) : value.toDouble();
            QString unit;
            // The cloud names these fields differently from the watch's own
            // schema, so toDisplayUnits() (which is tuned to SML paths)
            // doesn't apply. Only the two conversions that are unambiguous
            // here are made; the rest keep their raw value and no label,
            // which beats guessing a unit wrong.
            if (name.contains(QStringLiteral("emperature"))) {
                number -= 273.15;
                unit = QStringLiteral("\u00b0C");
            } else if (name.endsWith(QStringLiteral("Time"))) {
                unit = QStringLiteral("s");
            }
            QJsonObject entry;
            entry.insert(QStringLiteral("value"), number);
            if (!unit.isEmpty())
                entry.insert(QStringLiteral("unit"), unit);
            out->insert(name, entry);
        }
    }
}

// The cloud's per-workout extensions, turned into the same name/value rows
// the watch's own fields already use. An extension carries its kind in a
// "type" field, which becomes the name prefix so two extensions with a
// similarly named number don't collide.
QByteArray cloudDetailsJson(const QJsonObject &payload)
{
    QJsonObject fields;
    for (const QJsonValue &value : payload.value(QStringLiteral("extensions")).toArray()) {
        const QJsonObject extension = value.toObject();
        QString type = extension.value(QStringLiteral("type")).toString();
        if (type.isEmpty())
            type = QStringLiteral("Extension");
        flattenJson(extension, type, &fields);
    }
    return fields.isEmpty() ? QByteArray() : QJsonDocument(fields).toJson(QJsonDocument::Compact);
}

// Overlays the watch's own computed totals onto a workout decoded from
// /Data. Everything here is exact where Logbook::decode() could only
// approximate (see summarydecoder.h), so it wins outright; heart rate is
// left alone because this decoder doesn't read the Summary's own HR window.
void applySummary(Workout *w, const Summary::DecodedSummary &s)
{
    if (!s.valid)
        return;
    w->activityId = s.activityId;
    // A workout that never got a GPS fix has no timeline anchor in /Data
    // beyond its time base, so prefer the watch's own recorded start here,
    // and derive the end from it rather than leaving a stale value.
    if (s.startTimeMs > 0) {
        w->startTime = s.startTimeMs;
        w->stopTime = s.startTimeMs + static_cast<qint64>(s.durationSeconds * 1000);
    }
    w->totalTime = s.movingTimeSeconds;
    w->totalDistance = s.distanceMeters;
    if (s.stepCount > 0)
        w->stepCount = s.stepCount;
    if (s.hasAscent) {
        w->totalAscent = s.ascentMeters;
        w->totalDescent = s.descentMeters;
    }
    if (s.hasEnergy)
        w->energyConsumption = s.energyKcal;
    if (s.hasEpoc)
        w->epoc = s.epoc;
    if (s.hasPeakTrainingEffect)
        w->peakTrainingEffect = s.peakTrainingEffect;
    if (s.hasRecoveryTime)
        w->recoveryTime = s.recoveryTimeSeconds;
    if (s.hasMaxVo2)
        w->maxVo2 = s.maxVo2;
    if (s.hasTrainingLoad)
        w->trainingLoad = s.trainingLoad;
}

} // namespace

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_tokenVault(new TokenVault(this))
    , m_cloudAccountStore(new CloudAccountStore(dbPath()))
    , m_cloudClient(new SuuntoCloudClient(this))
    , m_bluezAdapter(new BluezAdapter(this))
    , m_deviceModel(new DeviceListModel(this))
    , m_pairedWatchStore(new PairedWatchStore(dbPath()))
    , m_whiteboardClient(new MdsWhiteboardClient(this))
    , m_workoutStore(new WorkoutStore(dbPath()))
    , m_workoutModel(new WorkoutListModel(this))
    , m_healthStore(new HealthStore(dbPath()))
{
    QString error;
    if (!m_cloudAccountStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_cloudAccount = m_cloudAccountStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load account: %1").arg(error));
        // Keeps the client's x-totp header keyed to the right account - see
        // SuuntoCloudClient::authorizedRequest(). Set in all three places
        // m_cloudAccount changes rather than derived from a signal, since
        // this one (startup load) deliberately doesn't emit one.
        m_cloudClient->setAccountEmail(m_cloudAccount.email);
    }

    if (!m_workoutStore->open(&error))
        emit errorOccurred(tr("Failed to open database: %1").arg(error));

    if (!m_healthStore->open(&error))
        emit errorOccurred(tr("Failed to open database: %1").arg(error));

    // Mirror every probe result to a file as well as to the screen. These
    // are hex dumps read off a phone banner and then retyped by hand, which
    // is slow and error-prone (one retyped serial already sent me looking
    // for a path bug that wasn't there). The file can just be copied off.
    {
        // QSettings rather than another SQLite table: these are two
        // preferences, and Sailfish puts them under the app's own config
        // directory automatically.
        QSettings settings;
        m_coverMode = settings.value(QStringLiteral("cover/mode"),
                                      QStringLiteral("latest")).toString();
        m_syncOnConnect = settings.value(QStringLiteral("sync/onConnect"), false).toBool();
    }

    // Until a watch's own table has been read, the compiled-in Race one
    // stands in - which is what every version before this used.
    m_descriptorTable = SbemDescriptors::Table::builtin();
    m_descriptorLayout = SbemLayout::builtin();

    connect(this, &AppController::logbookTestResult, this, appendProbeLog);
    connect(this, &AppController::whiteboardTestResult, this, appendProbeLog);

    if (!m_pairedWatchStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_pairedWatch = m_pairedWatchStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load paired watch: %1").arg(error));
        // Its field table, if this watch has been read before - so an
        // upload or a probe works right away, without waiting for a sync
        // to fetch it.
        loadStoredDescriptors();
    }

    // Safe to call every launch - "already exists" counts as success inside
    // TokenVault::ensureCollection(). Not gating startup on this: a Secrets
    // failure should degrade to "signed-out looking" state, not crash/hang
    // the app (same reasoning as OTP Cove's AppController::loadAccounts()).
    m_tokenVault->ensureCollection([this](bool ok, const QString &vaultError) {
        if (!ok)
            emit errorOccurred(tr("Failed to open secure storage: %1").arg(vaultError));
    });

    connect(m_bluezAdapter, &BluezAdapter::deviceUpdated, this, &AppController::onDeviceUpdated);
    connect(m_bluezAdapter, &BluezAdapter::connectFinished,
            this, &AppController::onConnectFinished);
    connect(m_bluezAdapter, &BluezAdapter::errorOccurred, this, &AppController::errorOccurred);

    connect(m_whiteboardClient, &MdsWhiteboardClient::readyChanged, this, [this](bool ready) {
        m_whiteboardReady = ready;
        emit whiteboardReadyChanged();
    });
    connect(m_whiteboardClient, &MdsWhiteboardClient::errorOccurred,
            this, &AppController::errorOccurred);

    // Ask BlueZ what it already knows, once, at startup.
    //
    // onDeviceUpdated() attaches the Whiteboard client to a watch that is
    // already connected at the BlueZ level - but it only runs in response
    // to a deviceUpdated signal, and BlueZ emits nothing when nothing
    // changes. A watch connected before this process started therefore
    // stayed invisible until something else called refresh(), which in
    // practice meant opening the pairing page. The app showed "not
    // connected" for a watch that was connected, and "Change watch" was
    // the only way to fix it.
    //
    // Only worth doing when a watch is actually paired: otherwise this is
    // a D-Bus round trip listing every Bluetooth device the phone has ever
    // seen, for nothing.
    //
    // Deferred to the event loop rather than called here: refresh() is a
    // blocking GetManagedObjects, and a wedged bluetoothd would hold up
    // startup for the D-Bus timeout. That is not hypothetical - it has
    // happened on this device before (see the Phase 5 notes). This way the
    // UI is up first and a slow answer costs nothing visible.
    if (m_pairedWatch.isValid())
        QTimer::singleShot(0, this, [this]() { m_bluezAdapter->refresh(); });
}

AppController::~AppController() = default;

QObject *AppController::deviceModelObject() const
{
    return m_deviceModel;
}

QObject *AppController::workoutModelObject() const
{
    return m_workoutModel;
}

void AppController::onDeviceUpdated(const BluezAdapter::Device &device)
{
    // Only surface devices that look like a Suunto watch - BlueZ otherwise
    // reports every device it's ever seen (headphones, the car, etc.).
    // "Suunto Race 2352D0000247" is the confirmed advertised name for
    // Jarno's Race; matching on the "Suunto " prefix should cover the 9
    // Baro and future models too without needing a per-model list.
    if (!device.name.startsWith(QStringLiteral("Suunto "), Qt::CaseInsensitive))
        return;
    m_deviceModel->upsert(device);

    if (device.objectPath != m_pairedWatch.objectPath)
        return;

    if (device.connected != m_watchConnected) {
        m_watchConnected = device.connected;
        emit watchConnectedChanged();
    }

    // Covers both "just connected via selectWatch()" (onConnectFinished
    // already attaches it too - harmless to attach again, attachToDevice()
    // detaches any prior state first) and "was already connected at the
    // BlueZ level from before this app process started" (e.g. after a
    // rebuild/relaunch - onConnectFinished never fires again in that case
    // since we don't auto-reconnect, so this was the only place that could
    // ever attach the Whiteboard client for an already-connected watch).
    if (device.connected && !m_whiteboardReady)
        m_whiteboardClient->attachToDevice(device.objectPath);
}

void AppController::onConnectFinished(const QString &objectPath, bool ok, const QString &error)
{
    if (objectPath != m_pairedWatch.objectPath)
        return;
    if (!ok) {
        emit errorOccurred(tr("Could not connect to watch: %1").arg(error));
        return;
    }
    m_watchConnected = true;
    emit watchConnectedChanged();
    m_whiteboardClient->attachToDevice(objectPath);
}

void AppController::refreshDevices()
{
    m_bluezAdapter->refresh();
}

void AppController::startScan()
{
    m_bluezAdapter->startDiscovery();
}

void AppController::stopScan()
{
    m_bluezAdapter->stopDiscovery();
}

void AppController::selectWatch(const QString &objectPath, const QString &address,
                                 const QString &name)
{
    PairedWatch watch;
    watch.objectPath = objectPath;
    watch.address = address;
    watch.name = name;
    // Model (e.g. "Race", "9 Baro") isn't reliably derivable from the
    // advertised name alone - left blank until Phase 8 needs it for
    // protocol branching.

    QString error;
    if (!m_pairedWatchStore->save(watch, &error)) {
        emit errorOccurred(tr("Failed to save paired watch: %1").arg(error));
        return;
    }
    m_pairedWatch = watch;
    m_watchConnected = false;
    // A different watch means a different field table. Whatever is in
    // hand belongs to the old one, so drop back to the built-in until
    // this one's own has been loaded or read.
    m_descriptorTable = SbemDescriptors::Table::builtin();
    m_descriptorLayout = SbemLayout::builtin();
    m_descriptorAddress.clear();
    loadStoredDescriptors();
    emit pairedWatchChanged();
    emit watchConnectedChanged();

    m_bluezAdapter->connectToDevice(objectPath);
}

void AppController::forgetWatch()
{
    QString error;
    if (!m_pairedWatchStore->clear(&error)) {
        emit errorOccurred(tr("Failed to forget watch: %1").arg(error));
        return;
    }
    if (m_watchConnected)
        m_bluezAdapter->disconnectFromDevice(m_pairedWatch.objectPath);
    m_whiteboardClient->detach();
    m_pairedWatch = PairedWatch();
    m_watchConnected = false;
    emit pairedWatchChanged();
    emit watchConnectedChanged();
}

void AppController::testWhiteboard()
{
    if (!m_whiteboardReady) {
        emit whiteboardTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_whiteboardTestInFlight) {
        emit whiteboardTestResult(tr("A test request is already in flight"));
        return;
    }

    m_whiteboardTestInFlight = true;
    m_whiteboardClient->get(QStringLiteral("/Logbook/Entries"),
                             [this](bool ok, const Mds::Frame &frame, const QString &error) {
        m_whiteboardTestInFlight = false;
        if (!ok) {
            emit whiteboardTestResult(tr("Request failed: %1").arg(error));
            return;
        }
        emit whiteboardTestResult(tr("OK - type=0x%1 requestId=%2 body=%3 bytes")
                                           .arg(frame.type, 2, 16, QLatin1Char('0'))
                                           .arg(frame.requestId)
                                           .arg(frame.body.size()));
    });
}

void AppController::testLogbookFetch(const QString &logbookId)
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight) {
        emit logbookTestResult(tr("A logbook fetch is already in flight"));
        return;
    }
    if (m_workoutSyncInProgress) {
        emit logbookTestResult(tr("A watch sync is already in progress"));
        return;
    }

    m_logbookTestInFlight = true;
    // Same as a real sync: this watch's own field table first, or the
    // probe reports zeros for a watch that is not a Race.
    ensureDescriptors(logbookId, [this, logbookId]() {
    const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
    m_whiteboardClient->fetchLogbookData(path,
            [this, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("Fetch failed: %1").arg(error));
            return;
        }

        try {
            const Logbook::DecodedWorkout decoded = Logbook::decode(data, m_descriptorLayout);
            const Workout w = workoutFromDecoded(logbookId, decoded);

            QString storeError;
            if (!m_workoutStore->upsert(w, &storeError)) {
                emit logbookTestResult(tr("Decoded OK but failed to save: %1").arg(storeError));
                return;
            }
            loadCachedWorkouts();

            emit logbookTestResult(
                    tr("OK - saved. %1 bytes compressed, activity=%2 duration=%3s "
                       "distance=%4m maxSpeed=%5m/s avgHR=%6 maxHR=%7 steps=%8")
                            .arg(data.size())
                            .arg(decoded.activityId)
                            .arg(decoded.totalTimeSeconds, 0, 'f', 0)
                            .arg(decoded.totalDistanceMeters, 0, 'f', 0)
                            .arg(decoded.maxSpeedMs, 0, 'f', 1)
                            .arg(decoded.avgHeartRateBpm, 0, 'f', 0)
                            .arg(decoded.maxHeartRateBpm, 0, 'f', 0)
                            .arg(decoded.stepCount));
        } catch (const std::exception &e) {
            emit logbookTestResult(tr("Fetched %1 bytes but decoding failed: %2")
                                            .arg(data.size())
                                            .arg(QString::fromUtf8(e.what())));
        }
    });
    });
}

void AppController::testEntriesFetch()
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight) {
        emit logbookTestResult(tr("A logbook fetch is already in flight"));
        return;
    }

    if (m_workoutSyncInProgress) {
        emit logbookTestResult(tr("A watch sync is already in progress"));
        return;
    }

    // Replaced the earlier attempt at reusing fetchLogbookData()'s
    // TYPE=0x10 stream-trigger mechanism (confirmed not to work for
    // /Entries on real hardware, see docs/logbook-data-format.md) with
    // fetchLogEntries(), built from decompiling libmds.so's own
    // protocol_v9 structure-deserializer code - a TYPE=0x0D handle-fetch
    // request, not a stream trigger. Confirmed working on real hardware
    // 2026-09-22 (see the doc's "Gate: PASSED" note).
    m_logbookTestInFlight = true;
    m_whiteboardClient->fetchLogEntries(QStringLiteral("/Logbook/Entries"),
            [this](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("/Entries fetch failed: %1").arg(error));
            return;
        }

        QStringList ids;
        for (const LogEntries::Entry &entry : entries)
            ids.append(QString::number(entry.id));

        emit logbookTestResult(tr("OK - %1 entries: %2")
                                        .arg(entries.size())
                                        .arg(ids.join(QStringLiteral(", "))));
    });
}

void AppController::testDescriptorsFetch(const QString &logbookId)
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight || m_workoutSyncInProgress) {
        emit logbookTestResult(tr("Another fetch is already in progress"));
        return;
    }

    m_logbookTestInFlight = true;
    const QString path = QStringLiteral("/Logbook/byId/%1/Descriptors").arg(logbookId);
    m_whiteboardClient->fetchSummary(path,
            [this, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("Descriptors fetch failed: %1").arg(error));
            return;
        }

        // Saved whole. This is a field catalogue with hundreds of entries;
        // the useful work happens offline against the file, the same way
        // the Race's table was built.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        const QString name = QStringLiteral("descriptors-%1.bin").arg(logbookId);
        QFile dump(dir + QStringLiteral("/") + name);
        if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            dump.write(reinterpret_cast<const char *>(data.data()),
                        static_cast<int>(data.size()));
            dump.close();
        }

        // How many <PTH> entries it carries is the quickest sanity check
        // that this is a field catalogue and not something else.
        int paths = 0;
        const char marker[] = "<PTH>";
        for (size_t i = 0; i + 5 <= data.size(); ++i) {
            if (std::memcmp(data.data() + i, marker, 5) == 0)
                ++paths;
        }
        emit logbookTestResult(tr("Descriptors: %1 bytes, %2 field paths, saved as %3")
                                .arg(data.size()).arg(paths).arg(name));
    });
}

void AppController::probePath(const QString &path)
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight || m_workoutSyncInProgress) {
        emit logbookTestResult(tr("Another fetch is already in progress"));
        return;
    }

    m_logbookTestInFlight = true;
    m_whiteboardClient->get(path, [this, path](bool ok, const Mds::Frame &frame,
                                                 const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("%1: %2").arg(path, error));
            return;
        }

        // f5 with a short body is this protocol's rejection, confirmed
        // against two resources that do not exist (docs/watch-push-
        // resources.md). Saying so beats printing six bytes of hex and
        // leaving the reader to remember what they mean.
        const bool rejected = frame.body.size() <= 6 && !frame.body.empty()
                && frame.body[0] == 0xF5;

        QString hex;
        for (size_t i = 0; i < frame.body.size() && i < 32; ++i)
            hex += QStringLiteral("%1 ").arg(frame.body[i], 2, 16, QLatin1Char('0'));

        emit logbookTestResult(tr("%1\n%2 - type 0x%3, %4 bytes\n%5")
                                .arg(path,
                                      rejected ? tr("NOT on this watch")
                                               : tr("exists"))
                                .arg(frame.type, 2, 16, QLatin1Char('0'))
                                .arg(frame.body.size())
                                .arg(hex.trimmed()));
    });
}

void AppController::testHealthResourceFetch(const QString &kind)
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight || m_workoutSyncInProgress) {
        emit logbookTestResult(tr("Another fetch is already in progress"));
        return;
    }

    // The real mechanism, from the 2026-09-23 capture: the watch renders a
    // timeline file and we page it off the filesystem. The earlier probe
    // used "/Sleep/<serial>/Entries", which the capture showed never
    // exists on the wire - it is an MDS-library abstraction on the Android
    // side, and the watch answered every spelling of it with the six-byte
    // f5 error.
    // Sleep is the only series that works this way. Daily activity was
    // tried here too, against a guessed "mdsAct.sbm" - no such file exists,
    // and libmds.so since showed activity is not a rendered file at all
    // (see testActivityTrendFetch()). The guess is gone rather than left
    // in as a menu entry that cannot succeed.
    if (kind != QLatin1String("Sleep")) {
        emit logbookTestResult(tr("No timeline file is known for %1.").arg(kind));
        return;
    }
    const QString resource = QStringLiteral("/Daily/Sleep/Timeline/Data");
    const QString filename = QStringLiteral("mdsSlp.sbm");

    // A week back. The capture used roughly a day; a week is a harmless
    // widening for a probe and shows whether the cursor is honoured.
    const qint64 newerThan = QDateTime::currentMSecsSinceEpoch() - 7LL * 24 * 3600 * 1000;

    m_logbookTestInFlight = true;
    m_whiteboardClient->fetchTimelineFile(resource, filename, newerThan,
            [this, resource, filename](bool ok, const std::vector<uint8_t> &data,
                                        const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("%1 (%2): %3").arg(resource, filename, error));
            return;
        }

        // Keep the whole payload, not just the hex preview: 6 kB of
        // SBEM0102 whose field meanings are unknown needs looking at
        // offline, and retyping a hex dump is not that.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        QFile dump(dir + QStringLiteral("/") + filename);
        if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            dump.write(reinterpret_cast<const char *>(data.data()),
                        static_cast<int>(data.size()));
            dump.close();
        }

        QString hex;
        for (size_t i = 0; i < data.size() && i < 32; ++i)
            hex += QStringLiteral("%1 ").arg(data[i], 2, 16, QLatin1Char('0'));
        QString ascii;
        for (size_t i = 0; i < data.size() && i < 48; ++i) {
            const uint8_t c = data[i];
            ascii += (c >= 0x20 && c < 0x7F) ? QChar(c) : QLatin1Char('.');
        }
        emit logbookTestResult(tr("%1: %2 bytes\n%3\n\"%4\"")
                                .arg(resource).arg(data.size()).arg(hex.trimmed(), ascii));
    });
}

namespace {

// The ways the /Activity/TrendData cursor could plausibly be encoded.
// libmds.so pins the parameter down to one integer named "timestamp" whose
// value is the app's NewerThan multiplied by 1000 - but the width and unit
// it lands in on the wire are decided by the watch's own metadata, which
// the JSON layer hides. Milliseconds first because that multiply says so;
// seconds second because the neighbouring recovery resource uses them.
//
// There is no int32-milliseconds row: epoch milliseconds do not fit in 32
// bits, so that combination could only ever send a truncated cursor. The
// danger is not that it fails - it is that the watch might accept the
// nonsense and answer, and the sweep would report a working encoding that
// is silently asking for the wrong window.
struct TrendCursorCandidate
{
    uint16_t typeCode;
    bool milliseconds;
    const char *label;
};

const TrendCursorCandidate kTrendCursorCandidates[] = {
    { Mds::kParamInt64, true,  "int64 ms" },
    { Mds::kParamInt64, false, "int64 s" },
    { Mds::kParamInt32, false, "int32 s" },
};

// f5 01 00 80 00 00 - this protocol's rejection reply, confirmed against
// two different resources (docs/watch-push-resources.md). Six bytes
// starting f5 means "no", not "nothing to report".
bool isRejection(const std::vector<uint8_t> &body)
{
    return body.size() <= 6 && !body.empty() && body[0] == 0xF5;
}

} // namespace

void AppController::testActivityTrendFetch()
{
    if (!m_whiteboardReady) {
        emit logbookTestResult(tr("Whiteboard channel isn't ready yet"));
        return;
    }
    if (m_logbookTestInFlight || m_workoutSyncInProgress) {
        emit logbookTestResult(tr("Another fetch is already in progress"));
        return;
    }
    m_logbookTestInFlight = true;
    tryActivityTrendEncoding(0);
}

void AppController::tryActivityTrendEncoding(int index)
{
    const int count = static_cast<int>(sizeof(kTrendCursorCandidates)
                                        / sizeof(kTrendCursorCandidates[0]));
    if (index >= count) {
        m_logbookTestInFlight = false;
        emit logbookTestResult(tr("/Activity/TrendData rejected every cursor encoding tried. "
                                   "The parameter is shaped differently than assumed."));
        return;
    }

    const TrendCursorCandidate &candidate = kTrendCursorCandidates[index];
    const QString path = QStringLiteral("/Activity/TrendData");

    // Two days back. Long enough that a watch worn today must have
    // something to say, short enough that a first reply stays readable.
    const qint64 sinceMs = QDateTime::currentMSecsSinceEpoch() - 2LL * 24 * 3600 * 1000;
    const qint64 value = candidate.milliseconds ? sinceMs : sinceMs / 1000;

    m_whiteboardClient->fetchWithCursor(path, candidate.typeCode, value,
            [this, index, candidate, path, value]
            (bool ok, const std::vector<uint8_t> &body, const QString &error) {
        if (!ok) {
            // A transport failure is not a verdict on the encoding, so the
            // sweep stops rather than blaming the next candidate for it.
            m_logbookTestInFlight = false;
            emit logbookTestResult(tr("%1 (%2): %3")
                                    .arg(path, QString::fromLatin1(candidate.label), error));
            return;
        }
        if (isRejection(body)) {
            tryActivityTrendEncoding(index + 1);
            return;
        }

        m_logbookTestInFlight = false;

        // The whole reply goes to a file, header included. The status word
        // in that header is the point of the exercise: libmds treats 202 as
        // "more fragments follow" and 200 as the last one, where every
        // other paged resource here uses 100/200. Retyping a hex dump is
        // not a substitute for having the bytes.
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        QFile dump(dir + QStringLiteral("/trenddata.bin"));
        if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            dump.write(reinterpret_cast<const char *>(body.data()),
                        static_cast<int>(body.size()));
            dump.close();
        }

        QString status = QStringLiteral("?");
        if (body.size() >= 8) {
            status = QString::number(static_cast<uint16_t>(
                    body[6] | (static_cast<uint16_t>(body[7]) << 8)));
        }
        QString hex;
        for (size_t i = 0; i < body.size() && i < 64; ++i)
            hex += QStringLiteral("%1 ").arg(body[i], 2, 16, QLatin1Char('0'));

        emit logbookTestResult(tr("/Activity/TrendData accepted %1 (cursor %2)\n"
                                   "%3 bytes, status %4, saved as trenddata.bin\n%5")
                                .arg(QString::fromLatin1(candidate.label))
                                .arg(value).arg(body.size()).arg(status, hex.trimmed()));
    });
}

void AppController::loginToCloud(const QString &email, const QString &password)
{
    if (m_cloudLoginInProgress)
        return;
    m_cloudLoginInProgress = true;
    emit cloudLoginInProgressChanged();

    m_cloudClient->login(email, password,
            [this](bool ok, const SuuntoCloudClient::Session &session, const QString &error) {
        m_cloudLoginInProgress = false;
        emit cloudLoginInProgressChanged();

        if (!ok) {
            emit errorOccurred(tr("Cloud login failed: %1").arg(error));
            return;
        }

        // Sessionkey is the only credential this API hands out (no separate
        // refresh token - see suuntocloudclient.h) - stored under the same
        // secret name Phase 2a already reserved for it.
        m_tokenVault->storeSecret(CloudAccountStore::TokenSecretName, session.sessionKey.toUtf8(),
                [this](bool storeOk, const QString &storeError) {
            if (!storeOk)
                emit errorOccurred(tr("Failed to store login session: %1").arg(storeError));
        });

        CloudAccount account;
        account.email = session.email;
        account.athleteId = session.userKey;
        account.tokenExpiry = 0; // no documented expiry for this sessionkey
        account.lastSync = 0;
        QString saveError;
        if (!m_cloudAccountStore->save(account, &saveError)) {
            emit errorOccurred(tr("Failed to save account: %1").arg(saveError));
            return;
        }
        m_cloudAccount = account;
        m_cloudClient->setAccountEmail(m_cloudAccount.email);
        emit cloudAccountChanged();
    });
}

void AppController::logoutFromCloud()
{
    QString error;
    if (!m_cloudAccountStore->clear(&error))
        emit errorOccurred(tr("Failed to clear account: %1").arg(error));
    m_tokenVault->deleteSecret(CloudAccountStore::TokenSecretName, [](bool, const QString &) {});
    m_cloudAccount = CloudAccount();
    m_cloudClient->setAccountEmail(QString());
    emit cloudAccountChanged();
}

// The four timeline kinds, in the order the page shows them. Names are the
// cloud's own path segments - see SuuntoCloudClient::fetchHealthEntries().
static const char *const kHealthKinds[] = {
    "sleep", "sleepstages", "recovery", "activity",
};
static const int kHealthKindCount = 4;

void AppController::syncHealthData()
{
    if (m_healthSyncInProgress)
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }
    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();
    fetchHealthKindAt(0, 0, QStringList());
}

void AppController::fetchHealthKindAt(int index, int fetched, const QStringList &failures)
{
    if (index >= kHealthKindCount) {
        m_healthSyncInProgress = false;
        emit healthSyncInProgressChanged();
        emit healthDataChanged();
        // Silent on full success, same as the workout syncs.
        if (!failures.isEmpty()) {
            emit errorOccurred(tr("Synced %1 health entries (%2)")
                                  .arg(fetched).arg(failures.join(QStringLiteral("; "))));
        }
        return;
    }

    const QString kind = QString::fromLatin1(kHealthKinds[index]);
    // Ask only for what we don't have. The server's filter is inclusive, so
    // the newest stored entry comes back again and the (kind, timestamp)
    // primary key absorbs it.
    const qint64 since = m_healthStore->newestTimestamp(kind);

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, index, fetched, failures, kind, since](
                    bool ok, const QByteArray &data, const QString &vaultError) {
        if (!ok) {
            QStringList next = failures;
            next.append(tr("%1: %2").arg(kind, vaultError));
            fetchHealthKindAt(index + 1, fetched, next);
            return;
        }
        m_cloudClient->fetchHealthEntries(QString::fromUtf8(data), kind, since,
                [this, index, fetched, failures, kind](
                        bool fetchOk, const QVector<HealthEntry> &entries,
                        const QString &fetchError) {
            QStringList next = failures;
            int total = fetched;
            if (!fetchOk) {
                next.append(tr("%1: %2").arg(kind, fetchError));
            } else {
                QString saveError;
                if (!m_healthStore->upsert(entries, &saveError))
                    next.append(tr("%1: %2").arg(kind, saveError));
                else
                    total += entries.size();
            }
            fetchHealthKindAt(index + 1, total, next);
        });
    });
}

bool AppController::canUploadWorkout(const QString &key) const
{
    QByteArray data;
    return m_cloudAccount.isSignedIn()
            && m_workoutStore->loadSmlSources(key, &data, nullptr)
            && !m_workoutStore->isSmlUploaded(key);
}

// Rebuilds the upload payload from the stored raw /Data and /Summary.
// Empty if this workout has none - which is the case for anything synced
// before saveSmlSources() existed, and for a cloud workout.
QByteArray AppController::buildUploadZip(const QString &key) const
{
    QByteArray data, summary;
    if (!m_workoutStore->loadSmlSources(key, &data, &summary))
        return QByteArray();

    // Timestamps come from the stored workout, not from "now".
    qint64 startTime = 0;
    qint64 stopTime = 0;
    for (const Workout &w : m_workoutStore->loadAll(nullptr)) {
        if (w.key == key) {
            startTime = w.startTime;
            stopTime = w.stopTime;
            break;
        }
    }

    const std::vector<uint8_t> dataVec(
            reinterpret_cast<const uint8_t *>(data.constData()),
            reinterpret_cast<const uint8_t *>(data.constData()) + data.size());
    const std::vector<uint8_t> summaryVec(
            reinterpret_cast<const uint8_t *>(summary.constData()),
            reinterpret_cast<const uint8_t *>(summary.constData()) + summary.size());
    return buildSmlZip(dataVec, summaryVec, smlSourceFor(m_pairedWatch.name),
                        startTime, stopTime, m_descriptorTable);
}

int AppController::pendingWorkoutUploads() const
{
    if (!m_cloudAccount.isSignedIn())
        return 0;
    return m_workoutStore->keysPendingUpload().size();
}

void AppController::uploadAllWorkouts()
{
    if (m_uploadInProgress) {
        emit errorOccurred(tr("An upload is already in progress."));
        return;
    }
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    const QVector<QString> keys = m_workoutStore->keysPendingUpload();
    if (keys.isEmpty()) {
        emit errorOccurred(tr("Nothing to upload - every watch workout is already in the cloud."));
        return;
    }

    m_uploadInProgress = true;
    uploadWorkoutAt(keys, 0, 0, QStringList());
}

void AppController::uploadWorkoutAt(const QVector<QString> &keys, int index, int succeeded,
                                     const QStringList &failures)
{
    if (index >= keys.size()) {
        m_uploadInProgress = false;
        emit workoutUploadProgress(keys.size(), keys.size());
        if (failures.isEmpty()) {
            emit errorOccurred(tr("Uploaded %1 workouts to Suunto.").arg(succeeded));
        } else {
            emit errorOccurred(tr("Uploaded %1 of %2 workouts. Failed: %3")
                                .arg(succeeded).arg(keys.size())
                                .arg(failures.join(QStringLiteral("; "))));
        }
        loadCachedWorkouts();
        return;
    }

    emit workoutUploadProgress(index, keys.size());

    const QString key = keys.at(index);
    const QByteArray zip = buildUploadZip(key);
    if (zip.isEmpty()) {
        QStringList next = failures;
        next.append(tr("%1: nothing to build an upload from").arg(key));
        uploadWorkoutAt(keys, index + 1, succeeded, next);
        return;
    }

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, keys, index, succeeded, failures, key, zip]
            (bool ok, const QByteArray &data, const QString &vaultError) {
        if (!ok) {
            QStringList next = failures;
            next.append(tr("%1: %2").arg(key, vaultError));
            uploadWorkoutAt(keys, index + 1, succeeded, next);
            return;
        }
        m_cloudClient->uploadWorkout(QString::fromUtf8(data), zip,
                [this, keys, index, succeeded, failures, key]
                (bool uploadOk, const QString &cloudKey, const QString &error) {
            QStringList next = failures;
            int done = succeeded;
            if (!uploadOk) {
                next.append(tr("%1: %2").arg(key, error));
            } else {
                m_workoutStore->markSmlUploaded(key, cloudKey, nullptr);
                ++done;
                emit workoutUploaded(key, true, tr("Uploaded to Suunto"));
            }
            uploadWorkoutAt(keys, index + 1, done, next);
        });
    });
}

void AppController::uploadWorkoutToCloud(const QString &key)
{
    if (m_uploadInProgress)
        return;
    const QByteArray zip = buildUploadZip(key);
    if (zip.isEmpty()) {
        emit workoutUploaded(key, false,
                              tr("Nothing to upload - sync this workout from the watch first."));
        return;
    }
    if (m_workoutStore->isSmlUploaded(key)) {
        emit workoutUploaded(key, false, tr("Already uploaded."));
        return;
    }
    if (!m_cloudAccount.isSignedIn()) {
        emit workoutUploaded(key, false, tr("Sign in to the Suunto cloud first."));
        return;
    }

    m_uploadInProgress = true;
    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key, zip](bool ok, const QByteArray &data, const QString &vaultError) {
        if (!ok) {
            m_uploadInProgress = false;
            emit workoutUploaded(key, false,
                                  tr("Could not read the stored session: %1").arg(vaultError));
            return;
        }
        m_cloudClient->uploadWorkout(QString::fromUtf8(data), zip,
                [this, key](bool uploadOk, const QString &cloudKey, const QString &error) {
            m_uploadInProgress = false;
            if (!uploadOk) {
                emit workoutUploaded(key, false, error);
                return;
            }
            m_workoutStore->markSmlUploaded(key, cloudKey, nullptr);
            emit workoutUploaded(key, true, tr("Uploaded to Suunto"));
            // So the list's cloud marker changes over without waiting for
            // the next sync. The batch path reloads once at the end; this
            // one uploads a single workout, so it reloads here.
            loadCachedWorkouts();
        });
    });
}

// Renders a decoded night into the JSON the cloud uses, field for field,
// so both sources land in one table and one page. Names and units are the
// cloud's: quality as 0..1 rather than the file's percent, heart rate left
// in hertz because that is what the cloud stores too.
static QByteArray sleepEntryJson(const SleepTimeline::Night &night)
{
    QJsonObject o;
    o.insert(QStringLiteral("duration"), night.durationSeconds);
    o.insert(QStringLiteral("deepSleepDuration"), night.deepSeconds);
    o.insert(QStringLiteral("lightSleepDuration"), night.lightSeconds);
    o.insert(QStringLiteral("remSleepDuration"), night.remSeconds);
    o.insert(QStringLiteral("sleepOnsetLatencyDuration"), night.onsetLatencySeconds);
    o.insert(QStringLiteral("wakeAfterSleepOnsetDuration"), night.wakeAfterOnsetSeconds);
    o.insert(QStringLiteral("wakeBeforeOffBedDuration"), night.wakeBeforeOffBedSeconds);
    o.insert(QStringLiteral("hrAvg"), night.heartRateAvgHz);
    o.insert(QStringLiteral("hrMin"), night.heartRateMinHz);
    o.insert(QStringLiteral("maxSpo2"), night.maxSpo2);
    o.insert(QStringLiteral("altitude"), night.altitudeMetres);
    // Fields the watch didn't measure are left out entirely rather than
    // sent as a sentinel. The cloud validates ranges and rejects the whole
    // batch otherwise.
    if (night.hrvAverageMs >= 0)
        o.insert(QStringLiteral("avgHrv"), night.hrvAverageMs);
    if (night.hrvSampleCount >= 0)
        o.insert(QStringLiteral("avgHrvSampleCount"), night.hrvSampleCount);
    if (night.qualityPercent >= 0)
        o.insert(QStringLiteral("quality"), night.qualityPercent / 100.0);
    o.insert(QStringLiteral("sleepId"), static_cast<qint64>(night.sleepId));
    o.insert(QStringLiteral("isNap"), night.isNap);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static QString stageName(SleepTimeline::Stage stage)
{
    switch (stage) {
    case SleepTimeline::Stage::Awake: return QStringLiteral("AWAKE");
    case SleepTimeline::Stage::Rem:   return QStringLiteral("REM");
    case SleepTimeline::Stage::Light: return QStringLiteral("LIGHT");
    case SleepTimeline::Stage::Deep:  return QStringLiteral("DEEP");
    }
    return QStringLiteral("AWAKE");
}

void AppController::syncWatchHealth()
{
    if (!m_whiteboardReady) {
        emit errorOccurred(tr("Connect the watch first."));
        return;
    }
    if (m_healthSyncInProgress || m_workoutSyncInProgress) {
        emit errorOccurred(tr("A sync is already in progress."));
        return;
    }

    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();

    // Ask for everything newer than what is already stored, falling back to
    // a fortnight on an empty database - the watch keeps a limited history
    // anyway, so asking for more costs nothing.
    qint64 since = m_healthStore->newestTimestamp(QStringLiteral("sleep"));
    if (since == 0)
        since = QDateTime::currentMSecsSinceEpoch() - 14LL * 24 * 3600 * 1000;

    m_whiteboardClient->fetchTimelineFile(
            QStringLiteral("/Daily/Sleep/Timeline/Data"),
            QStringLiteral("mdsSlp.sbm"), since,
            [this](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        m_healthSyncInProgress = false;
        emit healthSyncInProgressChanged();

        if (!ok) {
            emit errorOccurred(tr("Could not read sleep from the watch: %1").arg(error));
            return;
        }

        const std::vector<SleepTimeline::Night> nights =
                SleepTimeline::decode(Sbem::parseContainer(data));
        if (nights.empty()) {
            emit errorOccurred(tr("The watch returned %1 bytes but no readable nights.")
                                .arg(data.size()));
            return;
        }

        QVector<HealthEntry> entries;
        for (const SleepTimeline::Night &night : nights) {
            HealthEntry entry;
            entry.kind = QStringLiteral("sleep");
            entry.timestamp = night.startMs;
            entry.data = sleepEntryJson(night);
            entries.append(entry);

            for (const SleepTimeline::StageSample &stage : night.stages) {
                QJsonObject o;
                o.insert(QStringLiteral("stage"), stageName(stage.stage));
                o.insert(QStringLiteral("duration"),
                          static_cast<double>(stage.durationSeconds));
                HealthEntry stageEntry;
                stageEntry.kind = QStringLiteral("sleepstages");
                stageEntry.timestamp = stage.startMs;
                stageEntry.data = QJsonDocument(o).toJson(QJsonDocument::Compact);
                entries.append(stageEntry);
            }
        }

        QString storeError;
        // fromWatch: these rows are candidates for pushing to the cloud.
        if (!m_healthStore->upsert(entries, &storeError, true)) {
            emit errorOccurred(tr("Could not save sleep data: %1").arg(storeError));
            return;
        }
        emit healthDataChanged();

        // Recovery comes from a different resource and needs no rendered
        // file, so it is a separate fetch chained after this one rather
        // than part of the same reply.
        fetchWatchRecovery();
    });
}

void AppController::fetchWatchRecovery()
{
    qint64 since = m_healthStore->newestTimestamp(QStringLiteral("recovery"));
    if (since == 0)
        since = QDateTime::currentMSecsSinceEpoch() - 14LL * 24 * 3600 * 1000;

    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();

    // Seconds, not milliseconds - see fetchRecoveryMoments().
    m_whiteboardClient->fetchRecoveryMoments(since / 1000,
            [this](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        if (!ok) {
            // Sleep already succeeded by this point, so this is reported
            // but not treated as a failed sync - and daily activity is a
            // third resource again, so it still gets its turn.
            emit errorOccurred(tr("Sleep synced. Recovery failed: %1").arg(error));
            fetchWatchActivity();
            return;
        }

        const std::vector<RecoveryMoments::Sample> samples = RecoveryMoments::decode(data);
        if (samples.empty()) {
            emit errorOccurred(tr("Sleep synced. Recovery returned %1 bytes but no readable samples.")
                                .arg(data.size()));
            fetchWatchActivity();
            return;
        }

        QVector<HealthEntry> entries;
        entries.reserve(samples.size());
        for (const RecoveryMoments::Sample &sample : samples) {
            QJsonObject o;
            // The cloud's own field names and scale: balance 0..1.
            o.insert(QStringLiteral("balance"), sample.balancePercent / 100.0);
            o.insert(QStringLiteral("stressState"), sample.stressState);
            HealthEntry entry;
            entry.kind = QStringLiteral("recovery");
            entry.timestamp = sample.timestampMs;
            entry.data = QJsonDocument(o).toJson(QJsonDocument::Compact);
            entries.append(entry);
        }

        QString storeError;
        if (!m_healthStore->upsert(entries, &storeError, true)) {
            emit errorOccurred(tr("Could not save recovery data: %1").arg(storeError));
            fetchWatchActivity();
            return;
        }
        emit healthDataChanged();

        // Daily activity: a third resource, a third mechanism. Chained
        // rather than fired alongside, because the whiteboard link takes
        // one request at a time.
        fetchWatchActivity();
    });
}

void AppController::fetchWatchActivity()
{
    qint64 since = m_healthStore->newestTimestamp(QStringLiteral("activity"));
    if (since == 0)
        since = QDateTime::currentMSecsSinceEpoch() - 14LL * 24 * 3600 * 1000;
    // No +1 to skip the last stored bucket: the cursor is not a strict
    // lower bound (see fetchActivityTrend()), so the watch may hand back
    // earlier buckets regardless. Re-reading a few is free - the store
    // upserts on (kind, timestamp).

    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();

    // Milliseconds here, where recovery counts seconds. libmds.so settles
    // it: the official app multiplies its own NewerThan by 1000 before
    // this fetch. See docs/watch-push-resources.md.
    m_whiteboardClient->fetchActivityTrend(since,
            [this](bool ok, const std::vector<ActivityTrend::Sample> &samples,
                    const QString &error) {
        m_healthSyncInProgress = false;
        emit healthSyncInProgressChanged();

        if (!ok) {
            emit errorOccurred(tr("Sleep and recovery synced. Daily activity failed: %1")
                                .arg(error));
            return;
        }
        if (samples.empty()) {
            // Not an error: an up-to-date watch legitimately has nothing
            // newer than the last bucket already stored.
            emit healthDataChanged();
            return;
        }

        QVector<HealthEntry> entries;
        entries.reserve(samples.size());
        for (const ActivityTrend::Sample &sample : samples) {
            QJsonObject o;
            // The cloud's own field names and units, so a bucket read from
            // the watch and the same bucket read from the cloud are the
            // same row: energy unconverted, heart rate in hertz.
            o.insert(QStringLiteral("energyConsumption"),
                      static_cast<double>(sample.energy));
            o.insert(QStringLiteral("stepCount"), sample.stepCount);
            if (sample.heartRateBpm > 0) {
                // Omitted rather than sent as zero when the watch measured
                // none - the same lesson as the sleep upload, where a
                // sentinel passed through as a real reading and the server
                // rejected the lot.
                o.insert(QStringLiteral("hr"), sample.heartRateBpm / 60.0);
            }
            HealthEntry entry;
            entry.kind = QStringLiteral("activity");
            entry.timestamp = sample.timestampMs;
            entry.data = QJsonDocument(o).toJson(QJsonDocument::Compact);
            entries.append(entry);
        }

        QString storeError;
        if (!m_healthStore->upsert(entries, &storeError, true)) {
            emit errorOccurred(tr("Could not save activity data: %1").arg(storeError));
            return;
        }
        emit healthDataChanged();
    });
}

void AppController::uploadHealthToCloud()
{
    if (m_healthSyncInProgress) {
        emit errorOccurred(tr("A sync is already in progress."));
        return;
    }
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }
    if (m_healthStore->pendingUploadCount() == 0) {
        emit errorOccurred(tr("Nothing to upload - everything the watch gave us is already in the cloud."));
        return;
    }

    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();
    uploadHealthKindAt(0, 0, QStringList());
}

void AppController::uploadHealthKindAt(int index, int sent, const QStringList &failures)
{
    if (index >= kHealthKindCount) {
        m_healthSyncInProgress = false;
        emit healthSyncInProgressChanged();
        emit healthDataChanged();
        if (!failures.isEmpty()) {
            emit errorOccurred(tr("Uploaded %1 entries (%2)")
                                .arg(sent).arg(failures.join(QStringLiteral("; "))));
        } else if (sent > 0) {
            emit errorOccurred(tr("Uploaded %1 health entries to Suunto.").arg(sent));
        }
        return;
    }

    const QString kind = QString::fromLatin1(kHealthKinds[index]);
    // A night is a handful of rows but sleep stages run to dozens per
    // night, so this is capped per request rather than sending a year in
    // one POST.
    const QVector<HealthEntry> pending = m_healthStore->loadPendingUpload(kind, 200);
    if (pending.isEmpty()) {
        uploadHealthKindAt(index + 1, sent, failures);
        return;
    }

    // Stamp with the offset in force when the entry was recorded, the same
    // rule the workout upload uses.
    const int offsetMinutes =
            QDateTime::fromMSecsSinceEpoch(pending.first().timestamp).offsetFromUtc() / 60;

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, index, sent, failures, kind, pending, offsetMinutes]
            (bool ok, const QByteArray &data, const QString &vaultError) {
        if (!ok) {
            QStringList next = failures;
            next.append(tr("%1: %2").arg(kind, vaultError));
            uploadHealthKindAt(index + 1, sent, next);
            return;
        }
        m_cloudClient->uploadHealthEntries(QString::fromUtf8(data), kind, pending, offsetMinutes,
                [this, index, sent, failures, kind, pending]
                (bool uploadOk, const QString &error) {
            QStringList next = failures;
            int total = sent;
            if (!uploadOk) {
                next.append(tr("%1: %2").arg(kind, error));
            } else {
                QVector<qint64> stamps;
                stamps.reserve(pending.size());
                for (const HealthEntry &e : pending)
                    stamps.append(e.timestamp);
                m_healthStore->markUploaded(kind, stamps, nullptr);
                total += pending.size();
            }
            uploadHealthKindAt(index + 1, total, next);
        });
    });
}

int AppController::pendingHealthUploads() const
{
    return m_healthStore->pendingUploadCount();
}

void AppController::setCoverMode(const QString &mode)
{
    if (mode == m_coverMode)
        return;
    m_coverMode = mode;
    QSettings().setValue(QStringLiteral("cover/mode"), mode);
    emit coverModeChanged();
}

void AppController::setSyncOnConnect(bool enabled)
{
    if (enabled == m_syncOnConnect)
        return;
    m_syncOnConnect = enabled;
    QSettings().setValue(QStringLiteral("sync/onConnect"), enabled);
    emit syncOnConnectChanged();
}

QVariantMap AppController::coverSummary() const
{
    QVariantMap out;

    const QVector<Workout> workouts = m_workoutStore->loadAll(nullptr);
    if (!workouts.isEmpty()) {
        // loadAll is newest first.
        const Workout &newest = workouts.first();
        out.insert(QStringLiteral("hasWorkout"), true);
        out.insert(QStringLiteral("activityId"), newest.activityId);
        out.insert(QStringLiteral("source"), newest.source);
        out.insert(QStringLiteral("startTime"), newest.startTime);
        out.insert(QStringLiteral("distance"), newest.totalDistance);
        out.insert(QStringLiteral("duration"), newest.totalTime);

        double totalDistance = 0;
        double totalTime = 0;
        for (const Workout &w : workouts) {
            totalDistance += w.totalDistance;
            totalTime += w.totalTime;
        }
        out.insert(QStringLiteral("count"), workouts.size());
        out.insert(QStringLiteral("totalDistance"), totalDistance);
        out.insert(QStringLiteral("totalTime"), totalTime);
    } else {
        out.insert(QStringLiteral("hasWorkout"), false);
    }

    const QVector<HealthEntry> nights = m_healthStore->load(QStringLiteral("sleep"), 1, nullptr);
    if (!nights.isEmpty()) {
        const QJsonObject o = QJsonDocument::fromJson(nights.first().data).object();
        out.insert(QStringLiteral("hasSleep"), true);
        out.insert(QStringLiteral("sleepStart"), nights.first().timestamp);
        out.insert(QStringLiteral("sleepDuration"),
                    o.value(QStringLiteral("duration")).toDouble());
        out.insert(QStringLiteral("sleepQuality"),
                    o.value(QStringLiteral("quality")).toDouble());
    } else {
        out.insert(QStringLiteral("hasSleep"), false);
    }

    return out;
}

QVariantList AppController::healthEntries(const QString &kind, int limit) const
{
    QVariantList out;
    for (const HealthEntry &e : m_healthStore->load(kind, limit, nullptr)) {
        QVariantMap row;
        row.insert(QStringLiteral("timestamp"), e.timestamp);
        // The payload's own field names, passed through. A sleep entry has
        // eighteen and a recovery entry two; the page decides what to show.
        const QJsonObject obj = QJsonDocument::fromJson(e.data).object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
            row.insert(it.key(), it.value().toVariant());
        out.append(row);
    }
    return out;
}

QVariantMap AppController::healthOverview(const QString &kind) const
{
    QVariantMap out;
    const qint64 newest = m_healthStore->newestTimestamp(kind);
    if (newest == 0)
        return out;
    out.insert(QStringLiteral("newest"), newest);
    out.insert(QStringLiteral("count"), m_healthStore->load(kind, 0, nullptr).size());
    return out;
}

void AppController::loadCachedWorkouts()
{
    QString error;
    const QVector<Workout> workouts = m_workoutStore->loadAll(&error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to load workouts: %1").arg(error));
        return;
    }
    m_workoutModel->setWorkouts(workouts);
}

void AppController::syncCloudWorkouts()
{
    if (!m_cloudAccount.isSignedIn() || m_workoutSyncInProgress)
        return;

    m_workoutSyncInProgress = true;
    emit workoutSyncInProgressChanged();

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this](bool ok, const QByteArray &data, const QString &loadError) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();
            emit errorOccurred(tr("Failed to load login session: %1").arg(loadError));
            return;
        }

        const QString sessionKey = QString::fromUtf8(data);
        m_cloudClient->listWorkouts(sessionKey, 100,
                [this](bool listOk, const QVector<Workout> &workouts, const QString &listError) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();

            if (!listOk) {
                emit errorOccurred(tr("Failed to sync workouts: %1").arg(listError));
                return;
            }

            for (const Workout &w : workouts) {
                // The cloud carries the route as an encoded polyline in the
                // same list response, so a cloud workout gets a map too -
                // stored in exactly the format a BLE one uses, which means
                // the drawing code doesn't care where it came from.
                if (!w.polyline.isEmpty()) {
                    const auto points = Polyline::decode(w.polyline.toStdString());
                    if (!points.empty())
                        m_workoutStore->saveRoute(w.key, packTrack(points), nullptr);
                }

                QString storeError;
                if (!m_workoutStore->upsert(w, &storeError)) {
                    emit errorOccurred(tr("Failed to save workout: %1").arg(storeError));
                    return;
                }
            }

            // currentSecsSinceEpoch() is Qt 5.8+ - newer than Sailfish OS's
            // Qt5 (same vintage issue as QRandomGenerator elsewhere in this
            // project); currentMSecsSinceEpoch() has been available since
            // Qt 4.7 and works everywhere.
            m_cloudAccount.lastSync = QDateTime::currentMSecsSinceEpoch() / 1000;
            QString saveError;
            if (!m_cloudAccountStore->save(m_cloudAccount, &saveError))
                emit errorOccurred(tr("Failed to save account: %1").arg(saveError));

            loadCachedWorkouts();
        });
    });
}

void AppController::loadCloudDetails(const QString &key)
{
    // Only worth a request for a cloud workout we haven't already fetched -
    // a BLE one's details come from the watch, and re-fetching on every
    // page open would be a request per tap.
    if (!m_cloudAccount.isSignedIn() || key.startsWith(QStringLiteral("ble_"))
            || !m_workoutStore->loadDetails(key).isEmpty()) {
        return;
    }

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key](bool ok, const QByteArray &data, const QString &) {
        if (!ok)
            return;
        m_cloudClient->fetchWorkoutDetail(QString::fromUtf8(data), key,
                [this, key](bool detailOk, const QJsonObject &payload, const QString &) {
            // Silent on failure: this is an enrichment, and a workout that
            // shows its summary without the extras is still useful.
            if (!detailOk)
                return;
            const QByteArray json = cloudDetailsJson(payload);
            if (json.isEmpty())
                return;
            m_workoutStore->saveDetails(key, json, nullptr);

            // Promote the three the watch also reports, so a cloud workout
            // shows them as proper stats rather than only as rows in the
            // field table. The cloud's units match the watch's here
            // (ml/kg, 1-5, seconds), so no conversion.
            const QJsonObject fields = QJsonDocument::fromJson(json).object();
            auto number = [&fields](const char *name) {
                return fields.value(QLatin1String(name)).toObject()
                        .value(QStringLiteral("value")).toDouble();
            };
            const double epoc = number("SummaryExtension.peakEpoc");
            const double pte = number("SummaryExtension.pte");
            const double recovery = number("SummaryExtension.recoveryTime");
            if (epoc > 0 || pte > 0 || recovery > 0) {
                m_workoutStore->updateTrainingMetrics(key, epoc, pte, recovery, nullptr);
                loadCachedWorkouts();
            }

            emit workoutDetailsChanged(key);
        });
    });
}

void AppController::loadCloudSamples(const QString &key)
{
    if (m_cloudSamplesInProgress)
        return;
    if (!m_cloudAccount.isSignedIn() || key.startsWith(QStringLiteral("ble_"))) {
        emit errorOccurred(tr("Sample data is only available for cloud workouts."));
        return;
    }

    m_cloudSamplesInProgress = true;
    emit cloudSamplesInProgressChanged();

    // Unlike loadCloudDetails() this reports its failures: the user asked
    // for this one, so silence would just look broken.
    auto finish = [this, key](const QString &error) {
        m_cloudSamplesInProgress = false;
        emit cloudSamplesInProgressChanged();
        if (!error.isEmpty())
            emit errorOccurred(error);
        else
            emit workoutDetailsChanged(key);
    };

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key, finish](bool ok, const QByteArray &data, const QString &error) {
        if (!ok) {
            finish(tr("Could not read the stored session: %1").arg(error));
            return;
        }
        m_cloudClient->fetchWorkoutSml(QString::fromUtf8(data), key,
                [this, key, finish](bool smlOk, const QByteArray &body, const QString &smlError) {
            if (!smlOk) {
                finish(tr("Could not download sample data: %1").arg(smlError));
                return;
            }

            const QByteArray series = buildCloudSeriesJson(body);
            if (series.isEmpty()) {
                // Nothing recognised. Keep the body rather than throwing it
                // away - this is the only copy of the shape this parser was
                // written blind against. See loadCloudSamples()'s header
                // comment.
                const QString dir =
                        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
                QDir().mkpath(dir);
                const QString path = dir + QStringLiteral("/sml-") + key + QStringLiteral(".json");
                QFile file(path);
                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    file.write(body);
                    file.close();
                    finish(tr("No charts found in %1 kB of sample data. Raw response saved to %2.")
                           .arg(body.size() / 1024).arg(path));
                } else {
                    finish(tr("No charts found in the sample data, and it could not be saved."));
                }
                return;
            }

            m_workoutStore->saveSeries(key, series, nullptr);
            finish(QString());
        });
    });
}

QVariantList AppController::workoutDetails(const QString &key) const
{
    const QJsonObject fields =
            QJsonDocument::fromJson(m_workoutStore->loadDetails(key)).object();

    QVariantList out;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const double value = entry.value(QStringLiteral("value")).toDouble();
        // Skip fields the watch didn't record. Zero is the schema's own
        // "absent" marker for most of these (nillable=0), and a screen of
        // a hundred zeroes would bury the dozen that mean something.
        if (qFuzzyIsNull(value))
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("name"), it.key());
        row.insert(QStringLiteral("value"), value);
        row.insert(QStringLiteral("unit"), entry.value(QStringLiteral("unit")).toString());
        out.append(row);
    }
    return out;
}

QVariantList AppController::workoutLaps(const QString &key) const
{
    const QJsonArray laps = QJsonDocument::fromJson(m_workoutStore->loadLaps(key)).array();
    QVariantList out;
    for (const QJsonValue &value : laps) {
        const QJsonObject lap = value.toObject();
        QVariantMap row;
        row.insert(QStringLiteral("number"), lap.value(QStringLiteral("number")).toInt());
        row.insert(QStringLiteral("type"), lap.value(QStringLiteral("type")).toString());
        row.insert(QStringLiteral("durationSeconds"),
                    lap.value(QStringLiteral("durationSeconds")).toDouble());
        row.insert(QStringLiteral("distanceMeters"),
                    lap.value(QStringLiteral("distanceMeters")).toDouble());
        out.append(row);
    }
    return out;
}

QVariantList AppController::workoutSeries(const QString &key) const
{
    const QJsonArray series =
            QJsonDocument::fromJson(m_workoutStore->loadSeries(key)).array();

    QVariantList out;
    for (const QJsonValue &value : series) {
        const QJsonObject entry = value.toObject();
        QVariantList points;
        for (const QJsonValue &point : entry.value(QStringLiteral("points")).toArray())
            points.append(point.toDouble());

        QVariantMap row;
        row.insert(QStringLiteral("name"), entry.value(QStringLiteral("name")).toString());
        row.insert(QStringLiteral("unit"), entry.value(QStringLiteral("unit")).toString());
        row.insert(QStringLiteral("min"), entry.value(QStringLiteral("min")).toDouble());
        row.insert(QStringLiteral("max"), entry.value(QStringLiteral("max")).toDouble());
        row.insert(QStringLiteral("points"), points);
        out.append(row);
    }
    return out;
}

QVariantList AppController::workoutRoute(const QString &key) const
{
    const QByteArray packed = m_workoutStore->loadRoute(key);
    const int count = packed.size() / (2 * static_cast<int>(sizeof(qint32)));
    QVariantList points;
    if (count < 2)
        return points;

    QVector<double> lats, lons;
    lats.reserve(count);
    lons.reserve(count);
    const char *p = packed.constData();
    for (int i = 0; i < count; ++i) {
        qint32 lat = 0, lon = 0;
        std::memcpy(&lat, p, sizeof(qint32));
        p += sizeof(qint32);
        std::memcpy(&lon, p, sizeof(qint32));
        p += sizeof(qint32);
        lats.append(lat / 1e7);
        lons.append(lon / 1e7);
    }

    const auto latRange = std::minmax_element(lats.begin(), lats.end());
    const auto lonRange = std::minmax_element(lons.begin(), lons.end());
    const double minLat = *latRange.first, maxLat = *latRange.second;
    const double minLon = *lonRange.first, maxLon = *lonRange.second;

    // Equirectangular: a degree of longitude covers cos(latitude) as much
    // ground as a degree of latitude, so scale it that way before fitting,
    // otherwise a route at 60N comes out stretched to twice its real width.
    const double lonScale = std::cos(qDegreesToRadians((minLat + maxLat) / 2.0));
    const double spanX = (maxLon - minLon) * lonScale;
    const double spanY = maxLat - minLat;
    const double span = std::max(spanX, spanY);
    if (span <= 0)
        return points;

    // Centre the smaller axis so the shape keeps its proportions.
    const double offsetX = (span - spanX) / 2.0;
    const double offsetY = (span - spanY) / 2.0;

    for (int i = 0; i < count; ++i) {
        QVariantMap point;
        point.insert(QStringLiteral("x"), ((lons[i] - minLon) * lonScale + offsetX) / span);
        // y inverted: north should be up, canvas y grows downwards.
        point.insert(QStringLiteral("y"), 1.0 - ((lats[i] - minLat) + offsetY) / span);
        points.append(point);
    }
    return points;
}

void AppController::syncWatchWorkouts()
{
    if (!m_whiteboardReady || m_workoutSyncInProgress || m_logbookTestInFlight)
        return;

    m_workoutSyncInProgress = true;
    emit workoutSyncInProgressChanged();

    m_whiteboardClient->fetchLogEntries(QStringLiteral("/Logbook/Entries"),
            [this](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            emit workoutSyncInProgressChanged();
            emit errorOccurred(tr("Failed to list watch entries: %1").arg(error));
            return;
        }

        QVector<QString> logbookIds;
        logbookIds.reserve(static_cast<int>(entries.size()));
        for (const LogEntries::Entry &entry : entries)
            logbookIds.append(QString::number(entry.id));

        // Read the watch's own field table first, once per watch: which
        // chunk carries GPS or heart rate is per model, and decoding with
        // the wrong table produces a workout of zeros rather than an
        // error. See ensureDescriptors().
        ensureDescriptors(logbookIds.first(), [this, logbookIds]() {
            fetchWatchEntryAt(logbookIds, 0, 0, {});
        });
    });
}

void AppController::loadStoredDescriptors()
{
    const QString address = m_pairedWatch.address;
    if (address.isEmpty() || m_descriptorAddress == address)
        return;

    // Read once per watch and kept: 25 kB over BLE on every sync would be
    // wasteful, and the table only changes with a firmware update.
    const QByteArray stored = m_pairedWatchStore->loadDescriptors(address);
    if (stored.isEmpty())
        return;

    const std::vector<uint8_t> payload(stored.begin(), stored.end());
    SbemDescriptors::Table table = SbemDescriptors::Table::parse(payload);
    if (table.empty())
        return;

    m_descriptorTable = std::move(table);
    m_descriptorLayout = SbemLayout::resolve(m_descriptorTable);
    m_descriptorAddress = address;

    // Anything this watch's bytes could not be read with before, can be
    // now. Cheap: almost always nothing to do.
    redecodeStoredWorkouts();
}

int AppController::redecodeStoredWorkouts()
{
    int repaired = 0;
    for (const QString &key : m_workoutStore->keysWithEmptyDecode()) {
        QByteArray data, summary;
        if (!m_workoutStore->loadSmlSources(key, &data, &summary) || data.isEmpty())
            continue;

        const std::vector<uint8_t> dataVec(
                reinterpret_cast<const uint8_t *>(data.constData()),
                reinterpret_cast<const uint8_t *>(data.constData()) + data.size());

        Workout w;
        std::vector<Logbook::TrackPoint> track;
        try {
            const Logbook::DecodedWorkout decoded = Logbook::decode(dataVec, m_descriptorLayout);
            // "ble_<id>" - the same derivation the sync uses, so the key
            // this writes back is the key it read.
            const QString logbookId = key.mid(4);
            w = workoutFromDecoded(logbookId, decoded);
            track = decoded.track;
        } catch (const std::exception &) {
            continue; // unchanged, same as before
        }

        std::vector<uint8_t> summaryVec;
        if (!summary.isEmpty()) {
            summaryVec.assign(reinterpret_cast<const uint8_t *>(summary.constData()),
                               reinterpret_cast<const uint8_t *>(summary.constData())
                                       + summary.size());
            applySummary(&w, Summary::decode(summaryVec, m_descriptorLayout));
        }

        // Only a decode that actually produced something replaces the row.
        // A workout belonging to a *different* watch decodes to zeros
        // against this table, and leaving it alone is the right answer.
        if (w.totalTime == 0 && w.totalDistance == 0 && w.avgHeartRate == 0)
            continue;

        if (!m_workoutStore->upsert(w, nullptr))
            continue;
        ++repaired;

        if (!track.empty())
            m_workoutStore->saveRoute(w.key, packTrack(track), nullptr);
        if (!summaryVec.empty()) {
            m_workoutStore->saveDetails(w.key, summaryDetailsJson(summaryVec, m_descriptorTable),
                                         nullptr);
        }
        const QByteArray seriesJson = buildSeriesJson(dataVec, m_descriptorTable);
        if (!seriesJson.isEmpty())
            m_workoutStore->saveSeries(w.key, seriesJson, nullptr);
        const QByteArray lapsJson = buildLapsJson(dataVec, m_descriptorTable);
        if (!lapsJson.isEmpty())
            m_workoutStore->saveLaps(w.key, lapsJson, nullptr);
    }

    if (repaired > 0)
        loadCachedWorkouts();
    return repaired;
}

void AppController::ensureDescriptors(const QString &logbookId,
                                       const std::function<void()> &then)
{
    loadStoredDescriptors();

    const QString address = m_pairedWatch.address;
    if (address.isEmpty() || m_descriptorAddress == address) {
        then();
        return;
    }

    // The resource is addressed per workout, but the table it returns
    // describes the watch, not that workout - any id will do.
    const QString path = QStringLiteral("/Logbook/byId/%1/Descriptors").arg(logbookId);
    m_whiteboardClient->fetchSummary(path,
            [this, address, then](bool ok, const std::vector<uint8_t> &data, const QString &) {
        if (ok) {
            SbemDescriptors::Table table = SbemDescriptors::Table::parse(data);
            if (!table.empty()) {
                m_descriptorTable = std::move(table);
                m_descriptorLayout = SbemLayout::resolve(m_descriptorTable);
                m_descriptorAddress = address;
                const int repaired = redecodeStoredWorkouts();
                if (repaired > 0) {
                    emit errorOccurred(tr("Re-read %1 stored workouts with this watch's own "
                                           "field table.").arg(repaired));
                }
                m_pairedWatchStore->saveDescriptors(
                        address,
                        QByteArray(reinterpret_cast<const char *>(data.data()),
                                    static_cast<int>(data.size())),
                        nullptr);
            }
        }
        // Deliberately not an error if this failed: the built-in table is
        // what every version before this used, so falling back to it is
        // no worse than before. A watch it doesn't fit decodes to zeros,
        // which is visible in the list rather than silent.
        then();
    });
}

void AppController::fetchWatchEntryAt(const QVector<QString> &logbookIds, int index,
                                       int succeeded, const QStringList &failures)
{
    if (index >= logbookIds.size()) {
        m_workoutSyncInProgress = false;
        emit workoutSyncInProgressChanged();
        loadCachedWorkouts();
        if (!failures.isEmpty()) {
            emit errorOccurred(tr("Synced %1 of %2 watch workouts. Failed: %3")
                                        .arg(succeeded)
                                        .arg(logbookIds.size())
                                        .arg(failures.join(QStringLiteral("; "))));
        }
        return;
    }

    const QString logbookId = logbookIds.at(index);
    const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
    // Capturing "failures" (a const QStringList& parameter) by value would
    // capture it as a *const* QStringList regardless of "mutable" - the
    // const comes from the parameter's own reference type, not from the
    // lambda's default constness, so "mutable" can't strip it. An explicit
    // local copy sidesteps that: capturing a plain (non-reference,
    // non-const) QStringList by value gives an ordinary appendable member.
    QStringList failuresCopy = failures;

    m_whiteboardClient->fetchLogbookData(path,
            [this, logbookIds, index, succeeded, failuresCopy, logbookId]
            (bool ok, const std::vector<uint8_t> &data, const QString &error) mutable {
        if (!ok) {
            failuresCopy.append(tr("%1: %2").arg(logbookId, error));
            fetchWatchEntryAt(logbookIds, index + 1, succeeded, failuresCopy);
            return;
        }

        Workout w;
        std::vector<Logbook::TrackPoint> track;
        try {
            const Logbook::DecodedWorkout decoded = Logbook::decode(data, m_descriptorLayout);
            w = workoutFromDecoded(logbookId, decoded);
            track = decoded.track;
        } catch (const std::exception &e) {
            failuresCopy.append(tr("%1: decode failed (%2)")
                                         .arg(logbookId, QString::fromUtf8(e.what())));
            fetchWatchEntryAt(logbookIds, index + 1, succeeded, failuresCopy);
            return;
        }

        // /Summary carries the watch's own exact totals for the same
        // workout (see summarydecoder.h). It's a bonus, not a
        // prerequisite: if the fetch or decode fails the /Data-derived
        // figures are still worth saving, so this never fails the entry.
        const QString summaryPath = QStringLiteral("/Logbook/byId/%1/Summary").arg(logbookId);
        m_whiteboardClient->fetchSummary(summaryPath,
                [this, logbookIds, index, succeeded, failuresCopy, logbookId, w, track, data]
                (bool summaryOk, const std::vector<uint8_t> &payload, const QString &) mutable {
            if (summaryOk)
                applySummary(&w, Summary::decode(payload, m_descriptorLayout));

            QString storeError;
            if (m_workoutStore->upsert(w, &storeError)) {
                ++succeeded;
                if (!track.empty())
                    m_workoutStore->saveRoute(w.key, packTrack(track), nullptr);
                if (summaryOk)
                    m_workoutStore->saveDetails(w.key, summaryDetailsJson(payload, m_descriptorTable), nullptr);
                const QByteArray seriesJson = buildSeriesJson(data, m_descriptorTable);
                if (!seriesJson.isEmpty())
                    m_workoutStore->saveSeries(w.key, seriesJson, nullptr);
                const QByteArray lapsJson = buildLapsJson(data, m_descriptorTable);
                if (!lapsJson.isEmpty())
                    m_workoutStore->saveLaps(w.key, lapsJson, nullptr);

                // Keep the raw payloads for a later cloud upload - they
                // can't be refetched without going back to the watch. The
                // zip is built at upload time from these, so a fix to the
                // JSON writer takes effect without re-syncing (see
                // WorkoutStore::saveSmlSources()).
                m_workoutStore->saveSmlSources(
                        w.key,
                        QByteArray(reinterpret_cast<const char *>(data.data()),
                                    static_cast<int>(data.size())),
                        summaryOk ? QByteArray(reinterpret_cast<const char *>(payload.data()),
                                                static_cast<int>(payload.size()))
                                  : QByteArray(),
                        nullptr);
            } else {
                failuresCopy.append(tr("%1: failed to save (%2)").arg(logbookId, storeError));
            }
            fetchWatchEntryAt(logbookIds, index + 1, succeeded, failuresCopy);
        });
    });
}
