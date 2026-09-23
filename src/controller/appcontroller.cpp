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

#include <algorithm>
#include <cmath>
#include <map>
#include <cstring>
#include <stdexcept>

namespace {

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
QByteArray summaryDetailsJson(const std::vector<uint8_t> &payload)
{
    QJsonObject fields;
    Sml::decode(Sbem::parseContainer(payload), [&fields](const Sml::Reading &reading) {
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
QByteArray buildSeriesJson(const std::vector<uint8_t> &compressed)
{
    std::map<QString, std::vector<double>> samples;
    Sml::decode(Sbem::parseContainer(Sbem::heatshrinkDecompress(compressed)),
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
                        const QString &source, qint64 startTimeMs, qint64 stopTimeMs)
{
    const int offsetMinutes =
            QDateTime::fromMSecsSinceEpoch(startTimeMs).offsetFromUtc() / 60;
    const std::string src = source.toStdString();

    const std::string samples = SmlJson::buildDocument(
            Sbem::parseContainer(Sbem::heatshrinkDecompress(compressedData)),
            src, offsetMinutes);
    // A /Summary payload has no clock chunk, so its entries need stamping
    // from outside. The captured upload puts summary.json's entries at the
    // end of the workout, so that is what goes in.
    const std::string summary = summaryPayload.empty()
            ? std::string()
            : SmlJson::buildDocument(Sbem::parseContainer(summaryPayload), src, offsetMinutes,
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
QByteArray buildLapsJson(const std::vector<uint8_t> &compressed)
{
    struct Marker { int type; int64_t timeMs; double distance; };
    std::vector<Marker> markers;
    double distance = 0;
    int64_t firstTimeMs = 0;
    int64_t lastTimeMs = 0;

    Sml::decode(Sbem::parseContainer(Sbem::heatshrinkDecompress(compressed)),
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

    if (!m_pairedWatchStore->open(&error)) {
        emit errorOccurred(tr("Failed to open database: %1").arg(error));
    } else {
        m_pairedWatch = m_pairedWatchStore->load(&error);
        if (!error.isEmpty())
            emit errorOccurred(tr("Failed to load paired watch: %1").arg(error));
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
    const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
    m_whiteboardClient->fetchLogbookData(path,
            [this, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        m_logbookTestInFlight = false;
        if (!ok) {
            emit logbookTestResult(tr("Fetch failed: %1").arg(error));
            return;
        }

        try {
            const Logbook::DecodedWorkout decoded = Logbook::decode(data);
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
                        startTime, stopTime);
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
        });
    });
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

        fetchWatchEntryAt(logbookIds, 0, 0, {});
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
            const Logbook::DecodedWorkout decoded = Logbook::decode(data);
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
                applySummary(&w, Summary::decode(payload));

            QString storeError;
            if (m_workoutStore->upsert(w, &storeError)) {
                ++succeeded;
                if (!track.empty())
                    m_workoutStore->saveRoute(w.key, packTrack(track), nullptr);
                if (summaryOk)
                    m_workoutStore->saveDetails(w.key, summaryDetailsJson(payload), nullptr);
                const QByteArray seriesJson = buildSeriesJson(data);
                if (!seriesJson.isEmpty())
                    m_workoutStore->saveSeries(w.key, seriesJson, nullptr);
                const QByteArray lapsJson = buildLapsJson(data);
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
