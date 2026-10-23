#include "appcontroller.h"

#include "../ble/capabilities.h"
#include "../ble/pluginregistry.h"
#include "../ble/sbemcontainer.h"
#include "../secrets/tokenvault.h"
#include "../cloud/cloudaccountstore.h"
#include "../ble/pairedwatchstore.h"
#include "../ble/devicelistmodel.h"
#include "../ble/mdswhiteboardclient.h"
#include "../cloud/suuntocloudclient.h"
#include "../store/workoutstore.h"
#include "../store/workout.h"
#include "../store/workoutsummary.h"
#include "../map/mercator.h"
#include "../model/workoutlistmodel.h"
#include "../health/healthstore.h"
#include "../ble/logbookdecoder.h"
#include "../ble/summarydecoder.h"
#include "../ble/smldecoder.h"
#include "../ble/sleepdecoder.h"
#include "../ble/recoverydecoder.h"
#include "../ble/activitydecoder.h"
#include "../ble/notificationcodec.h"
#include "../cloud/polyline.h"
#include "../cloud/smljson.h"
#include "../cloud/zipwriter.h"

#include <QDebug>
#include <QTimeZone>
#include <QStandardPaths>
#include <QtMath>
#include <QVariantMap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegExp>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QTimer>
#include <QSet>
#include <QSettings>
#include <QTextStream>

#include <algorithm>
#include "../fit/activity.h"
#include "../fit/suuntosport.h"
#include "../fit/fitencoder.h"
#include "../fit/watchactivity.h"

#include <cctype>
#include <cmath>
#include <map>
#include <cstring>
#include <stdexcept>

namespace {

// Where the preferences live, and it has to be said explicitly.
//
// A default-constructed QSettings writes ~/.config/<Org>/<App>.conf, and
// Sailjail does not grant that path: it grants the *directory*
// ~/.config/<Org>/<App>, which is exactly what QStandardPaths'
// AppConfigLocation names. On this phone that directory existed and was
// empty while the .conf beside it had not been written since the day the
// cover setting was added - every setValue since had gone nowhere, silently,
// because QSettings reports a status nobody checks. The base-map switch was
// simply the first preference anyone changed twice.
//
// Reads kept working, which is what made it look like a save bug rather than
// a path bug.
QString settingsPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.ini");
}


// What the watch shows as the sending application. The official app sends
// the posting package's name here; ours is the only sender there is.
const char *const kApplicationId = "io.github.jarnose.suuntosync";

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

QString dataDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

// The shape of a stored cloud-details field map, written into it as
// "_schema" so a row from an older build can be recognised and re-fetched.
// 1 = numbers and booleans only. 2 = strings too, which is what made the
// recording watch's own model, serial and firmware version visible: without
// this marker, every workout whose page had already been opened once would
// have kept a row that silently lacked them for ever.
const int kCloudDetailsSchema = 2;

// The default, and the reason it can be one at all: OpenFreeMap needs no API
// key, sets no request limit and permits commercial use, where every raster
// source that would have been simpler to draw wants a key that cannot live in
// a public repository. See docs/base-map.md.
//
// This is the TileJSON endpoint rather than a {z}/{x}/{y} template on purpose:
// OpenFreeMap's own tile path carries the date of the planet build it came
// from, so a template hardcoded today stops being current. setMapTileUrl
// accepts either form.
const QString kDefaultMapTileUrl = QStringLiteral("https://tiles.openfreemap.org/planet");

// The cloud_cache kind for the personal records. One name, in one place,
// because a typo would silently cache into a second row that nothing reads.
const QLatin1String kRecordsCacheKind("records");

QString dbPath()
{
    return QDir(dataDirectory()).filePath(QStringLiteral("suuntosync.sqlite"));
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
// The identifier that goes into an uploaded workout's `Source`, built from
// what the file says about the device that wrote it: "garmin-3428415862"
// for a real Edge, "suunto-23" for a Suunto's own FIT export with no
// serial in it. Truthful by construction - the alternative, borrowing the
// paired watch's serial, would file somebody else's ride as that watch's.
std::string fitSourceName(const Fit::Summary &summary)
{
    // FIT manufacturer ids: 1 is Garmin, 23 is Suunto. Only the two that
    // have turned up here are named; anything else keeps its number, which
    // is still a stable identifier.
    std::string maker;
    switch (summary.manufacturer) {
    case 1:  maker = "garmin"; break;
    case 23: maker = "suunto"; break;
    default: maker = "fit" + std::to_string(summary.manufacturer); break;
    }
    const uint32_t id = summary.serial != 0 ? summary.serial
                                             : static_cast<uint32_t>(summary.product);
    return maker + "-" + std::to_string(id);
}

// The workout row for an imported file.
//
// The key is the start time in seconds, in its own "fit_" namespace: the
// same ride imported twice overwrites itself rather than appearing twice,
// and a cloud copy of the same outing stays a separate row the way a
// watch-read one does.
Workout workoutFromFit(const Fit::Activity &activity)
{
    const Fit::Summary &s = activity.summary;
    Workout w;
    w.key = QStringLiteral("fit_%1").arg(static_cast<qint64>(s.startTime));
    w.source = QStringLiteral("fit");
    w.activityId = Fit::suuntoActivityId(Fit::sportKey(s.sport, s.subSport));
    w.startTime = static_cast<qint64>(s.startTime) * 1000;
    // The elapsed time spans the pauses and the timer does not; the figure
    // this project shows elsewhere is the moving one (a watch workout's
    // Duration - PauseDuration), so the timer is what matches it.
    const double seconds = !std::isnan(s.timer) ? s.timer
                                                 : (!std::isnan(s.elapsed) ? s.elapsed : 0.0);
    w.totalTime = seconds;
    w.stopTime = w.startTime + static_cast<qint64>((!std::isnan(s.elapsed) ? s.elapsed : seconds)
                                                     * 1000.0);
    if (!std::isnan(s.distance))
        w.totalDistance = s.distance;
    if (!std::isnan(s.maxSpeed))
        w.maxSpeed = s.maxSpeed;
    if (s.avgHeartRate > 0)
        w.avgHeartRate = s.avgHeartRate;
    if (s.maxHeartRate > 0)
        w.maxHeartRate = s.maxHeartRate;
    if (!std::isnan(s.ascent))
        w.totalAscent = s.ascent;
    if (!std::isnan(s.descent))
        w.totalDescent = s.descent;
    if (s.calories > 0)
        w.energyConsumption = s.calories;
    return w;
}

// Defined below, next to the chart code it belongs with.
QByteArray seriesJsonFromSamples(const std::map<QString, std::vector<double>> &samples);

// The charts, in the canonical units seriesJsonFromSamples() expects -
// which are the watch's, not the file's: heart rate and cadence in hertz,
// temperature in kelvin. A reading the device did not take is left out of
// its series rather than filled with a zero.
QByteArray fitSeriesJson(const Fit::Activity &activity)
{
    std::map<QString, std::vector<double>> samples;
    for (const Fit::Sample &sample : activity.samples) {
        if (sample.heartRate > 0)
            samples[QStringLiteral("Sample.HR")].push_back(sample.heartRate / 60.0);
        if (sample.cadence >= 0)
            samples[QStringLiteral("Sample.Cadence")].push_back(sample.cadence / 60.0);
        if (sample.power >= 0)
            samples[QStringLiteral("Sample.Power")].push_back(sample.power);
        if (!std::isnan(sample.altitude))
            samples[QStringLiteral("Sample.Altitude")].push_back(sample.altitude);
        if (!std::isnan(sample.speed))
            samples[QStringLiteral("Sample.Speed")].push_back(sample.speed);
        if (!std::isnan(sample.temperature))
            samples[QStringLiteral("Sample.Temperature")].push_back(sample.temperature + 273.15);
    }
    return seriesJsonFromSamples(samples);
}

// The laps a FIT file states, rather than laps derived from markers: the
// file carries them as their own messages with their own averages, so this
// fills the same four fields a cloud lap does and two more besides.
QByteArray fitLapsJson(const Fit::Activity &activity)
{
    QJsonArray laps;
    int number = 0;
    for (const Fit::Lap &lap : activity.laps) {
        const double seconds = !std::isnan(lap.timer) ? lap.timer : lap.elapsed;
        if (std::isnan(seconds))
            continue;
        QJsonObject out;
        out.insert(QStringLiteral("number"), ++number);
        // The file does not say why a lap ended, so this says where it came
        // from instead of inventing a reason.
        out.insert(QStringLiteral("type"), QStringLiteral("FIT"));
        out.insert(QStringLiteral("durationSeconds"), seconds);
        if (!std::isnan(lap.distance))
            out.insert(QStringLiteral("distanceMeters"), lap.distance);
        if (!std::isnan(lap.avgSpeed))
            out.insert(QStringLiteral("avgSpeed"), lap.avgSpeed);
        if (lap.avgHeartRate > 0)
            out.insert(QStringLiteral("avgHeartRate"), lap.avgHeartRate);
        if (!std::isnan(lap.ascent))
            out.insert(QStringLiteral("ascent"), lap.ascent);
        if (lap.calories > 0)
            out.insert(QStringLiteral("energy"), lap.calories);
        laps.append(out);
    }
    return laps.isEmpty() ? QByteArray() : QJsonDocument(laps).toJson(QJsonDocument::Compact);
}

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

// Laps out of a cloud sample-data response.
//
// They live in Summary.Samples, not in Data: each sample carries
// Attributes["suunto/sml"].Windows, and each window has a Type. Captured
// from a walk with both kinds of lap on it:
//
//   Autolap   an automatic lap - three of them, 1000 + 1000 + 830 m
//   Lap       a lap the wearer pressed for - three, 1349 + 270 + 1211 m
//   Activity  the whole activity
//   Move      the whole recording
//
// The two lap kinds are **parallel partitions of the same workout**, not a
// sequence: each set sums to the total on its own (2830 m and 2663 s both
// ways). So they are numbered within their own kind rather than
// interleaved, and the type is carried through so a reader can tell which
// is which.
//
// The names map onto the vocabulary the BLE decoder already uses for the
// watch's own markers - Autolap is a distance lap and Lap is a manual one -
// so both sources produce the same words.
//
// Distance and Duration are each window's own, not cumulative. TimeISO8601
// on the sample is where the window ended.
QByteArray buildCloudLapsJson(const QByteArray &body)
{
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonArray samples =
            root.value(QStringLiteral("Summary")).toObject()
                .value(QStringLiteral("Samples")).toArray();

    // Two lists, not one. The cloud's summary carries the auto-laps and the
    // manual laps as *separate complete partitions* of the same workout -
    // measured on a 2830 m walk, where three auto-laps summed to 2830 m and
    // 2663 s and the three manual ones did too - and the file interleaves
    // them by time. Emitting them in that order gives a table with two rows
    // numbered 1, two numbered 2, and twice the real distance down the
    // column. Grouped, each run of rows is one whole partition and the type
    // column says which.
    QJsonArray automaticLaps;
    QJsonArray manualLaps;
    int automatic = 0;
    int manual = 0;
    for (const QJsonValue &sampleValue : samples) {
        const QJsonArray windows =
                sampleValue.toObject().value(QStringLiteral("Attributes")).toObject()
                    .value(QStringLiteral("suunto/sml")).toObject()
                    .value(QStringLiteral("Windows")).toArray();
        for (const QJsonValue &windowValue : windows) {
            const QJsonObject window = windowValue.toObject();
            const QString type = window.value(QStringLiteral("Type")).toString();

            QString name;
            int number = 0;
            if (type == QStringLiteral("Autolap")) {
                name = QStringLiteral("Distance");
                number = ++automatic;
            } else if (type == QStringLiteral("Lap")) {
                name = QStringLiteral("Manual");
                number = ++manual;
            } else {
                // Activity and Move are the whole workout, which the
                // summary fields already carry.
                continue;
            }

            // "Avg" out of a one-element array of {Avg,Max,Min}, which is
            // how the cloud's SML writes every one of these. A null reading
            // and an absent field both come back 0, which is also how the
            // rest of this page reads "not recorded" - the watch's own
            // schema marks these nillable=0.
            auto average = [&window](const char *field) {
                return window.value(QLatin1String(field)).toArray().at(0).toObject()
                        .value(QStringLiteral("Avg")).toDouble();
            };

            QJsonObject lap;
            lap.insert(QStringLiteral("number"), number);
            lap.insert(QStringLiteral("type"), name);
            lap.insert(QStringLiteral("durationSeconds"),
                        window.value(QStringLiteral("Duration")).toDouble());
            lap.insert(QStringLiteral("distanceMeters"),
                        window.value(QStringLiteral("Distance")).toDouble());
            // The rest of the window, in this page's units rather than the
            // cloud's: heart rate arrives in hertz and energy in joules.
            // Measured on a real walk - HR 1.29 Hz is 77 bpm, and the three
            // auto-laps' 333688 + 281771.7 + 230274 J come to the 202 kcal
            // the whole workout reports.
            // Through toDisplayUnits(), not by multiplying here: hertz to
            // bpm and joules to kcal are that function's job, and a second
            // copy of those factors is a second place to get them wrong.
            lap.insert(QStringLiteral("avgHeartRate"),
                        toDisplayUnits(QStringLiteral("HR"), average("HR")).value);
            lap.insert(QStringLiteral("avgSpeed"), average("Speed"));
            lap.insert(QStringLiteral("ascent"),
                        window.value(QStringLiteral("Ascent")).toDouble());
            lap.insert(QStringLiteral("energy"),
                        toDisplayUnits(QStringLiteral("Energy"),
                                        window.value(QStringLiteral("Energy")).toDouble()).value);
            if (type == QStringLiteral("Autolap"))
                automaticLaps.append(lap);
            else
                manualLaps.append(lap);
        }
    }

    QJsonArray laps = automaticLaps;
    for (const QJsonValue &lap : manualLaps)
        laps.append(lap);

    return laps.isEmpty() ? QByteArray() : QJsonDocument(laps).toJson(QJsonDocument::Compact);
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
    // Heart rate per sample, kept so each lap can carry its own average.
    // The watch sends no per-lap figures at all - these laps are the
    // stretches between marker events - but the samples are there, and the
    // cloud's own lap averages turn out to be the plain mean of exactly
    // these readings: computed both ways against a real workout's 2664
    // one-second samples, the sample mean and the time-weighted mean agreed
    // with the cloud's own HR.Avg to within 0.1 bpm on all six of its laps.
    // At one sample a second there is nothing to choose between them.
    std::vector<std::pair<int64_t, double>> heartRates;
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
        else if (name == QLatin1String("Sample.HR") && reading.value > 0)
            heartRates.push_back({ reading.timeMs, reading.value });
        else if (name == QLatin1String("Sample.Events.Array.Lap.Type"))
            markers.push_back({ static_cast<int>(reading.value), reading.timeMs, distance });
    });

    if (markers.empty())
        return QByteArray();

    // Average speed is the lap's distance over its duration, which is not
    // an approximation of the cloud's figure but the same definition: the
    // cloud reported 0.929 m/s for a lap of 1000 m in 1076 s. Heart rate is
    // the mean of the samples inside the lap. Energy and ascent are left
    // alone - energy is in no sample at all, and a per-lap ascent off the
    // altitude series would carry the whole of this project's 2 m
    // hysteresis error inside a few hundred metres.
    auto addDerived = [&heartRates](QJsonObject *lap, int64_t from, int64_t to,
                                     double metres) {
        const double seconds = (to - from) / 1000.0;
        if (seconds > 0 && metres > 0)
            lap->insert(QStringLiteral("avgSpeed"), metres / seconds);

        double sum = 0;
        int count = 0;
        for (const std::pair<int64_t, double> &reading : heartRates) {
            if (reading.first < from || reading.first > to)
                continue;
            sum += reading.second;
            ++count;
        }
        if (count > 0) {
            lap->insert(QStringLiteral("avgHeartRate"),
                         toDisplayUnits(QStringLiteral("HR"), sum / count).value);
        }
    };

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
        addDerived(&lap, fromTime, marker.timeMs, marker.distance - fromDistance);
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
        addDerived(&lap, fromTime, lastTimeMs, distance - fromDistance);
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
        } else if (value.isString() && !value.toString().isEmpty()) {
            // Text, kept under its own key so a reader can tell the two
            // apart without guessing from the value's type. These were
            // dropped until the gear block turned out to be where the
            // recording watch names itself - model, serial and firmware
            // version - which is worth knowing for an account that two
            // watches both write to.
            QJsonObject entry;
            entry.insert(QStringLiteral("text"), value.toString());
            out->insert(name, entry);
        }
    }
}

// The cloud's per-workout extensions, turned into the same name/value rows
// the watch's own fields already use. An extension carries its kind in a
// "type" field, which becomes the name prefix so two extensions with a
// similarly named number don't collide.
// Both the detail GET and the extensions POST answer with the same
// {"extensions": [...]} shape, so one function flattens either, and
// `into` lets the two be merged before anything is stored.
void collectCloudDetailFields(const QJsonObject &payload, QJsonObject *into)
{
    for (const QJsonValue &value : payload.value(QStringLiteral("extensions")).toArray()) {
        const QJsonObject extension = value.toObject();
        QString type = extension.value(QStringLiteral("type")).toString();
        if (type.isEmpty())
            type = QStringLiteral("Extension");
        flattenJson(extension, type, into);
    }
}

// CTL, ATL and TSB over a list of workouts. A free function rather than a
// member because it reads nothing but its argument, and split out of
// AppController::trainingProgress() so the cover can show the same three
// figures without a second loadAll of a thousand-odd rows - it already has
// the workouts in hand for its other modes.
//
// The arithmetic itself is in the Qt-free WorkoutSummary; what has to be
// here is the bucketing into days, because a day boundary needs a timezone.
QVariantMap progressFrom(const QVector<Workout> &workouts)
{
    QVariantMap out;
    out.insert(QStringLiteral("ctl"), 0.0);
    out.insert(QStringLiteral("atl"), 0.0);
    out.insert(QStringLiteral("tsb"), 0.0);
    out.insert(QStringLiteral("days"), 0);

    if (workouts.isEmpty())
        return out;

    // Deduplicated the same way the month is, and for the same reason: the
    // same outing can be here twice. The larger stress score wins, which in
    // practice means the cloud's copy beats a watch copy that has none.
    std::vector<WorkoutSummary::Entry> entries;
    entries.reserve(static_cast<size_t>(workouts.size()));
    for (const Workout &workout : workouts)
        entries.push_back({ workout.startTime, workout.totalTime, workout.trainingStressScore });
    entries = WorkoutSummary::collapseDuplicates(std::move(entries));

    // Bucket into local calendar days. This is the part that cannot live in
    // the Qt-free side: a day boundary needs a timezone.
    const QDate first = QDateTime::fromMSecsSinceEpoch(entries.front().startMs).date();
    const QDate today = QDate::currentDate();
    if (first > today)
        return out;

    const int days = first.daysTo(today) + 1;
    std::vector<double> daily(static_cast<size_t>(days), 0.0);

    for (const WorkoutSummary::Entry &entry : entries) {
        const QDate date = QDateTime::fromMSecsSinceEpoch(entry.startMs).date();
        const int day = first.daysTo(date);
        if (day >= 0 && day < days)
            daily[static_cast<size_t>(day)] += entry.tss;
    }

    const WorkoutSummary::Progress progress = WorkoutSummary::progressFromDailyLoad(daily);
    out[QStringLiteral("ctl")] = progress.ctl;
    out[QStringLiteral("atl")] = progress.atl;
    out[QStringLiteral("tsb")] = progress.tsb;
    out[QStringLiteral("days")] = days;

    // **How far behind these figures are**, which the page needs in order
    // not to lie by omission.
    //
    // Only the cloud scores a workout: a watch-synced row has no
    // trainingStressScore at all, so three sessions read off the watch
    // move nothing here while the official app, which has the cloud's
    // numbers, moves by twenty points. That is exactly what happened on
    // 2026-10-19 - ours read 14/4/+9 against the app's 17/19/0, and the
    // cause was a cloud sync eight days stale with three watch workouts
    // sitting on top of it.
    //
    // The count is of collapsed entries, so a workout that exists as both
    // a cloud and a watch row does not count: collapseDuplicates() keeps
    // the scored copy.
    qint64 newestScored = 0;
    for (const WorkoutSummary::Entry &entry : entries) {
        if (entry.tss > 0 && entry.startMs > newestScored)
            newestScored = entry.startMs;
    }
    int unscored = 0;
    for (const WorkoutSummary::Entry &entry : entries) {
        if (entry.tss <= 0 && entry.startMs > newestScored)
            ++unscored;
    }
    out[QStringLiteral("unscored")] = unscored;
    out[QStringLiteral("scoredThrough")] = newestScored;
    return out;
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
    , m_watchLink(dataDirectory(), WatchLink::Foreground)
    , m_workoutStore(new WorkoutStore(dbPath()))
    , m_workoutModel(new WorkoutListModel(this))
    , m_healthStore(new HealthStore(dbPath()))
{
    // Everything the user is told goes to the journal too. errorOccurred is
    // this app's one channel for anything worth putting on a banner, and
    // until now none of it was recorded anywhere: a message that scrolled
    // past, or was too long to read on a phone, was simply gone. Connecting
    // it here catches every emitter rather than asking fifty call sites to
    // remember.
    // qWarning, not qInfo: Sailfish's default logging rules drop an
    // application's info level, so the first attempt at this logged nothing
    // at all while debug and warning from the same process came through. The
    // level is wrong for the cheerful half of these messages; being readable
    // matters more.
    connect(this, &AppController::errorOccurred, this, [](const QString &message) {
        qWarning().noquote() << message;
    });
    // errorOccurred is not the only channel, which the first version of this
    // missed: an upload reports through workoutUploaded, so the one message
    // actually being chased - the cloud's 523 - still went nowhere. Every
    // signal that carries text a user reads belongs here.
    connect(this, &AppController::workoutUploaded, this,
            [](const QString &key, bool ok, const QString &message) {
        qWarning().noquote() << QStringLiteral("upload %1 %2: %3")
                                        .arg(key, ok ? QStringLiteral("ok")
                                                     : QStringLiteral("failed"), message);
    });
    connect(this, &AppController::whiteboardTestResult, this, [](const QString &summary) {
        qWarning().noquote() << summary;
    });
    // The cover's "sync everything" is a queue, and these three signals are
    // what say a step has finished. One connection each rather than a timer:
    // polling for idleness would be a guess about how long a step takes.
    const auto step = [this]() {
        if (!m_coverChain.isEmpty())
            startNextCoverStep();
    };
    connect(this, &AppController::workoutSyncInProgressChanged, this, step);
    connect(this, &AppController::healthSyncInProgressChanged, this, step);
    connect(this, &AppController::uploadInProgressChanged, this, step);
    // The WiFi transcript especially: it is hex, it is long, and reading it
    // off a phone screen and typing it out again is how a measurement
    // acquires typos. Jarno asked for this after transcribing three.
    connect(this, &AppController::wifiResult, this, [](const QString &summary) {
        qWarning().noquote() << QStringLiteral("wifi: ") + summary;
    });
    connect(this, &AppController::logbookTestResult, this, [](const QString &summary) {
        qWarning().noquote() << summary;
    });
    connect(this, &AppController::watchNotificationsRead, this,
            [](int value, const QString &detail) {
        qWarning().noquote() << QStringLiteral("watch notifications %1: %2")
                                        .arg(value).arg(detail);
    });

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
        // One-time move out of the path Sailjail never granted, so a cover
        // choice made before this is not silently forgotten.
        if (!QFile::exists(settingsPath())) {
            QSettings legacy;
            QSettings moved(settingsPath(), QSettings::IniFormat);
            const QStringList keys = legacy.allKeys();
            for (const QString &key : keys)
                moved.setValue(key, legacy.value(key));
            moved.sync();
        }

        QSettings settings(settingsPath(), QSettings::IniFormat);
        m_coverMode = settings.value(QStringLiteral("cover/mode"),
                                      QStringLiteral("latest")).toString();
        m_coverSteps = settings.value(QStringLiteral("cover/steps"), false).toBool();
        m_coverAction = settings.value(QStringLiteral("cover/action"),
                                        QStringLiteral("watchWorkouts")).toString();
        m_syncOnConnect = settings.value(QStringLiteral("sync/onConnect"), false).toBool();
        m_autoUploadSuunto =
                settings.value(QStringLiteral("upload/autoSuunto"), false).toBool();
        m_wifiSsid = settings.value(QStringLiteral("wifi/ssid")).toString();
        // Whether the vault holds a passphrase is a note here rather than a
        // read of the vault: the page only asks to decide what to show, and
        // a read would wake the Secrets daemon on every launch. A note that
        // is wrong surfaces as a readable error on the first attempt.
        m_wifiKeyStored = settings.value(QStringLiteral("wifi/keyStored"), false).toBool();
        m_baseMapEnabled = settings.value(QStringLiteral("map/enabled"), false).toBool();
        m_mapTileUrl = settings.value(QStringLiteral("map/tileUrl"), kDefaultMapTileUrl).toString();
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

        if (m_watchSessionAttaching && m_watchAttachTimer.isValid()) {
            // Into the probe log, not just qWarning(): journald on this
            // device runs with Storage=volatile and has no user journal at
            // all, so a qWarning() is a measurement nobody can read back.
            const QString line = QStringLiteral("watch session %1 %2 ms")
                    .arg(ready ? QStringLiteral("attached in")
                                : QStringLiteral("failed after"))
                    .arg(m_watchAttachTimer.elapsed());
            qWarning().noquote() << line;
            appendProbeLog(line);
        }
        m_watchSessionAttaching = false;
        ++m_watchSessionAttachGeneration;
        // A readyChanged(false) while an attach was in flight is the same
        // story as the timeout above: nothing holds a session, so nothing
        // will release the lock unless this does.
        if (!ready)
            abandonWatchSession();
        if (m_watchSessionWaiters.isEmpty())
            return;

        const QVector<std::function<void(const WatchSession &)>> waiting =
                m_watchSessionWaiters;
        m_watchSessionWaiters.clear();
        if (!ready) {
            // The attach failed, or the watch went away mid-attach. Saying
            // so beats leaving a tap that did nothing: whiteboardClient
            // reports its own reason too, and this is the one that names
            // the thing the user asked for.
            emit errorOccurred(tr("The watch did not answer."));
            return;
        }
        // One session for all of them, held until the last chain lets go.
        const WatchSession session = currentWatchSession();
        for (const std::function<void(const WatchSession &)> &operation : waiting)
            operation(session);
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
        // A new connection renegotiates the MTU, so what was measured on
        // the old one stops being true. The session itself is let go the
        // same way it always is - by the last holder going away.
        if (!device.connected)
            m_whiteboardClient->forgetConnection();
    }

    // Deliberately does not attach. A connected watch used to mean an
    // attached one for the whole life of the process, which held the
    // Whiteboard session - and therefore the watch - away from the
    // notification daemon the entire time the app was open. The session is
    // taken per operation now, by withWatchSession(); being connected is
    // what the UI gates on.
    //
    // One operation does start from here, and only if the setting says so:
    // the workouts. "Sync when the watch connects" was a switch that was
    // stored, shown and toggled and read by nothing at all - it has never
    // done anything since it was added. It syncs the workouts and not the
    // health data: that is the longer of the two, the health page has its
    // own action for it, and a setting whose switch says "a sync" should
    // not quietly mean two.
    //
    // A second's grace first. The watch reports Connected before BlueZ has
    // resolved its services, and an attach that starts too early waits for
    // them - which works, but means the first thing a connection does is
    // sit on the watch while the daemon may have a notification for it.
    if (device.connected && m_syncOnConnect) {
        QTimer::singleShot(1000, this, [this]() {
            if (m_watchConnected)
                syncWatchWorkouts();
        });
    }
}

void AppController::onConnectFinished(const QString &objectPath, bool ok, const QString &error,
                                      const QString &errorName)
{
    if (objectPath != m_pairedWatch.objectPath)
        return;
    if (!ok) {
        // BlueZ allows one Connect() per device at a time, so if the
        // notification daemon is already trying, ours comes back
        // "In Progress" - which means the connection is on its way, not that
        // it failed. Reporting it made a perfectly good watch look broken
        // for a minute, which is how this was found.
        if (errorName == QLatin1String(BluezError::kInProgress)) {
            qWarning() << "a connection to the watch is already being opened; waiting";
            return;
        }
        if (errorName != QLatin1String(BluezError::kAlreadyConnected)) {
            emit errorOccurred(tr("Could not connect to watch: %1").arg(error));
            return;
        }
        // Already connected is not a failure either: carry on as if the
        // call had succeeded, which is what the watch's state says.
    }
    m_watchConnected = true;
    emit watchConnectedChanged();
    // No attach here either - see onDeviceUpdated().
    Q_UNUSED(objectPath);
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
    // Let go of whatever is in hand first. Without this, switching from a
    // connected Race to a 9 Baro left the Whiteboard client attached to the
    // Race's characteristics - every request would still have gone to the
    // old watch while the UI said otherwise, which is the worst kind of
    // wrong because it looks like it works.
    if (!m_pairedWatch.address.isEmpty() && m_pairedWatch.address != address) {
        m_whiteboardClient->detach();
        releaseWatchLink();
        if (m_watchConnected && !m_pairedWatch.objectPath.isEmpty())
            m_bluezAdapter->disconnectFromDevice(m_pairedWatch.objectPath);
    }

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

QVariantList AppController::knownWatches() const
{
    QVariantList list;
    // nullptr rather than a discarded QString: this is const, so it cannot
    // emit errorOccurred, and a menu that comes back empty says as much as
    // an error nobody would see.
    const QVector<PairedWatch> watches = m_pairedWatchStore->knownWatches(nullptr);
    for (const PairedWatch &watch : watches) {
        QVariantMap entry;
        entry[QStringLiteral("address")] = watch.address;
        entry[QStringLiteral("objectPath")] = watch.objectPath;
        entry[QStringLiteral("name")] = watch.name;
        entry[QStringLiteral("active")] = watch.address == m_pairedWatch.address;
        list.append(entry);
    }
    return list;
}

void AppController::switchToWatch(const QString &address)
{
    if (address.isEmpty() || address == m_pairedWatch.address)
        return;

    QString error;
    const QVector<PairedWatch> watches = m_pairedWatchStore->knownWatches(&error);
    if (watches.isEmpty() && !error.isEmpty()) {
        emit errorOccurred(tr("Could not read the remembered watches: %1").arg(error));
        return;
    }
    for (const PairedWatch &watch : watches) {
        if (watch.address != address)
            continue;
        selectWatch(watch.objectPath, watch.address, watch.name);
        return;
    }
    emit errorOccurred(tr("That watch is not remembered any more - pair it again."));
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
    releaseWatchLink();
    m_pairedWatch = PairedWatch();
    m_watchConnected = false;
    emit pairedWatchChanged();
    emit watchConnectedChanged();
}

namespace {

// The cloud wants the watch's variant as its own parameter, and the watch
// states it inside the capability list as feat_simvariant_<name> - "Tianjin"
// on the Race. Nothing else carries it, so this is not a shortcut.
QString variantFrom(const std::vector<std::string> &capabilities)
{
    const std::string prefix = "feat_simvariant_";
    for (const std::string &capability : capabilities) {
        if (capability.compare(0, prefix.size(), prefix) == 0)
            return QString::fromStdString(capability.substr(prefix.size()));
    }
    return QString();
}

} // namespace

void AppController::testDeleteZoneSense()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_whiteboardTestInFlight)
            return;

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->fetchInstallLocation(
                [this, session](bool ok, const QString &location, const QString &error) {
            if (!ok) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("Could not ask the watch where plugins go: %1")
                                                   .arg(error));
                return;
            }
            // The installed form, not the staged zip: the watch keeps a sports
            // app as <pluginId>.fea, a watch face as .wfa and a guide as .gui,
            // and there is no .zip left in the directory at all.
            const QString target = location + QStringLiteral("/zzaeroen.fea");
            m_whiteboardClient->deleteFile(target,
                    [this, session, location, target](bool delOk, const QString &delError) {
                if (!delOk) {
                    m_whiteboardTestInFlight = false;
                    emit whiteboardTestResult(delError);
                    return;
                }
                m_whiteboardClient->listDirectory(location,
                        [this, session, target](bool listOk, const std::vector<uint8_t> &body,
                                        const QString &listError) {
                    m_whiteboardTestInFlight = false;
                    if (!listOk) {
                        emit whiteboardTestResult(tr("Deleted %1, but listing failed: %2")
                                                           .arg(target, listError));
                        return;
                    }
                    const QByteArray raw(reinterpret_cast<const char *>(body.data()),
                                          static_cast<int>(body.size()));
                    const bool stillThere = raw.contains("zzaeroen.fea");
                    emit whiteboardTestResult(
                            stillThere ? tr("The watch accepted the delete but zzaeroen.fea "
                                             "is still listed")
                                       : tr("zzaeroen.fea is gone. Now install it again and "
                                             "list the directory."));
                });
            });
        });
    });
}

void AppController::testListZappDirectory()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_whiteboardTestInFlight)
            return;

        m_whiteboardTestInFlight = true;
        // Ask the watch where plugins go rather than assuming the prefix, then
        // list exactly that.
        m_whiteboardClient->fetchInstallLocation(
                [this, session](bool ok, const QString &location, const QString &error) {
            if (!ok) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("Could not ask the watch where plugins go: %1")
                                                   .arg(error));
                return;
            }
            m_whiteboardClient->listDirectory(location,
                    [this, session, location](bool listOk, const std::vector<uint8_t> &body,
                                      const QString &listError) {
                m_whiteboardTestInFlight = false;
                if (!listOk) {
                    emit whiteboardTestResult(listError);
                    return;
                }
                // The JSON is found by its braces, which is what zappctl does -
                // the reply's own header is not worth trusting an offset into.
                const QByteArray raw(reinterpret_cast<const char *>(body.data()),
                                      static_cast<int>(body.size()));
                const int open = raw.indexOf('{');
                const int close = raw.lastIndexOf('}');
                if (open < 0 || close <= open) {
                    emit whiteboardTestResult(tr("No listing in the watch's %1-byte reply")
                                                       .arg(body.size()));
                    return;
                }
                const QByteArray json = raw.mid(open, close - open + 1);
                if (json == QByteArrayLiteral("{}")) {
                    // zappctl's measured meaning for this: the path was spelled
                    // for the other generation. It should not happen now that
                    // the location comes from the watch.
                    emit whiteboardTestResult(tr("The watch returned an empty listing for %1")
                                                       .arg(location));
                    return;
                }
                emit whiteboardTestResult(tr("%1: %2").arg(location,
                                                            QString::fromUtf8(json)));
            });
        });
    });
}

void AppController::testInstallPlugin()
{
    withWatchSession([this](const WatchSession &session) {
        if (!m_cloudAccount.isSignedIn()) {
            emit whiteboardTestResult(tr("Sign in to the Suunto cloud first."));
            return;
        }
        if (m_whiteboardTestInFlight)
            return;

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->fetchCapabilities(
                [this, session](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            if (!ok) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("Capabilities read failed: %1").arg(error));
                return;
            }
            const std::vector<std::string> caps = Capabilities::parse(payload);
            const QString variant = variantFrom(caps);
            const QString joined = QString::fromStdString(Capabilities::join(caps));

            m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
                    [this, session, variant, joined](bool secretOk, const QByteArray &data,
                                             const QString &secretError) {
                if (!secretOk) {
                    m_whiteboardTestInFlight = false;
                    emit whiteboardTestResult(tr("Could not read the session key: %1")
                                                       .arg(secretError));
                    return;
                }
                const QString key = QString::fromUtf8(data);
                const QString wanted = QStringLiteral("zzaero01");
                m_cloudClient->listSuuntoPlusFeatures(key, variant, joined,
                        [this, session, key, joined, wanted](
                                bool listOk,
                                const QVector<SuuntoCloudClient::SuuntoPlusItem> &items,
                                const QString &listError) {
                    if (!listOk) {
                        m_whiteboardTestInFlight = false;
                        emit whiteboardTestResult(tr("Library listing failed: %1").arg(listError));
                        return;
                    }
                    SuuntoCloudClient::SuuntoPlusItem target;
                    for (const SuuntoCloudClient::SuuntoPlusItem &item : items) {
                        if (item.id == wanted)
                            target = item;
                    }
                    if (target.id.isEmpty()) {
                        m_whiteboardTestInFlight = false;
                        emit whiteboardTestResult(tr("%1 is not in the library").arg(wanted));
                        return;
                    }
                    // A watch face is stored under its catalogue id and a sports
                    // app under its pluginId - /Plugin/List answers in the
                    // latter, and the captured uploads are named that way.
                    const QString fileName =
                            (target.type == QStringLiteral("watchface") ? target.id
                                                                         : target.pluginId)
                            + QStringLiteral(".zip");

                    m_cloudClient->downloadSuuntoPlusPlugin(key, target.id, joined,
                            [this, session, target, fileName](bool dlOk, const QByteArray &zip,
                                                      const QString &dlError) {
                        if (!dlOk) {
                            m_whiteboardTestInFlight = false;
                            emit whiteboardTestResult(tr("Download of %1 failed: %2")
                                                               .arg(target.id, dlError));
                            return;
                        }
                        const int size = zip.size();
                        m_whiteboardClient->installPlugin(fileName, zip, nullptr,
                                [this, session, target, fileName, size](bool instOk, const QString &instError) {
                            m_whiteboardTestInFlight = false;
                            if (!instOk) {
                                emit whiteboardTestResult(tr("Install of %1 failed: %2")
                                                                   .arg(fileName, instError));
                                return;
                            }
                            // An install that answers 200 is not proof the file
                            // landed - the wrong drive prefix is accepted just
                            // as cheerfully - so the thing to check next is
                            // whether /Plugin/List now names it.
                            emit whiteboardTestResult(
                                    tr("Installed %1 (%2, %3 bytes). Run the plugin list to "
                                        "check the watch agrees.")
                                            .arg(fileName, target.name).arg(size));
                        });
                    });
                });
            });
        });
    });
}

void AppController::testDownloadPlugin()
{
    withWatchSession([this](const WatchSession &session) {
        if (!m_cloudAccount.isSignedIn()) {
            emit whiteboardTestResult(tr("Sign in to the Suunto cloud first."));
            return;
        }
        if (m_whiteboardTestInFlight)
            return;

        m_whiteboardTestInFlight = true;
        // The capability string is sent on the download too, the way the
        // official app sends it, so it has to be read first.
        m_whiteboardClient->fetchCapabilities(
                [this, session](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            if (!ok) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("Capabilities read failed: %1").arg(error));
                return;
            }
            const QString joined =
                    QString::fromStdString(Capabilities::join(Capabilities::parse(payload)));
            m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
                    [this, session, joined](bool secretOk, const QByteArray &data, const QString &secretError) {
                if (!secretOk) {
                    m_whiteboardTestInFlight = false;
                    emit whiteboardTestResult(tr("Could not read the session key: %1")
                                                       .arg(secretError));
                    return;
                }
                const QString id = QStringLiteral("zzaero01");
                m_cloudClient->downloadSuuntoPlusPlugin(QString::fromUtf8(data), id, joined,
                        [this, session, id](bool dlOk, const QByteArray &zip, const QString &dlError) {
                    m_whiteboardTestInFlight = false;
                    if (!dlOk) {
                        emit whiteboardTestResult(tr("Download of %1 failed: %2")
                                                           .arg(id, dlError));
                        return;
                    }
                    // 17784 in the capture. A different number is not
                    // necessarily wrong - the store does update packages - but
                    // it is worth noticing rather than glossing.
                    emit whiteboardTestResult(tr("OK - %1 is %2 bytes (capture had 17784)")
                                                       .arg(id).arg(zip.size()));
                });
            });
        });
    });
}

void AppController::testSuuntoPlusLibrary()
{
    withWatchSession([this](const WatchSession &session) {
        if (!m_cloudAccount.isSignedIn()) {
            emit whiteboardTestResult(tr("Sign in to the Suunto cloud first."));
            return;
        }
        if (m_whiteboardTestInFlight) {
            emit whiteboardTestResult(tr("A test request is already in flight"));
            return;
        }

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->fetchCapabilities(
                [this, session](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            if (!ok) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("Capabilities read failed: %1").arg(error));
                return;
            }
            const std::vector<std::string> caps = Capabilities::parse(payload);
            const QString variant = variantFrom(caps);
            if (variant.isEmpty()) {
                m_whiteboardTestInFlight = false;
                emit whiteboardTestResult(tr("No feat_simvariant_ in the watch's %1 "
                                              "capabilities").arg(caps.size()));
                return;
            }
            const QString joined = QString::fromStdString(Capabilities::join(caps));

            m_whiteboardClient->fetchPluginList(
                    [this, session, variant, joined](bool listOk, const std::vector<uint8_t> &listPayload,
                                             const QString &listError) {
                if (!listOk) {
                    m_whiteboardTestInFlight = false;
                    emit whiteboardTestResult(tr("Plugin list read failed: %1").arg(listError));
                    return;
                }
                QSet<QString> installed;
                for (const std::string &plugin : Capabilities::tokens(listPayload))
                    installed.insert(QString::fromStdString(plugin));

                m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
                        [this, session, variant, joined, installed](bool secretOk, const QByteArray &data,
                                                            const QString &secretError) {
                    if (!secretOk) {
                        m_whiteboardTestInFlight = false;
                        emit whiteboardTestResult(tr("Could not read the session key: %1")
                                                           .arg(secretError));
                        return;
                    }
                    m_cloudClient->listSuuntoPlusFeatures(QString::fromUtf8(data), variant, joined,
                            [this, session, installed](bool cloudOk,
                                               const QVector<SuuntoCloudClient::SuuntoPlusItem> &items,
                                               const QString &cloudError) {
                        m_whiteboardTestInFlight = false;
                        if (!cloudOk) {
                            emit whiteboardTestResult(tr("Library listing failed: %1")
                                                               .arg(cloudError));
                            return;
                        }
                        int onWatch = 0;
                        int faces = 0;
                        QStringList missing;
                        for (const SuuntoCloudClient::SuuntoPlusItem &item : items) {
                            if (item.type == QStringLiteral("watchface"))
                                ++faces;
                            if (installed.contains(item.pluginId))
                                ++onWatch;
                            else
                                missing.append(item.name);
                        }
                        // The join is the point: the watch names plugins by
                        // pluginId and the cloud by catalogue id, and only the
                        // listing carries both.
                        emit whiteboardTestResult(
                                tr("OK - library %1 (%2 watch faces), %3 on the watch, "
                                    "%4 installed the watch knows that the library does not. "
                                    "Not on the watch: %5")
                                        .arg(items.size()).arg(faces).arg(onWatch)
                                        .arg(installed.size() - onWatch)
                                        .arg(missing.isEmpty() ? tr("none")
                                                                : missing.join(QStringLiteral(", "))));
                    });
                });
            });
        });
    });
}

void AppController::testPluginList()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_whiteboardTestInFlight) {
            emit whiteboardTestResult(tr("A test request is already in flight"));
            return;
        }

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->fetchPluginList(
                [this, session](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            m_whiteboardTestInFlight = false;
            if (!ok) {
                emit whiteboardTestResult(tr("Plugin list read failed: %1").arg(error));
                return;
            }
            const std::vector<std::string> plugins = Capabilities::tokens(payload);
            if (plugins.empty()) {
                emit whiteboardTestResult(tr("Plugin list returned %1 bytes that decoded "
                                              "to nothing").arg(payload.size()));
                return;
            }
            // Ten on the captured Race: a factory watch face, eight sports apps
            // and a guide.
            emit whiteboardTestResult(tr("OK - %1 plugins: %2")
                                               .arg(plugins.size())
                                               .arg(QString::fromStdString(
                                                        Capabilities::join(plugins))));
        });
    });
}

void AppController::testCapabilities()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_whiteboardTestInFlight) {
            emit whiteboardTestResult(tr("A test request is already in flight"));
            return;
        }

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->fetchCapabilities(
                [this, session](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            m_whiteboardTestInFlight = false;
            if (!ok) {
                emit whiteboardTestResult(tr("Capabilities read failed: %1").arg(error));
                return;
            }
            const std::vector<std::string> caps = Capabilities::parse(payload);
            if (caps.empty()) {
                emit whiteboardTestResult(tr("Capabilities read returned %1 bytes that "
                                              "decoded to nothing").arg(payload.size()));
                return;
            }
            // The count is what to compare against the official app's request:
            // the capture had 49 for this Race, and the joined string is what
            // the cloud's `capabilities=` parameter carries.
            emit whiteboardTestResult(tr("OK - %1 capabilities, %2 bytes: %3")
                                               .arg(caps.size())
                                               .arg(payload.size())
                                               .arg(QString::fromStdString(
                                                        Capabilities::join(caps))));
        });
    });
}

void AppController::testWhiteboard()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_whiteboardTestInFlight) {
            emit whiteboardTestResult(tr("A test request is already in flight"));
            return;
        }

        m_whiteboardTestInFlight = true;
        m_whiteboardClient->get(QStringLiteral("/Logbook/Entries"),
                                 [this, session](bool ok, const Mds::Frame &frame, const QString &error) {
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
    });
}

void AppController::testLogbookFetch(const QString &logbookId)
{
    withWatchSession([this, logbookId](const WatchSession &session) {
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
        ensureDescriptors(logbookId, [this, session, logbookId]() {
        const QString path = QStringLiteral("/Logbook/byId/%1/Data").arg(logbookId);
        m_whiteboardClient->fetchLogbookData(path,
                [this, session, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
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
    });
}

void AppController::testEntriesFetch()
{
    withWatchSession([this](const WatchSession &session) {
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
                [this, session](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
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
    });
}

void AppController::testDescriptorsFetch(const QString &logbookId)
{
    withWatchSession([this, logbookId](const WatchSession &session) {
        if (m_logbookTestInFlight || m_workoutSyncInProgress) {
            emit logbookTestResult(tr("Another fetch is already in progress"));
            return;
        }

        m_logbookTestInFlight = true;
        const QString path = QStringLiteral("/Logbook/byId/%1/Descriptors").arg(logbookId);
        m_whiteboardClient->fetchSummary(path,
                [this, session, logbookId](bool ok, const std::vector<uint8_t> &data, const QString &error) {
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
    });
}

void AppController::testNotification(const QString &title, const QString &message,
                                     int categoryId)
{
    withWatchSession([this, title, message, categoryId](const WatchSession &session) {
        if (m_logbookTestInFlight || m_workoutSyncInProgress) {
            emit logbookTestResult(tr("Another fetch is already in progress"));
            return;
        }

        Ancs::Notification notification;
        notification.appId = kApplicationId;
        notification.title = title.toStdString();
        notification.message = message.toStdString();
        notification.categoryId = static_cast<uint8_t>(qBound(0, categoryId, 11));
        notification.date = static_cast<uint32_t>(QDateTime::currentMSecsSinceEpoch() / 1000);

        // A fixed source id, so every send from here addresses the same
        // notification: the second one replaces the first rather than filling
        // the watch's list.
        notification.notificationId =
                Ancs::notificationIdFor(1, notification.categoryId, notification.appId);
        notification.modifyExisting = notification.notificationId == m_lastNotificationId;

        // One button, as two of the three captures carried - it costs eight
        // bytes and it exercises the label array, which is the one part of the
        // layout with its own alignment rule.
        Ancs::Label dismiss;
        dismiss.text = "Dismiss";
        notification.labels.push_back(dismiss);

        const size_t before = Ancs::encodedSize(notification);
        Ancs::truncateToFit(notification);
        const bool trimmed = Ancs::encodedSize(notification) != before;

        m_logbookTestInFlight = true;
        m_lastNotificationId = notification.notificationId;
        m_whiteboardClient->sendNotification(notification,
                // `answered` is for the daemon, which retries a watch that said
                // nothing. This is a button somebody pressed: it reports what
                // happened once and does not try again on their behalf.
                [this, session, trimmed](bool ok, bool, const QString &error) {
            m_logbookTestInFlight = false;
            if (!ok) {
                emit logbookTestResult(error);
                return;
            }
            emit logbookTestResult(trimmed
                    ? tr("The watch took the notification, with the text trimmed to fit")
                    : tr("The watch took the notification"));
        });
    });
}

void AppController::testNotificationRemove()
{
    withWatchSession([this](const WatchSession &session) {
        if (m_lastNotificationId == 0) {
            emit logbookTestResult(tr("Nothing has been sent from here yet"));
            return;
        }
        if (m_logbookTestInFlight || m_workoutSyncInProgress) {
            emit logbookTestResult(tr("Another fetch is already in progress"));
            return;
        }

        m_logbookTestInFlight = true;
        m_whiteboardClient->removeNotification(m_lastNotificationId,
                [this, session](bool ok, bool, const QString &error) {
            m_logbookTestInFlight = false;
            m_lastNotificationId = 0;
            emit logbookTestResult(ok ? tr("The watch removed it") : error);
        });
    });
}

void AppController::attachWhenLinkIsFree(const QString &objectPath, int attemptsLeft)
{
    QString error;
    if (m_watchLink.tryClaim(&error)) {
        m_whiteboardClient->attachToDevice(objectPath);
        return;
    }

    if (attemptsLeft > 0) {
        // 250 ms apart, so five seconds in total - far longer than the
        // daemon needs for one notification, and short enough that nobody
        // watching the screen notices.
        QTimer::singleShot(250, this, [this, objectPath, attemptsLeft]() {
            if (objectPath == m_pairedWatch.objectPath && !m_whiteboardReady)
                attachWhenLinkIsFree(objectPath, attemptsLeft - 1);
        });
        return;
    }

    // Five seconds of somebody else holding it means something is wrong
    // with them, not with us. Attach anyway and say so: a stuck daemon
    // should not make the watch unusable from the app.
    qWarning() << "attaching without the watch link after waiting:" << error;
    m_whiteboardClient->attachToDevice(objectPath);
}

void AppController::releaseWatchLink()
{
    m_watchLink.release();
}

AppController::WatchSession AppController::currentWatchSession()
{
    WatchSession session = m_watchSession.lock();
    if (session)
        return session;

    // The token is a real allocation rather than a null pointer with a
    // deleter, so there is no question about whether the deleter runs.
    session = WatchSession(new char, [this](void *token) {
        delete static_cast<char *>(token);
        QTimer::singleShot(0, this, [this]() { releaseIdleWatchSession(); });
    });
    m_watchSession = session;
    m_watchSessionHeld.start();
    return session;
}

void AppController::abandonWatchSession()
{
    // Only when nothing holds one. An attach that failed while a previous
    // session is still alive - two operations overlapping - must not pull
    // the watch out from under the one that is working.
    if (!m_watchSession.expired())
        return;
    m_whiteboardClient->detach();
    releaseWatchLink();
    m_watchSessionHeld.invalidate();
}

void AppController::releaseIdleWatchSession()
{
    // Somebody took a new session between the last holder dying and this
    // running - which is the common case when two operations follow each
    // other - so there is nothing to let go of.
    if (!m_watchSession.expired())
        return;
    if (!m_whiteboardReady)
        return;

    // How long the watch was held, beside the line that says it was taken.
    // Without this, working out that a session had leaked took the daemon's
    // log, this log, the lock file and the age of the app's own process -
    // and the one number that would have said so immediately was how long
    // the hold had lasted. An operation is hundreds of milliseconds; a leak
    // is however long the app has been open.
    if (m_watchSessionHeld.isValid()) {
        const QString line = QStringLiteral("watch session released after %1 ms")
                .arg(m_watchSessionHeld.elapsed());
        qWarning().noquote() << line;
        appendProbeLog(line);
        m_watchSessionHeld.invalidate();
    }

    m_whiteboardClient->detach();
    releaseWatchLink();
}

void AppController::withWatchSession(
        const std::function<void(const WatchSession &)> &operation)
{
    if (!m_watchConnected || m_pairedWatch.objectPath.isEmpty()) {
        emit errorOccurred(tr("Connect the watch first."));
        return;
    }
    if (m_whiteboardReady) {
        operation(currentWatchSession());
        return;
    }

    m_watchSessionWaiters.append(operation);
    if (m_watchSessionAttaching)
        return;
    m_watchSessionAttaching = true;
    m_watchAttachTimer.start();

    // An attach that never finishes has to fail the waiters itself. The
    // client reports a handshake that goes unanswered after ten seconds,
    // but it reports it through errorOccurred() and not through
    // readyChanged() - it was never ready, so there is no change to
    // announce - and without this the operations queued behind it would
    // wait for the rest of the run of the app.
    const int generation = ++m_watchSessionAttachGeneration;
    QTimer::singleShot(20000, this, [this, generation]() {
        if (!m_watchSessionAttaching || generation != m_watchSessionAttachGeneration)
            return;
        m_watchSessionAttaching = false;
        // Let the lock go. attachWhenLinkIsFree() takes it *before* the
        // attach, and an attach that never finishes - which is what a
        // wedged or frozen watch produces - used to leave it held for the
        // rest of the run: no session token is ever created, so nothing
        // was ever destroyed and releaseIdleWatchSession() never ran. The
        // daemon then starved, logged "the other process is using the
        // watch" by the minute, and gave up on notifications the watch
        // never saw. Measured with /proc/locks: the application's pid
        // holding an FLOCK on watch-link.lock with no session of any kind
        // in its own log.
        abandonWatchSession();
        if (m_watchSessionWaiters.isEmpty())
            return;
        m_watchSessionWaiters.clear();
        emit errorOccurred(tr("The watch did not answer."));
    });

    // Waits for the daemon to finish whatever it is sending, then attaches
    // anyway if it takes longer than five seconds - see
    // attachWhenLinkIsFree().
    attachWhenLinkIsFree(m_pairedWatch.objectPath);
}

namespace {
// The watch's own notification switch. The official app reads it on every
// connection; the value is what decides whether a notification PUT is
// accepted or refused with 400.
const char *const kAncsEnabledPath = "/Settings/Ble/AncsEnabled";
} // namespace

void AppController::readWatchNotificationsEnabled()
{
    withWatchSession([this](const WatchSession &session) {
        m_whiteboardClient->readValue(QString::fromLatin1(kAncsEnabledPath),
                [this, session](bool ok, const std::vector<uint8_t> &body, const QString &error) {
            if (!ok) {
                emit watchNotificationsRead(-1, error);
                return;
            }
            if (body.size() < 2) {
                emit watchNotificationsRead(
                        -1, tr("the watch answered with %1 bytes").arg(body.size()));
                return;
            }
            // The reply ends in the value as a 16-bit little-endian integer:
            // ...c8 00 01 00 00 on a 9 Baro, ...c8 00 01 00 01 on a Race.
            const int value = body[body.size() - 2] | (body[body.size() - 1] << 8);
            emit watchNotificationsRead(value != 0 ? 1 : 0, QString());
        });
    });
}

void AppController::setWatchNotificationsEnabled(bool enabled)
{
    withWatchSession([this, enabled](const WatchSession &session) {
        m_whiteboardClient->putSmallEnum(QString::fromLatin1(kAncsEnabledPath),
                                          enabled ? 1 : 0,
                [this, session, enabled](bool ok, const QString &error) {
            if (!ok) {
                emit logbookTestResult(error);
                // Put the switch back where the watch has it.
                readWatchNotificationsEnabled();
                return;
            }
            emit logbookTestResult(enabled
                    ? tr("The watch will accept notifications now")
                    : tr("The watch will ignore notifications now"));
            readWatchNotificationsEnabled();
        });
    });
}

void AppController::probePath(const QString &path)
{
    withWatchSession([this, path](const WatchSession &session) {
        if (m_logbookTestInFlight || m_workoutSyncInProgress) {
            emit logbookTestResult(tr("Another fetch is already in progress"));
            return;
        }

        m_logbookTestInFlight = true;
        m_whiteboardClient->readValue(path,
                [this, session, path](bool ok, const std::vector<uint8_t> &body, const QString &error) {
            m_logbookTestInFlight = false;
            if (!ok) {
                // readValue() says "not on this watch" for the f5 rejection,
                // which is the answer this probe usually exists to get.
                emit logbookTestResult(tr("%1\n%2").arg(path, error));
                return;
            }

            QString hex;
            for (size_t i = 0; i < body.size() && i < 40; ++i)
                hex += QStringLiteral("%1 ").arg(body[i], 2, 16, QLatin1Char('0'));
            QString ascii;
            for (size_t i = 0; i < body.size() && i < 48; ++i) {
                const uint8_t c = body[i];
                ascii += (c >= 0x20 && c < 0x7F) ? QChar(c) : QLatin1Char('.');
            }
            emit logbookTestResult(tr("%1\nexists - %2 bytes\n%3\n\"%4\"")
                                    .arg(path).arg(body.size()).arg(hex.trimmed(), ascii));
        });
    });
}

namespace {

// A scalar resource's reply is [handle 6][status 2][type code 2][value].
// The type code is the same vocabulary the fetch parameters use - 0x0003
// for the small enum Format returns, 0x000C for the NUL-terminated string
// Date returns.
bool scalarPayload(const std::vector<uint8_t> &body, uint16_t *typeCode,
                    const uint8_t **value, size_t *length)
{
    if (body.size() < 10)
        return false;
    *typeCode = static_cast<uint16_t>(body[8] | (static_cast<uint16_t>(body[9]) << 8));
    *value = body.data() + 10;
    *length = body.size() - 10;
    return true;
}

QString scalarString(const std::vector<uint8_t> &body)
{
    uint16_t typeCode = 0;
    const uint8_t *value = nullptr;
    size_t length = 0;
    if (!scalarPayload(body, &typeCode, &value, &length))
        return QString();
    const void *nul = std::memchr(value, 0, length);
    const size_t end = nul ? static_cast<size_t>(static_cast<const uint8_t *>(nul) - value)
                            : length;
    return QString::fromLatin1(reinterpret_cast<const char *>(value), static_cast<int>(end));
}

constexpr const char *kEphemerisFormatPath = "/Device/GNSS/ExtendedEphemerisData/Format";
constexpr const char *kEphemerisDatePath = "/Device/GNSS/ExtendedEphemerisData/Date";

// The commit is answered with 202 Accepted, and the watch then validates
// the data on its own time. A Suunto Race has a new date about three
// seconds later; a 9 Baro, older hardware handed nearly three times as
// much data, takes minutes. So this polls instead of waiting once for a
// fixed time that would be wrong for one of them - which is exactly the
// mistake the first version made, reporting failure on a watch that was
// merely still working.
constexpr int kEphemerisPollMs = 3000;
constexpr int kEphemerisPollAttempts = 60; // three minutes

} // namespace

void AppController::updateWatchGps()
{
    withWatchSession([this](const WatchSession &session) {
        m_gpsSession = session;
        if (m_gpsUpdateInProgress || m_workoutSyncInProgress || m_healthSyncInProgress) {
            emit errorOccurred(tr("Something else is using the watch connection."));
            return;
        }

        m_gpsUpdateInProgress = true;
        emit gpsUpdateInProgressChanged();

        // Ask the watch which of the four formats it wants rather than
        // deciding from the model name. That is what makes one code path serve
        // watches this project has never seen.
        m_whiteboardClient->readValue(QString::fromLatin1(kEphemerisFormatPath),
                [this, session](bool ok, const std::vector<uint8_t> &body, const QString &error) {
            if (!ok) {
                finishGpsUpdate(tr("Could not ask the watch which GPS data it wants: %1")
                                          .arg(error));
                return;
            }
            uint16_t typeCode = 0;
            const uint8_t *value = nullptr;
            size_t length = 0;
            if (!scalarPayload(body, &typeCode, &value, &length) || length < 1) {
                finishGpsUpdate(tr("The watch's GPS format answer was unreadable."));
                return;
            }
            const int format = value[0];

            m_cloudClient->downloadEphemeris(format,
                    [this, session, format](bool downloadOk, const QByteArray &data, const QString &dlError) {
                if (!downloadOk) {
                    finishGpsUpdate(tr("Could not download GPS data: %1").arg(dlError));
                    return;
                }

                const std::vector<uint8_t> blob(
                        reinterpret_cast<const uint8_t *>(data.constData()),
                        reinterpret_cast<const uint8_t *>(data.constData()) + data.size());
                emit errorOccurred(tr("Sending %1 kB of GPS data to the watch…")
                                    .arg(blob.size() / 1024));

                m_whiteboardClient->putEphemeris(blob, nullptr,
                        [this, session, format](bool putOk, const QString &putError) {
                    if (!putOk) {
                        finishGpsUpdate(putError);
                        return;
                    }
                    // The commit answers 202 Accepted, which is the watch
                    // saying it has taken the data - not that it has finished
                    // validating 60 kB of orbit predictions. Reading the date
                    // straight away catches it mid-thought, so give it a
                    // moment first. Read it anyway if the wait is wrong: a
                    // stale answer is information too, and it is reported as
                    // what it is rather than as a failure.
                    pollEphemerisDate(format, 0);
                });
            });
        });
    });
}

void AppController::pollEphemerisDate(int format, int attempt)
{
    m_whiteboardClient->readValue(QString::fromLatin1(kEphemerisDatePath),
            [this, format, attempt](bool ok, const std::vector<uint8_t> &body, const QString &) {
        const QString date = ok ? scalarString(body) : QString();

        // "N/A" is the watch saying it holds no ephemeris - which is also
        // what it says while it is busy replacing one, so it is a reason
        // to wait rather than a verdict.
        if (!date.isEmpty() && date != QLatin1String("N/A")) {
            const int seconds = attempt * kEphemerisPollMs / 1000;
            finishGpsUpdate(seconds > 5
                    ? tr("GPS data updated - the watch reports %1, after %2 s of processing.")
                              .arg(date).arg(seconds)
                    : tr("GPS data updated - the watch now reports %1.").arg(date));
            return;
        }

        if (attempt + 1 >= kEphemerisPollAttempts) {
            finishGpsUpdate(tr("GPS data sent (format %1) and accepted, but the watch still "
                                "reports no date after %2 minutes. Check it again later - "
                                "older watches take a while.")
                              .arg(format)
                              .arg(kEphemerisPollAttempts * kEphemerisPollMs / 60000));
            return;
        }

        QTimer::singleShot(kEphemerisPollMs, this, [this, format, attempt]() {
            pollEphemerisDate(format, attempt + 1);
        });
    });
}

void AppController::finishGpsUpdate(const QString &message)
{
    m_gpsUpdateInProgress = false;
    m_gpsSession.reset();
    emit gpsUpdateInProgressChanged();
    emit errorOccurred(message);
}

void AppController::testHealthResourceFetch(const QString &kind)
{
    withWatchSession([this, kind](const WatchSession &session) {
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
                [this, session, resource, filename](bool ok, const std::vector<uint8_t> &data,
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
    withWatchSession([this](const WatchSession &session) {
        if (m_logbookTestInFlight || m_workoutSyncInProgress) {
            emit logbookTestResult(tr("Another fetch is already in progress"));
            return;
        }
        m_logbookTestInFlight = true;
        tryActivityTrendEncoding(0);
    });
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

bool AppController::canDeleteWorkout(const QString &key) const
{
    if (!key.startsWith(QStringLiteral("ble_")))
        return false;
    return !m_workoutStore->isSmlUploaded(key);
}

void AppController::deleteWorkout(const QString &key)
{
    if (!canDeleteWorkout(key)) {
        emit errorOccurred(tr("That workout cannot be deleted here."));
        return;
    }

    QString error;
    if (!m_workoutStore->remove(key, &error)) {
        emit errorOccurred(tr("Could not delete the workout: %1").arg(error));
        return;
    }
    loadCachedWorkouts();
    emit workoutDeleted(key);
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
QByteArray AppController::fitFromWatchWorkout(const QString &key,
                                               const QByteArray &payload) const
{
    const std::vector<uint8_t> raw(
            reinterpret_cast<const uint8_t *>(payload.constData()),
            reinterpret_cast<const uint8_t *>(payload.constData()) + payload.size());

    Fit::Activity activity;
    try {
        activity = Fit::activityFromWatchData(raw, m_descriptorTable);
    } catch (const std::exception &) {
        return QByteArray();
    }
    if (activity.samples.empty())
        return QByteArray();

    // The summary the app itself shows, rather than figures derived again
    // from the samples: a watch workout's ascent, descent and energy come
    // from its /Summary resource, which is the watch's own arithmetic and
    // the thing worth carrying into the file.
    Workout row;
    for (const Workout &w : m_workoutStore->loadAll(nullptr)) {
        if (w.key == key) {
            row = w;
            break;
        }
    }
    Fit::Summary &summary = activity.summary;
    if (row.startTime != 0)
        summary.startTime = row.startTime / 1000;
    if (row.totalTime > 0) {
        summary.timer = row.totalTime;
        // The row keeps the moving time; the span to the last sample is
        // the elapsed one, and saying they are equal would hide the pauses.
        const double span = static_cast<double>(activity.samples.back().time
                                                 - activity.samples.front().time);
        summary.elapsed = std::max(span, row.totalTime);
    }
    if (row.totalDistance > 0)
        summary.distance = row.totalDistance;
    if (row.maxSpeed > 0)
        summary.maxSpeed = row.maxSpeed;
    if (row.avgHeartRate > 0)
        summary.avgHeartRate = static_cast<int>(row.avgHeartRate);
    if (row.maxHeartRate > 0)
        summary.maxHeartRate = static_cast<int>(row.maxHeartRate);
    if (row.totalAscent > 0)
        summary.ascent = row.totalAscent;
    if (row.totalDescent > 0)
        summary.descent = row.totalDescent;
    if (row.energyConsumption > 0)
        summary.calories = static_cast<int>(row.energyConsumption);
    // 23 is Suunto's own FIT manufacturer id, which is what the cloud
    // writes into its exports of this watch's workouts. The product and
    // the serial are left unstated rather than guessed.
    summary.manufacturer = 23;
    int sport = -1;
    int subSport = -1;
    if (Fit::fitSportFromSuunto(row.activityId, &sport, &subSport)) {
        summary.sport = sport;
        summary.subSport = subSport;
    }

    // The laps as stored, which for a watch workout are the stretches
    // between its own markers. Their start times are the running sum: the
    // stored form carries a duration and not a clock.
    const QJsonArray laps = QJsonDocument::fromJson(m_workoutStore->loadLaps(key)).array();
    int64_t at = summary.startTime;
    for (const QJsonValue &value : laps) {
        const QJsonObject object = value.toObject();
        const double seconds = object.value(QStringLiteral("durationSeconds")).toDouble();
        if (seconds <= 0)
            continue;
        Fit::Lap lap;
        lap.startTime = at;
        lap.elapsed = seconds;
        lap.timer = seconds;
        if (object.contains(QStringLiteral("distanceMeters")))
            lap.distance = object.value(QStringLiteral("distanceMeters")).toDouble();
        if (object.contains(QStringLiteral("avgSpeed")))
            lap.avgSpeed = object.value(QStringLiteral("avgSpeed")).toDouble();
        if (object.contains(QStringLiteral("avgHeartRate"))) {
            lap.avgHeartRate =
                    static_cast<int>(object.value(QStringLiteral("avgHeartRate")).toDouble());
        }
        if (object.contains(QStringLiteral("ascent")))
            lap.ascent = object.value(QStringLiteral("ascent")).toDouble();
        if (object.contains(QStringLiteral("energy")))
            lap.calories = static_cast<int>(object.value(QStringLiteral("energy")).toDouble());
        activity.laps.push_back(lap);
        at += static_cast<int64_t>(seconds);
    }

    const std::vector<uint8_t> encoded = Fit::encodeActivity(activity);
    return QByteArray(reinterpret_cast<const char *>(encoded.data()),
                       static_cast<int>(encoded.size()));
}

QString AppController::sourceOfWorkout(const QString &key) const
{
    for (const Workout &w : m_workoutStore->loadAll(nullptr)) {
        if (w.key == key)
            return w.source;
    }
    return QString();
}

QByteArray AppController::buildUploadZip(const QString &key) const
{
    QByteArray data, summary;
    if (!m_workoutStore->loadSmlSources(key, &data, &summary))
        return QByteArray();

    // Timestamps come from the stored workout, not from "now".
    qint64 startTime = 0;
    qint64 stopTime = 0;
    int activityId = 0;
    QString source;
    for (const Workout &w : m_workoutStore->loadAll(nullptr)) {
        if (w.key == key) {
            startTime = w.startTime;
            stopTime = w.stopTime;
            activityId = w.activityId;
            source = w.source;
            break;
        }
    }

    // An imported workout's stored "data" is the FIT file, not a /Data
    // payload. The row's own source says which, rather than the key's
    // prefix: the prefix test worked, but a cloud workout's key is a
    // sixteen-character string the server picks, and one of the ones in
    // this database already begins "fit" - "fiti5dkqv6ih1ugn". Only the
    // underscore kept that apart, which is a thinner margin than a column
    // that states the answer.
    //
    // The two documents are the same shape either way; only what they are
    // built from differs.
    if (source == QStringLiteral("fit")) {
        const std::vector<uint8_t> raw(
                reinterpret_cast<const uint8_t *>(data.constData()),
                reinterpret_cast<const uint8_t *>(data.constData()) + data.size());
        Fit::Activity activity;
        std::string decodeError;
        if (!Fit::parseActivity(raw, activity, decodeError))
            return QByteArray();

        const int offsetMinutes =
                QDateTime::fromMSecsSinceEpoch(startTime).offsetFromUtc() / 60;
        const std::string src = fitSourceName(activity.summary);
        const std::string samples = SmlJson::buildFitSamples(activity, src, offsetMinutes);
        const std::string header =
                SmlJson::buildFitSummary(activity, src, offsetMinutes, activityId);

        std::vector<ZipWriter::Entry> entries;
        if (!samples.empty())
            entries.push_back({ "samples.json", samples });
        if (!header.empty())
            entries.push_back({ "summary.json", header });
        if (entries.empty())
            return QByteArray();
        const std::vector<uint8_t> zip = ZipWriter::build(entries);
        return QByteArray(reinterpret_cast<const char *>(zip.data()),
                           static_cast<int>(zip.size()));
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

    setUploadInProgress(true);
    uploadWorkoutAt(keys, 0, 0, QStringList());
}

void AppController::uploadWorkoutAt(const QVector<QString> &keys, int index, int succeeded,
                                     const QStringList &failures)
{
    if (index >= keys.size()) {
        setUploadInProgress(false);
        emit workoutUploadProgress(keys.size(), keys.size());
        if (failures.isEmpty()) {
            emit errorOccurred(tr("Uploaded %1 workouts to Suunto.").arg(succeeded));
        } else {
            emit errorOccurred(tr("Uploaded %1 of %2 workouts. Failed: %3")
                                .arg(succeeded).arg(keys.size())
                                .arg(failures.join(QStringLiteral("; "))));
        }
        loadCachedWorkouts();
        // **Fetch back what was just sent.** Only the cloud scores a
        // workout, so an upload without a read leaves the fitness figures
        // behind by exactly the workouts that were uploaded - which is the
        // state that read 14/4/+9 against the official app's 17/19/0 on
        // 2026-10-19.
        //
        // **The cloud scores it immediately**, measured 2026-10-23: a
        // workout sent from the watch moved the figures the moment this
        // read came back, so no retry and no waiting is needed. And if a
        // future upload ever does arrive unscored, nothing is lost - the
        // cursor is the cloud's `lastModified`, so the workout comes back
        // on an ordinary sync once the score exists.
        if (succeeded > 0)
            syncCloudWorkouts();
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

    setUploadInProgress(true);
    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key, zip](bool ok, const QByteArray &data, const QString &vaultError) {
        if (!ok) {
            setUploadInProgress(false);
            emit workoutUploaded(key, false,
                                  tr("Could not read the stored session: %1").arg(vaultError));
            return;
        }
        m_cloudClient->uploadWorkout(QString::fromUtf8(data), zip,
                [this, key](bool uploadOk, const QString &cloudKey, const QString &error) {
            setUploadInProgress(false);
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
            // And read it back for its training stress score - see the
            // batch path's own note. The duplicate row this creates is
            // hidden by the list's own filter, which has the cloud key
            // this upload just recorded.
            syncCloudWorkouts();
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
    if (m_healthSyncInProgress || m_workoutSyncInProgress) {
        emit errorOccurred(tr("A sync is already in progress."));
        return;
    }

    withWatchSession([this](const WatchSession &session) {
        m_healthSyncInProgress = true;
        emit healthSyncInProgressChanged();
        startWatchHealthSync(session);
    });
}

void AppController::startWatchHealthSync(const WatchSession &session)
{

    // Ask for everything newer than what is already stored, falling back to
    // a fortnight on an empty database - the watch keeps a limited history
    // anyway, so asking for more costs nothing.
    qint64 since = m_healthStore->newestTimestamp(QStringLiteral("sleep"));
    if (since == 0)
        since = QDateTime::currentMSecsSinceEpoch() - 14LL * 24 * 3600 * 1000;

    m_whiteboardClient->fetchTimelineFile(
            QStringLiteral("/Daily/Sleep/Timeline/Data"),
            QStringLiteral("mdsSlp.sbm"), since,
            [this, session](bool ok, const std::vector<uint8_t> &data, const QString &error) {
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
        fetchWatchRecovery(session);
    });
}

void AppController::fetchWatchRecovery(const WatchSession &session)
{
    qint64 since = m_healthStore->newestTimestamp(QStringLiteral("recovery"));
    if (since == 0)
        since = QDateTime::currentMSecsSinceEpoch() - 14LL * 24 * 3600 * 1000;

    m_healthSyncInProgress = true;
    emit healthSyncInProgressChanged();

    // Seconds, not milliseconds - see fetchRecoveryMoments().
    m_whiteboardClient->fetchRecoveryMoments(since / 1000,
            [this, session](bool ok, const std::vector<uint8_t> &data, const QString &error) {
        if (!ok) {
            // Sleep already succeeded by this point, so this is reported
            // but not treated as a failed sync - and daily activity is a
            // third resource again, so it still gets its turn.
            emit errorOccurred(tr("Sleep synced. Recovery failed: %1").arg(error));
            fetchWatchActivity(session);
            return;
        }

        const std::vector<RecoveryMoments::Sample> samples = RecoveryMoments::decode(data);
        if (samples.empty()) {
            emit errorOccurred(tr("Sleep synced. Recovery returned %1 bytes but no readable samples.")
                                .arg(data.size()));
            fetchWatchActivity(session);
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
            fetchWatchActivity(session);
            return;
        }
        emit healthDataChanged();

        // Daily activity: a third resource, a third mechanism. Chained
        // rather than fired alongside, because the whiteboard link takes
        // one request at a time.
        fetchWatchActivity(session);
    });
}

void AppController::fetchWatchActivity(const WatchSession &session)
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
            [this, session](bool ok, const std::vector<ActivityTrend::Sample> &samples,
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

void AppController::setBaseMapEnabled(bool enabled)
{
    if (enabled == m_baseMapEnabled)
        return;
    m_baseMapEnabled = enabled;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("map/enabled"), enabled);
    emit baseMapChanged();
}

void AppController::setMapTileUrl(const QString &url)
{
    // An emptied field means "back to the default" rather than "no map":
    // leaving it blank with the switch still on would be a switch that does
    // nothing, which is the one thing this project has learned to avoid.
    const QString value = url.trimmed().isEmpty() ? kDefaultMapTileUrl : url.trimmed();
    if (value == m_mapTileUrl)
        return;
    m_mapTileUrl = value;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("map/tileUrl"), value);
    emit baseMapChanged();
}

QString AppController::defaultMapTileUrl() const
{
    return kDefaultMapTileUrl;
}

void AppController::setUploadInProgress(bool busy)
{
    if (busy == m_uploadInProgress)
        return;
    m_uploadInProgress = busy;
    emit uploadInProgressChanged();
}

void AppController::setCoverAction(const QString &action)
{
    if (action == m_coverAction)
        return;
    m_coverAction = action;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("cover/action"), action);
    emit coverModeChanged();
}

void AppController::runCoverAction()
{
    // "all" is the four steps in the order that makes sense: read from the
    // watch first, then send what was read onwards. The other values are
    // one step each.
    if (m_coverAction == QStringLiteral("all")) {
        m_coverChain = QStringList{ QStringLiteral("watchWorkouts"),
                                     QStringLiteral("watchHealth"),
                                     QStringLiteral("uploadWorkouts"),
                                     QStringLiteral("uploadHealth") };
    } else {
        m_coverChain = QStringList{ m_coverAction.isEmpty()
                ? QStringLiteral("watchWorkouts") : m_coverAction };
    }
    startNextCoverStep();
}

void AppController::startNextCoverStep()
{
    // Anything running means this is not the moment: whichever flag is up
    // will call back here when it clears.
    if (m_workoutSyncInProgress || m_healthSyncInProgress || m_uploadInProgress)
        return;

    while (!m_coverChain.isEmpty()) {
        const QString step = m_coverChain.takeFirst();
        // A step whose precondition fails is skipped, not reported: the
        // cover's one button is not the place to explain that the watch is
        // out of range, and the next step may well be able to run.
        if (step == QStringLiteral("watchWorkouts")) {
            if (!m_watchConnected)
                continue;
            syncWatchWorkouts();
            return;
        }
        if (step == QStringLiteral("watchHealth")) {
            if (!m_watchConnected)
                continue;
            syncWatchHealth();
            return;
        }
        if (step == QStringLiteral("uploadWorkouts")) {
            if (!m_cloudAccount.isSignedIn() || m_workoutStore->keysPendingUpload().isEmpty())
                continue;
            uploadAllWorkouts();
            return;
        }
        if (step == QStringLiteral("uploadHealth")) {
            if (!m_cloudAccount.isSignedIn() || m_healthStore->pendingUploadCount() == 0)
                continue;
            uploadHealthToCloud();
            return;
        }
    }
}

void AppController::setCoverMode(const QString &mode)
{
    if (mode == m_coverMode)
        return;
    m_coverMode = mode;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("cover/mode"), mode);
    emit coverModeChanged();
}

void AppController::setCoverSteps(bool enabled)
{
    if (enabled == m_coverSteps)
        return;
    m_coverSteps = enabled;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("cover/steps"), enabled);
    // The same signal coverMode uses: the cover re-reads its summary on
    // it, which is exactly what a change here needs.
    emit coverModeChanged();
}


void AppController::setAutoUploadSuunto(bool enabled)
{
    if (enabled == m_autoUploadSuunto)
        return;
    m_autoUploadSuunto = enabled;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("upload/autoSuunto"), enabled);
    emit autoUploadChanged();
}

void AppController::autoUploadAfterSync(const QVector<QString> &keys)
{
    if (keys.isEmpty())
        return;

    // The existing action, unchanged: it uploads every watch workout the
    // cloud hasn't taken, which after a sync is these and anything an
    // earlier sync left behind.
    if (m_autoUploadSuunto && m_cloudAccount.isSignedIn() && !m_uploadInProgress
            && !m_workoutStore->keysPendingUpload().isEmpty()) {
        uploadAllWorkouts();
    }
}

QString AppController::wifiEnablePath() const
{
    // The firmware version is not read anywhere in this app, so the newer
    // of the two resources is the default; sendWifiToWatch() falls back to
    // the other one when the watch rejects it, which is a measurement
    // rather than a guess.
    return QStringLiteral("/Settings/Wifi/Enabled");
}

void AppController::setWifiCredentials(const QString &ssid, const QString &passphrase)
{
    const QString trimmed = ssid.trimmed();
    if (trimmed.isEmpty()) {
        emit errorOccurred(tr("Enter the network's name."));
        return;
    }
    m_wifiSsid = trimmed;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("wifi/ssid"), trimmed);

    // An open network is a real case and stores no passphrase, rather than
    // storing an empty one that would later look like a forgotten key.
    if (passphrase.isEmpty()) {
        m_tokenVault->deleteSecret(CloudAccountStore::WifiKeySecretName,
                [this](bool, const QString &) {
            m_wifiKeyStored = false;
            QSettings(settingsPath(), QSettings::IniFormat)
                    .setValue(QStringLiteral("wifi/keyStored"), false);
            emit wifiChanged();
            emit errorOccurred(tr("Saved %1 with no passphrase.").arg(m_wifiSsid));
        });
        return;
    }

    m_tokenVault->storeSecret(CloudAccountStore::WifiKeySecretName, passphrase.toUtf8(),
            [this](bool ok, const QString &error) {
        if (!ok) {
            emit errorOccurred(tr("Could not store the passphrase: %1").arg(error));
            return;
        }
        m_wifiKeyStored = true;
        QSettings(settingsPath(), QSettings::IniFormat)
                .setValue(QStringLiteral("wifi/keyStored"), true);
        emit wifiChanged();
        emit errorOccurred(tr("Saved %1.").arg(m_wifiSsid));
    });
}

void AppController::forgetWifiCredentials()
{
    m_tokenVault->deleteSecret(CloudAccountStore::WifiKeySecretName,
            [this](bool, const QString &) {
        m_wifiSsid.clear();
        m_wifiKeyStored = false;
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.remove(QStringLiteral("wifi/ssid"));
        settings.setValue(QStringLiteral("wifi/keyStored"), false);
        emit wifiChanged();
        emit errorOccurred(tr("Forgot the network."));
    });
}

void AppController::finishWifi(const QString &message)
{
    m_wifiBusy = false;
    m_wifiSession.reset();
    emit wifiBusyChanged();
    emit wifiResult(message);
}

void AppController::sendWifiToWatch()
{
    if (m_wifiBusy)
        return;
    if (m_wifiSsid.isEmpty()) {
        emit errorOccurred(tr("Save a network first."));
        return;
    }

    // The watch has to say it has WiFi at all. A 9 Baro does not, and
    // writing these resources to one would be asking a question with no
    // answer; the Race answers `hw_wifi_1`, so the test is a prefix.
    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();

        m_whiteboardClient->fetchCapabilities(
                [this](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
            if (!ok) {
                finishWifi(tr("Could not read the watch's capabilities: %1").arg(error));
                return;
            }
            bool hasWifi = false;
            for (const std::string &token : Capabilities::parse(payload)) {
                if (QString::fromStdString(token).startsWith(QStringLiteral("hw_wifi"))) {
                    hasWifi = true;
                    break;
                }
            }
            if (!hasWifi) {
                finishWifi(tr("This watch says it has no WiFi (no hw_wifi in its "
                               "capabilities)."));
                return;
            }
            enableWatchWifi();
        });
    });
}

void AppController::enableWatchWifi()
{
    // Step 1: switch WiFi on. Both resources are tried, newest first,
    // because which one a watch has depends on its firmware and this app
    // does not read the version. Whichever answers is reported.
    const QString primary = wifiEnablePath();
    const QString fallback = QStringLiteral("/Settings/Connectivity/Current/Wifi/Enabled");

    m_whiteboardClient->setWifiEnabled(primary, true,
            [this, primary, fallback](bool ok, const QString &error) {
        if (ok) {
            joinWatchWifi(tr("WiFi enabled (%1).").arg(primary));
            return;
        }
        m_whiteboardClient->setWifiEnabled(fallback, true,
                [this, primary, fallback, error](bool fallbackOk, const QString &fallbackError) {
            if (fallbackOk) {
                joinWatchWifi(tr("WiFi enabled (%1; %2 was refused: %3).")
                                        .arg(fallback, primary, error));
                return;
            }
            // Neither worked, which is worth reporting in full rather than
            // retrying: both answers name the resource that refused them.
            finishWifi(tr("Could not switch WiFi on. %1: %2. %3: %4.")
                                .arg(primary, error, fallback, fallbackError));
        });
    });
}

void AppController::joinWatchWifi(const QString &soFar)
{
    // Step 2: the passphrase, out of the vault and straight onto the wire.
    // It is never held in a member and never reaches QML.
    if (!m_wifiKeyStored) {
        connectWatchWifi(soFar, QString());
        return;
    }
    m_tokenVault->loadSecret(CloudAccountStore::WifiKeySecretName,
            [this, soFar](bool ok, const QByteArray &data, const QString &error) {
        if (!ok) {
            finishWifi(tr("%1 Could not read the passphrase: %2").arg(soFar, error));
            return;
        }
        connectWatchWifi(soFar, QString::fromUtf8(data));
    });
}

void AppController::connectWatchWifi(const QString &soFar, const QString &passphrase)
{
    m_whiteboardClient->connectWifi(m_wifiSsid, passphrase,
            [this, soFar](bool ok, const std::vector<uint8_t> &body, const QString &error) {
        if (!ok) {
            finishWifi(tr("%1 The join request failed: %2").arg(soFar, error));
            return;
        }

        // The reply is a result structure and its layout is not decoded
        // yet, so the bytes are reported as they arrived. That is the
        // measurement this step exists for; once the layout is known this
        // turns into a sentence.
        const QString hex = QString::fromLatin1(
                QByteArray(reinterpret_cast<const char *>(body.data()),
                            static_cast<int>(body.size())).toHex());
        // Then ask the watch how it went, which is a resource of its own.
        m_whiteboardClient->readValue(
                QStringLiteral("/Device/Connectivity/Wifi/Connection/Status"),
                [this, soFar, hex](bool statusOk, const std::vector<uint8_t> &status,
                                     const QString &statusError) {
            const QString statusText = statusOk
                    ? QString::fromLatin1(
                            QByteArray(reinterpret_cast<const char *>(status.data()),
                                        static_cast<int>(status.size())).toHex())
                    : statusError;
            finishWifi(tr("%1 Join reply: %2. Status: %3.").arg(soFar, hex, statusText));
        });
    });
}

QString AppController::watchSerial() const
{
    return m_pairedWatch.name.section(QLatin1Char(' '), -1).trimmed();
}

void AppController::setMapsBusy(bool busy)
{
    if (busy == m_mapsBusy)
        return;
    m_mapsBusy = busy;
    // The session rides with the flag, the same way the SuuntoPlus one
    // does, so there is one place to forget rather than two.
    if (!busy)
        m_mapsSession.reset();
    emit mapsBusyChanged();
}

QVariantList AppController::mapCatalogue() const
{
    // `inLibrary` is computed here rather than stored, so one refresh of
    // either list keeps both honest.
    QSet<QString> owned;
    for (const QVariant &value : m_mapLibrary)
        owned.insert(value.toMap().value(QStringLiteral("id")).toString());

    QVariantList out;
    for (const QVariant &value : m_mapCatalogue) {
        QVariantMap row = value.toMap();
        row.insert(QStringLiteral("inLibrary"),
                    owned.contains(row.value(QStringLiteral("id")).toString()));
        out.append(row);
    }
    return out;
}

QVariantList AppController::mapLibrary() const
{
    return m_mapLibrary;
}

void AppController::refreshMaps()
{
    if (m_mapsBusy)
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    // The watch is needed for its capability string, which decides which
    // tile packages the cloud offers - the same parameter the SuuntoPlus
    // store takes, and the same context that fetches it.
    withWatchSession([this](const WatchSession &session) {
        m_mapsSession = session;
        setMapsBusy(true);
        withSuuntoPlusContext([this](const QString &key, const QString &,
                                       const QString &capabilities) {
            // The watch's part is over: it answered its capabilities, and
            // everything below is HTTPS. Holding the link through two
            // cloud requests blocks notifications for as long as they
            // take - measured at five seconds - and that is exactly what
            // the per-operation session was built to stop.
            m_mapsSession.reset();
            m_cloudClient->fetchMapCatalogue(key, capabilities,
                    [this, key, capabilities](bool ok, const QVariantList &regions,
                                                const QString &error) {
                if (!ok) {
                    setMapsBusy(false);
                    emit errorOccurred(tr("Map catalogue failed: %1").arg(error));
                    return;
                }
                m_mapCatalogue = regions;
                m_cloudClient->fetchMapLibrary(key, watchSerial(), capabilities,
                        [this](bool libraryOk, const QVariantList &library,
                                 const QString &libraryError) {
                    setMapsBusy(false);
                    if (libraryOk)
                        m_mapLibrary = library;
                    else
                        emit errorOccurred(tr("Map library failed: %1").arg(libraryError));
                    emit mapsChanged();
                });
            });
        }, [this]() { setMapsBusy(false); });
    });
}

void AppController::addMapRegion(const QString &regionId)
{
    if (m_mapsBusy || regionId.isEmpty())
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    withWatchSession([this, regionId](const WatchSession &session) {
        m_mapsSession = session;
        setMapsBusy(true);
        withSuuntoPlusContext([this, regionId](const QString &key, const QString &,
                                                 const QString &capabilities) {
            // Same here: the capability read is done, and the nudge at the
            // end takes the watch again for itself.
            m_mapsSession.reset();
            m_cloudClient->addMapRegion(key, regionId, watchSerial(), capabilities,
                    [this, key, capabilities](bool ok, const QString &error) {
                if (!ok) {
                    setMapsBusy(false);
                    emit errorOccurred(tr("The cloud would not take that region: %1")
                                                .arg(error));
                    return;
                }
                // The cloud has it; now the watch is told to go and look,
                // which is what actually starts a download.
                m_cloudClient->fetchMapLibrary(key, watchSerial(), capabilities,
                        [this](bool libraryOk, const QVariantList &library, const QString &) {
                    if (libraryOk)
                        m_mapLibrary = library;
                    emit mapsChanged();
                    notifyWatchMapsChanged(tr("Added to the library."));
                });
            });
        }, [this]() { setMapsBusy(false); });
    });
}

void AppController::removeMapRegion(const QString &regionId)
{
    if (m_mapsBusy || regionId.isEmpty())
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    withWatchSession([this, regionId](const WatchSession &session) {
        m_mapsSession = session;
        setMapsBusy(true);
        withSuuntoPlusContext([this, regionId](const QString &key, const QString &,
                                                 const QString &capabilities) {
            m_mapsSession.reset();
            m_cloudClient->removeMapRegion(key, regionId, watchSerial(), capabilities,
                    [this, key, capabilities](bool ok, const QString &error) {
                if (!ok) {
                    setMapsBusy(false);
                    emit errorOccurred(tr("The cloud would not drop that region: %1")
                                                .arg(error));
                    return;
                }
                m_cloudClient->fetchMapLibrary(key, watchSerial(), capabilities,
                        [this](bool libraryOk, const QVariantList &library, const QString &) {
                    if (libraryOk)
                        m_mapLibrary = library;
                    emit mapsChanged();
                    notifyWatchMapsChanged(tr("Removed from the library."));
                });
            });
        }, [this]() { setMapsBusy(false); });
    });
}

void AppController::notifyWatchMapsChanged(const QString &soFar)
{
    // Two writes and a read, all on the watch: the cloud credentials so it
    // can fetch at all, the nudge, and then how many areas it says it has.
    // The credentials every time for the same reason the weather sync does
    // it - a stale token is indistinguishable from a watch that has no
    // network.
    //
    // Its own session, because the caller let the watch go before its
    // cloud requests; this is the part that actually needs the link.
    withWatchSession([this, soFar](const WatchSession &session) {
        m_mapsSession = session;
        writeCloudCredentials([this, soFar](const QString &credentials) {
        m_whiteboardClient->putEmpty(QStringLiteral("/MapsDownload/AreaSelectionChanged"),
                [this, soFar, credentials](bool ok, const QString &error) {
            if (!ok) {
                setMapsBusy(false);
                emit errorOccurred(tr("%1 %2 The watch would not take the map selection: %3")
                                            .arg(soFar, credentials, error));
                return;
            }
            m_whiteboardClient->readValue(QStringLiteral("/MapsDownload/NumberOfAreas"),
                    [this, soFar, credentials](bool countOk, const std::vector<uint8_t> &body,
                                                 const QString &countError) {
                setMapsBusy(false);
                // The count is a scalar, the same shape /Settings/... values
                // have: a 16-bit value after the type code.
                QString areas = countError;
                uint16_t typeCode = 0;
                const uint8_t *value = nullptr;
                size_t length = 0;
                if (countOk && scalarPayload(body, &typeCode, &value, &length) && length >= 1) {
                    areas = QString::number(length >= 2
                            ? static_cast<int>(value[0] | (value[1] << 8))
                            : static_cast<int>(value[0]));
                }
                emit errorOccurred(tr("%1 %2 Accept the download on the watch; it holds "
                                       "%3 area(s) so far.")
                                            .arg(soFar, credentials, areas));
                });
            });
        }, [this, soFar](const QString &error) {
            setMapsBusy(false);
            emit errorOccurred(tr("%1 The watch was not told: %2").arg(soFar, error));
        });
    });
}

void AppController::writeCloudCredentials(
        const std::function<void(const QString &soFar)> &then,
        const std::function<void(const QString &error)> &onFailure)
{
    // Measured values, both of them: the official app writes exactly this
    // URL - the API base, despite the resource being named after offline
    // maps - and `WatchUserKey ` plus a sixteen-character key. See
    // docs/watch-push-resources.md for the capture and
    // docs/wifi-and-offline-maps.md for where the key comes from.
    static const QString kWatchCloudUrl =
            QStringLiteral("https://api.sports-tracker.com/apiserver/");

    if (!m_cloudAccount.isSignedIn()) {
        onFailure(tr("Sign in to the Suunto cloud first - the watch needs a credential of "
                      "its own, and only the cloud can mint one."));
        return;
    }

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, then, onFailure](bool ok, const QByteArray &session,
                                      const QString &vaultError) {
        if (!ok) {
            onFailure(tr("Could not read the stored session: %1").arg(vaultError));
            return;
        }
        m_cloudClient->fetchWatchKey(QString::fromUtf8(session),
                [this, then, onFailure](bool keyOk, const QString &watchKey,
                                          const QString &keyError) {
            if (!keyOk) {
                onFailure(tr("The cloud would not give the watch a key: %1").arg(keyError));
                return;
            }
            m_whiteboardClient->putString(
                    QStringLiteral("/Settings/Wifi/Cloud/OfflineMaps/Url"), kWatchCloudUrl,
                    [this, then, onFailure, watchKey](bool urlOk, const QString &urlError) {
                if (!urlOk) {
                    onFailure(tr("Could not write the cloud URL: %1").arg(urlError));
                    return;
                }
                m_whiteboardClient->putString(
                        QStringLiteral("/Settings/Wifi/Cloud/STTAuthorization"),
                        QStringLiteral("WatchUserKey %1").arg(watchKey),
                        [this, then, onFailure, watchKey](bool tokenOk,
                                                            const QString &tokenError) {
                    if (!tokenOk) {
                        onFailure(tr("Could not write the watch's key: %1").arg(tokenError));
                        return;
                    }
                    // The key itself is not shown - it is a credential -
                    // but its length is, because sixteen characters is
                    // what the capture had and a different number would
                    // mean this endpoint answers something else.
                    then(tr("Cloud credentials written (key of %1 characters).")
                                  .arg(watchKey.size()));
                });
            });
        });
    });
}

void AppController::sendCloudCredentialsToWatch()
{
    if (m_wifiBusy)
        return;
    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();
        writeCloudCredentials([this](const QString &soFar) {
            finishWifi(soFar);
        }, [this](const QString &error) { finishWifi(error); });
    });
}

void AppController::syncWatchWeather()
{
    if (m_wifiBusy)
        return;
    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();

        // **The credentials are the part that matters.** The watch fetches
        // its own forecast over WiFi with them, exactly as it fetches its
        // own GPS predictions, and a token it cannot use is the one
        // failure that looks like having no network at all.
        //
        // The /Weather/Sync below is a **subscribe**, not a command: in
        // both captures the official app GETs it and sends 0x10, the same
        // pair it sends for /Voice/Status, /FindPhone/PhoneState and
        // /Navigation/QuickNaviState - resources the *watch* pushes on.
        // Calling it a trigger was this project's reading, and the
        // captures do not support it. It is kept because it is what the
        // official app does at this point in a sync, and because
        // subscribing is harmless; the credential write is the mechanism.
        writeCloudCredentials([this](const QString &soFar) {
            m_whiteboardClient->triggerStreamSync(QStringLiteral("/Weather/Sync"),
                    [this, soFar](bool ok, const std::vector<uint8_t> &body,
                                    const QString &error) {
                if (!ok) {
                    finishWifi(tr("%1 The weather sync failed: %2").arg(soFar, error));
                    return;
                }
                // A short reply with no forecast in it is the *expected*
                // answer - the watch goes and fetches it over WiFi, and
                // its own event log is where the result shows up. So this
                // reports the bytes and says what to look at next rather
                // than claiming success.
                const QString hex = QString::fromLatin1(
                        QByteArray(reinterpret_cast<const char *>(body.data()),
                                    static_cast<int>(body.size())).toHex());
                finishWifi(tr("%1 Subscribed to /Weather/Sync (%2 bytes: %3). The watch "
                               "fetches the forecast itself over WiFi now that it has a "
                               "credential; look at its weather view in a minute.")
                                    .arg(soFar).arg(body.size()).arg(hex));
            });
        }, [this](const QString &error) { finishWifi(error); });
    });
}

namespace {

// One scalar reply, as text: the type code and the value, decoded when the
// type is one this project knows and shown as hex when it is not. Enough
// for a diagnostic, and it never pretends to understand more than it does.
QString scalarText(bool ok, const std::vector<uint8_t> &body, const QString &error)
{
    if (!ok)
        return error;
    uint16_t typeCode = 0;
    const uint8_t *value = nullptr;
    size_t length = 0;
    if (!scalarPayload(body, &typeCode, &value, &length))
        return AppController::tr("unreadable (%1 bytes)").arg(body.size());

    const QByteArray raw(reinterpret_cast<const char *>(value), static_cast<int>(length));
    switch (typeCode) {
    case Mds::kParamString:
        return QString::fromLatin1(raw).left(raw.indexOf('\0') < 0 ? raw.size()
                                                                    : raw.indexOf('\0'));
    case Mds::kParamSmallEnum:
        return length >= 1 ? QString::number(value[0]) : QString();
    case 0x0001:
        // The 16-bit value /Settings/... booleans read as - see
        // docs/notifications.md's AncsEnabled measurement.
        return length >= 2 ? QString::number(value[0] | (value[1] << 8))
                            : QString::number(value[0]);
    case Mds::kParamInt32:
        if (length >= 4) {
            const uint32_t v = static_cast<uint32_t>(value[0]) | (static_cast<uint32_t>(value[1]) << 8)
                    | (static_cast<uint32_t>(value[2]) << 16) | (static_cast<uint32_t>(value[3]) << 24);
            return QString::number(v);
        }
        break;
    case Mds::kParamInt64:
        if (length >= 8) {
            uint64_t v = 0;
            for (int i = 7; i >= 0; --i)
                v = (v << 8) | value[i];
            // The analytics log stamps events in microseconds since the
            // epoch (measured), and a time this size is almost certainly
            // one of those; a plain integer is shown beside it rather than
            // instead of it, because the unit is a reading and not a fact.
            const QDateTime asMicroseconds =
                    QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(v / 1000));
            return QStringLiteral("%1 (%2 if microseconds)")
                    .arg(QString::number(v),
                          asMicroseconds.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        }
        break;
    default:
        break;
    }
    return QStringLiteral("type 0x%1: %2")
            .arg(typeCode, 4, 16, QLatin1Char('0')).arg(QString::fromLatin1(raw.toHex()));
}

} // namespace

void AppController::readWatchWeatherState()
{
    if (m_wifiBusy)
        return;

    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();

        m_whiteboardClient->readValue(QStringLiteral("/Weather/Sync/LastTime"),
                [this](bool ok, const std::vector<uint8_t> &body, const QString &error) {
            // Measured: a 32-bit Unix time in **seconds**, and it is
            // exactly what the watch draws on its own weather screen as
            // "Synk." - 0x6acbc585 there read as 10/11/2026 on the watch,
            // which is 11 October 2026 in month/day order.
            QString lastTime = scalarText(ok, body, error);
            uint16_t typeCode = 0;
            const uint8_t *value = nullptr;
            size_t length = 0;
            if (ok && scalarPayload(body, &typeCode, &value, &length) && length >= 4) {
                const quint32 seconds = static_cast<quint32>(value[0])
                        | (static_cast<quint32>(value[1]) << 8)
                        | (static_cast<quint32>(value[2]) << 16)
                        | (static_cast<quint32>(value[3]) << 24);
                if (seconds > 1500000000u) {
                    lastTime = QDateTime::fromMSecsSinceEpoch(
                            static_cast<qint64>(seconds) * 1000)
                            .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                }
            }
            m_whiteboardClient->readValue(QStringLiteral("/Weather/Info"),
                    [this, lastTime](bool infoOk, const std::vector<uint8_t> &infoBody,
                                       const QString &infoError) {
                // The structure is not decoded, but its tail is plain
                // text - a country code and a place name ("FI",
                // "Vehmainen") - and two 32-bit floats earlier in it are
                // the latitude and longitude, measured against a known
                // location. The place is what a person wants to see here;
                // the rest stays hex.
                QString info = infoError;
                if (infoOk && infoBody.size() > 8) {
                    const QByteArray raw(
                            reinterpret_cast<const char *>(infoBody.data()) + 8,
                            static_cast<int>(infoBody.size()) - 8);
                    QStringList words;
                    QString word;
                    for (char c : raw) {
                        if (c >= 0x20 && c < 0x7f) {
                            word.append(QLatin1Char(c));
                            continue;
                        }
                        if (word.size() >= 2)
                            words.append(word);
                        word.clear();
                    }
                    if (word.size() >= 2)
                        words.append(word);
                    info = words.isEmpty() ? QString::fromLatin1(raw.toHex())
                                            : words.join(QLatin1Char(' '));
                }
                m_whiteboardClient->readValue(QStringLiteral("/Weather/Current"),
                        [this, lastTime, info](bool currentOk,
                                                 const std::vector<uint8_t> &currentBody,
                                                 const QString &currentError) {
                    const QString current = currentOk && currentBody.size() > 8
                            ? QString::fromLatin1(
                                    QByteArray(reinterpret_cast<const char *>(currentBody.data()) + 8,
                                                static_cast<int>(currentBody.size()) - 8).toHex())
                            : currentError;
                    finishWifi(tr("Last sync: %1. Info: %2. Current: %3.")
                                        .arg(lastTime, info, current));
                });
            });
        });
    });
}

void AppController::readWatchEventLog()
{
    if (m_wifiBusy)
        return;

    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();

        // Not a stream: measured in btsnoop_race_sync_2026-09-25.log, the
        // official app hands /Analytics/Data a **filename** and then reads
        // that file off the filesystem. The first attempt here used the
        // bulk-stream trigger and timed out, which is the same lesson
        // /Logbook/Entries taught - the trigger is per resource and the
        // capture is what says which.
        m_whiteboardClient->fetchRenderedFile(QStringLiteral("/Analytics/Data"),
                                               QStringLiteral("sysevt.sbm"),
                [this](bool ok, const std::vector<uint8_t> &data, const QString &error) {
            if (!ok) {
                finishWifi(tr("Could not read the watch's event log: %1").arg(error));
                return;
            }

            QByteArray bytes(reinterpret_cast<const char *>(data.data()),
                              static_cast<int>(data.size()));

            // The whole file goes to disk as well as the summary to the
            // screen. A filtered tail of twelve lines with no timestamps
            // is enough to see that something is wrong and not enough to
            // say what - and this file is cheap to keep.
            QString saved;
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                    + QStringLiteral("/SuuntoSync");
            if (QDir().mkpath(dir)) {
                const QString out = QStringLiteral("%1/watch-eventlog_%2.sbm")
                        .arg(dir, QDateTime::currentDateTime()
                                    .toString(QStringLiteral("yyyy-MM-dd_HHmm")));
                QFile file(out);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(bytes);
                    file.close();
                    saved = out;
                }
            }

            // The log is an uncompressed SBEM0102 container - measured, on
            // a 444-byte read - so it parses with the same reader a
            // workout's payload uses. Its chunks, in the order they
            // arrive:
            //
            //   0x01 source   0x02 when the file was written
            //   0x03 firmware 0x04 serial  0x06 the watch's own city
            //   then per event: 0x07 index, 0x08 type, 0x09 time,
            //   0x0a message, 0x0b and 0x0c two tags
            //
            // **The times are microseconds since the epoch**, not the
            // local64 milliseconds a workout carries: a header stamp
            // decoded to the exact second the read was made.
            std::vector<uint8_t> container = data;
            if (Sbem::parseContainer(container).empty()) {
                // Not a container as it stands; a compressed one would be,
                // once inflated.
                const std::vector<uint8_t> inflated = Sbem::heatshrinkDecompress(data);
                if (!Sbem::parseContainer(inflated).empty())
                    container = inflated;
            }
            const std::vector<Sbem::Chunk> chunks = Sbem::parseContainer(container);

            QString city;
            QString firmware;
            QStringList events;
            QString when;
            QString message;
            QStringList tags;
            auto flush = [&events, &when, &message, &tags]() {
                if (message.isEmpty())
                    return;
                events.append(tags.isEmpty()
                        ? QStringLiteral("%1 %2").arg(when, message)
                        : QStringLiteral("%1 %2 [%3]")
                                .arg(when, message, tags.join(QLatin1Char('/'))));
                message.clear();
                tags.clear();
                when.clear();
            };

            for (const Sbem::Chunk &chunk : chunks) {
                const QByteArray value(reinterpret_cast<const char *>(chunk.value.data()),
                                        static_cast<int>(chunk.value.size()));
                const QString text = QString::fromLatin1(
                        value.left(value.indexOf('\0') < 0 ? value.size()
                                                            : value.indexOf('\0')));
                switch (chunk.id) {
                case 0x03: firmware = text; break;
                case 0x06: city = text; break;
                case 0x07: flush(); break;
                case 0x09:
                    if (chunk.value.size() >= 8) {
                        uint64_t microseconds = 0;
                        for (int i = 7; i >= 0; --i)
                            microseconds = (microseconds << 8) | chunk.value[i];
                        when = QDateTime::fromMSecsSinceEpoch(
                                static_cast<qint64>(microseconds / 1000))
                                .toString(QStringLiteral("HH:mm:ss"));
                    }
                    break;
                case 0x0a: message = text; break;
                case 0x0b:
                case 0x0c: if (!text.isEmpty()) tags.append(text); break;
                default: break;
                }
            }
            flush();

            if (events.isEmpty()) {
                finishWifi(tr("The watch's log parsed to no events (%1 bytes, %2 chunks). "
                               "Saved: %3")
                                    .arg(container.size()).arg(chunks.size()).arg(saved));
                return;
            }

            // **Reading the log empties it**: a second read right after the
            // first came back with only the events in between. So this says
            // how many there were, and the file on disk is the copy that
            // keeps them.
            const int keep = 8;
            const QStringList tail = events.mid(qMax(0, events.size() - keep));
            finishWifi(tr("Watch log: firmware %1, city %2, %3 events (last %4 shown; the "
                           "read empties the log, full copy in %5): %6")
                                .arg(firmware, city).arg(events.size()).arg(tail.size())
                                .arg(saved, tail.join(QStringLiteral(" | "))));
        });
    });
}

void AppController::readWatchWifiState()
{
    if (m_wifiBusy)
        return;

    withWatchSession([this](const WatchSession &session) {
        m_wifiSession = session;
        m_wifiBusy = true;
        emit wifiBusyChanged();

        // Three reads, each reported as the watch answered it: whether
        // WiFi is on, which slots hold a network, and the last connection
        // result. Hex, because none of the three layouts is decoded yet.
        m_whiteboardClient->readValue(wifiEnablePath(),
                [this](bool, const std::vector<uint8_t> &enabled, const QString &enabledError) {
            const QString enabledText = enabled.empty()
                    ? enabledError
                    : QString::fromLatin1(
                            QByteArray(reinterpret_cast<const char *>(enabled.data()),
                                        static_cast<int>(enabled.size())).toHex());
            m_whiteboardClient->readValue(QStringLiteral("/Settings/Wifi/AccessPoints"),
                    // Not `slots`: Qt's own macro by that name turns the
                    // parameter list into something the compiler cannot
                    // parse, and the error points at the line after.
                    [this, enabledText](bool, const std::vector<uint8_t> &savedSlots,
                                          const QString &slotsError) {
                const QString slotsText = savedSlots.empty()
                        ? slotsError
                        : QString::fromLatin1(
                                QByteArray(reinterpret_cast<const char *>(savedSlots.data()),
                                            static_cast<int>(savedSlots.size())).toHex());
                m_whiteboardClient->readValue(
                        QStringLiteral("/Device/Connectivity/Wifi/Connection/Status"),
                        [this, enabledText, slotsText](bool, const std::vector<uint8_t> &status,
                                                         const QString &statusError) {
                    const QString statusText = status.empty()
                            ? statusError
                            : QString::fromLatin1(
                                    QByteArray(reinterpret_cast<const char *>(status.data()),
                                                static_cast<int>(status.size())).toHex());
                    finishWifi(tr("Enabled: %1. Saved slots: %2. Status: %3.")
                                        .arg(enabledText, slotsText, statusText));
                });
            });
        });
    });
}

void AppController::setSyncOnConnect(bool enabled)
{
    if (enabled == m_syncOnConnect)
        return;
    m_syncOnConnect = enabled;
    QSettings(settingsPath(), QSettings::IniFormat)
            .setValue(QStringLiteral("sync/onConnect"), enabled);
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

        // The same three figures the main page leads with, from the
        // workouts already loaded above. hasProgress is about having a
        // history to average over at all, not about the numbers being
        // non-zero: a genuine CTL of zero after a month off is a real
        // reading and the cover should show it rather than claim nothing
        // is synced.
        const QVariantMap progress = progressFrom(workouts);
        out.insert(QStringLiteral("hasProgress"), progress[QStringLiteral("days")].toInt() > 0);
        out.insert(QStringLiteral("ctl"), progress[QStringLiteral("ctl")]);
        out.insert(QStringLiteral("atl"), progress[QStringLiteral("atl")]);
        out.insert(QStringLiteral("tsb"), progress[QStringLiteral("tsb")]);
    } else {
        out.insert(QStringLiteral("hasWorkout"), false);
        out.insert(QStringLiteral("hasProgress"), false);
    }

    // Today's steps and calories, for the cover's own line. The day's
    // samples are 144 ten-minute buckets at most, and dailyHealth() is the
    // one place that knows where a local day starts - so the cover reads
    // the same figures the health page's own list does rather than summing
    // them a second way.
    const QVariantList today = dailyHealth(1);
    if (!today.isEmpty()) {
        const QVariantMap day = today.first().toMap();
        out.insert(QStringLiteral("hasSteps"), day.value(QStringLiteral("hasActivity")).toBool());
        out.insert(QStringLiteral("steps"), day.value(QStringLiteral("steps")));
        out.insert(QStringLiteral("energyKcal"), day.value(QStringLiteral("energyKcal")));
    } else {
        out.insert(QStringLiteral("hasSteps"), false);
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

QVariantList AppController::dailyHealth(int days) const
{
    if (days <= 0)
        return QVariantList();

    // The window starts at midnight local time `days - 1` days ago, so
    // "30 days" means thirty calendar days including today rather than
    // thirty times 24 hours from now.
    const QDate firstDay = QDate::currentDate().addDays(-(days - 1));
    const qint64 sinceMs = QDateTime(firstDay, QTime(0, 0)).toMSecsSinceEpoch();

    struct Day {
        double steps = 0;
        double joules = 0;
        double sleepSeconds = 0;
        double sleepQuality = -1;
        bool hasActivity = false;
        bool hasSleep = false;
    };
    QVector<Day> daily(days);

    const auto indexOf = [firstDay, days](qint64 ms) {
        const int i = firstDay.daysTo(QDateTime::fromMSecsSinceEpoch(ms).date());
        return (i >= 0 && i < days) ? i : -1;
    };

    for (const HealthEntry &entry : m_healthStore->loadSince(QStringLiteral("activity"),
                                                              sinceMs, nullptr)) {
        const int i = indexOf(entry.timestamp);
        if (i < 0)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(entry.data).object();
        daily[i].steps += o.value(QStringLiteral("stepCount")).toDouble();
        // Joules, as everywhere in this protocol - the kcal conversion is
        // one place, here, rather than in every page that shows it.
        daily[i].joules += o.value(QStringLiteral("energyConsumption")).toDouble();
        daily[i].hasActivity = true;
    }

    // A night is one entry, and it belongs to the day its timestamp falls
    // on - the same convention the page's "earlier nights" list already
    // uses. A second entry for the same day (a nap) adds its duration and
    // keeps the better quality rather than overwriting.
    for (const HealthEntry &entry : m_healthStore->loadSince(QStringLiteral("sleep"),
                                                              sinceMs, nullptr)) {
        const int i = indexOf(entry.timestamp);
        if (i < 0)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(entry.data).object();
        daily[i].sleepSeconds += o.value(QStringLiteral("duration")).toDouble();
        if (o.contains(QStringLiteral("quality"))) {
            daily[i].sleepQuality = std::max(daily[i].sleepQuality,
                                              o.value(QStringLiteral("quality")).toDouble());
        }
        daily[i].hasSleep = true;
    }

    QVariantList out;
    for (int i = days - 1; i >= 0; --i) {
        const Day &day = daily[i];
        QVariantMap row;
        row.insert(QStringLiteral("dayMs"),
                    QDateTime(firstDay.addDays(i), QTime(0, 0)).toMSecsSinceEpoch());
        row.insert(QStringLiteral("steps"), day.steps);
        row.insert(QStringLiteral("energyKcal"), day.joules / 4184.0);
        row.insert(QStringLiteral("sleepSeconds"), day.sleepSeconds);
        // -1 rather than 0 for "the watch did not score this night": a
        // zero would draw as a bad night.
        row.insert(QStringLiteral("sleepQuality"), day.sleepQuality);
        row.insert(QStringLiteral("hasActivity"), day.hasActivity);
        row.insert(QStringLiteral("hasSleep"), day.hasSleep);
        out.append(row);
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

void AppController::importFitFile(const QString &path)
{
    // Picked by the user through a FilePickerPage, so it arrives as a
    // file:// URL as often as a path.
    const QString local = path.startsWith(QStringLiteral("file://"))
            ? QUrl(path).toLocalFile()
            : path;

    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("Could not open %1: %2")
                                    .arg(QFileInfo(local).fileName(), file.errorString()));
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    std::vector<uint8_t> raw(reinterpret_cast<const uint8_t *>(bytes.constData()),
                              reinterpret_cast<const uint8_t *>(bytes.constData())
                                      + bytes.size());

    Fit::Activity activity;
    std::string decodeError;
    if (!Fit::parseActivity(raw, activity, decodeError)) {
        emit errorOccurred(tr("%1 is not a readable FIT activity: %2")
                                    .arg(QFileInfo(local).fileName(),
                                          QString::fromStdString(decodeError)));
        return;
    }
    if (activity.summary.startTime == 0) {
        // Without a start time there is no key, and a key made from the
        // import's own clock would make the same file a new workout every
        // time it was imported.
        emit errorOccurred(tr("%1 carries no start time, so it cannot be filed.")
                                    .arg(QFileInfo(local).fileName()));
        return;
    }

    const Workout w = workoutFromFit(activity);
    QString storeError;
    if (!m_workoutStore->upsert(w, &storeError)) {
        emit errorOccurred(tr("Could not save the imported workout: %1").arg(storeError));
        return;
    }

    // The route, in the same packed form the watch's own track is stored
    // in, so the map item and the detail page cannot tell the difference.
    std::vector<Logbook::TrackPoint> track;
    for (const Fit::Sample &sample : activity.samples) {
        if (!sample.hasPosition())
            continue;
        track.push_back({ sample.lat, sample.lon });
    }
    if (!track.empty())
        m_workoutStore->saveRoute(w.key, packTrack(track), nullptr);

    const QByteArray series = fitSeriesJson(activity);
    if (!series.isEmpty())
        m_workoutStore->saveSeries(w.key, series, nullptr);
    const QByteArray laps = fitLapsJson(activity);
    if (!laps.isEmpty())
        m_workoutStore->saveLaps(w.key, laps, nullptr);

    // The file itself goes in the same column a watch workout's /Data
    // payload does, and that is what puts an import in the upload queue:
    // keysPendingUpload() asks for rows that have `data` and no cloud key,
    // so it appears in "send N workouts to the cloud" beside the others
    // with nothing else to wire. buildUploadZip() tells the two apart by
    // the key's own prefix.
    m_workoutStore->saveSmlSources(w.key, bytes, QByteArray(), nullptr);

    loadCachedWorkouts();
    emit errorOccurred(tr("Imported %1: %2 km, %3 samples")
                                .arg(QFileInfo(local).fileName())
                                .arg(w.totalDistance / 1000.0, 0, 'f', 2)
                                .arg(activity.samples.size()));
}

bool AppController::canExportFit(const QString &key) const
{
    const QString source = sourceOfWorkout(key);
    // An import keeps the file itself. A cloud workout has one up there -
    // or so one endpoint in one third-party client says; the attempt is
    // offered because its failure is readable, and a watch workout is not
    // offered at all because writing FIT needs an encoder this project
    // does not have.
    if (source == QStringLiteral("cloud"))
        return m_cloudAccount.isSignedIn();
    QByteArray data;
    // A watch workout is written from its stored payload now - see
    // fitFromWatchWorkout() - so the only thing that cannot be exported
    // as FIT is a cloud workout nobody is signed in for.
    return (source == QStringLiteral("fit") || source == QStringLiteral("ble"))
            && m_workoutStore->loadSmlSources(key, &data, nullptr) && !data.isEmpty();
}

bool AppController::canExportSml(const QString &key) const
{
    QByteArray data;
    return m_workoutStore->loadSmlSources(key, &data, nullptr) && !data.isEmpty();
}

void AppController::exportWorkoutFit(const QString &key, const QString &label)
{
    const QString source = sourceOfWorkout(key);
    QByteArray data;
    const bool haveSources =
            m_workoutStore->loadSmlSources(key, &data, nullptr) && !data.isEmpty();

    // An import already is a FIT file; nothing to write but the bytes.
    if (haveSources && source == QStringLiteral("fit")) {
        writeExport(key, label, QStringLiteral("fit"), data);
        return;
    }

    // A watch workout is written from the payload the watch sent.
    if (haveSources && source == QStringLiteral("ble")) {
        const QByteArray encoded = fitFromWatchWorkout(key, data);
        if (encoded.isEmpty()) {
            emit errorOccurred(tr("This workout's stored data decoded to nothing, so there "
                                   "is no FIT file to write."));
            return;
        }
        writeExport(key, label, QStringLiteral("fit"), encoded);
        return;
    }

    if (source != QStringLiteral("cloud")) {
        emit errorOccurred(tr("There is no FIT file stored for this workout."));
        return;
    }
    if (m_fitExportInProgress)
        return;
    m_fitExportInProgress = true;

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key, label](bool ok, const QByteArray &secret, const QString &vaultError) {
        if (!ok) {
            m_fitExportInProgress = false;
            emit errorOccurred(tr("Could not read the stored session: %1").arg(vaultError));
            return;
        }
        m_cloudClient->downloadWorkoutFit(QString::fromUtf8(secret), key,
                [this, key, label](bool fitOk, const QByteArray &body, const QString &error) {
            m_fitExportInProgress = false;
            if (!fitOk) {
                emit errorOccurred(tr("The cloud would not export this as FIT: %1").arg(error));
                return;
            }
            // Decoded before it is called a success. The header check in
            // the client says it is shaped like a FIT file; this says it is
            // an activity with records in it, which is the thing that was
            // actually wanted.
            const std::vector<uint8_t> raw(
                    reinterpret_cast<const uint8_t *>(body.constData()),
                    reinterpret_cast<const uint8_t *>(body.constData()) + body.size());
            Fit::Activity activity;
            std::string decodeError;
            if (!Fit::parseActivity(raw, activity, decodeError)) {
                emit errorOccurred(tr("The cloud sent %1 bytes that are not a readable "
                                       "activity: %2")
                                            .arg(body.size())
                                            .arg(QString::fromStdString(decodeError)));
                return;
            }
            writeExport(key, label, QStringLiteral("fit"), body);
            emit errorOccurred(tr("The cloud's own FIT: %1 samples, %2 laps, %3 km")
                                        .arg(activity.samples.size())
                                        .arg(activity.laps.size())
                                        .arg(activity.summary.distance / 1000.0, 0, 'f', 2));
        });
    });
}

void AppController::exportWorkoutSml(const QString &key, const QString &label)
{
    // Built, not stored - the same call the upload makes, so the file on
    // disk is what would go to the cloud.
    const QByteArray zip = buildUploadZip(key);
    if (zip.isEmpty()) {
        emit errorOccurred(tr("Nothing to export: this workout has no stored sources."));
        return;
    }
    writeExport(key, label, QStringLiteral("sml.zip"), zip);
}

void AppController::writeExport(const QString &key, const QString &label,
                                 const QString &extension, const QByteArray &contents)
{
    // Beside what harbour-edgesync writes, under its own name - the two
    // apps export the same kinds of file and a shared directory would make
    // it ambiguous which wrote what.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/SuuntoSync");
    if (!QDir().mkpath(dir)) {
        emit errorOccurred(tr("Could not create %1").arg(dir));
        return;
    }

    // The activity and when it was, which is how a file is found again; the
    // key goes on the end because two rides can start in the same minute
    // and a silent overwrite is worse than a long name.
    qint64 startTime = 0;
    for (const Workout &w : m_workoutStore->loadAll(nullptr)) {
        if (w.key == key) {
            startTime = w.startTime;
            break;
        }
    }
    QString stem = label.isEmpty() ? tr("Workout") : label;
    stem.replace(QRegExp(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("-"));
    const QString when = startTime != 0
            ? QDateTime::fromMSecsSinceEpoch(startTime).toString(QStringLiteral("yyyy-MM-dd_HHmm"))
            : QStringLiteral("undated");
    const QString path = QStringLiteral("%1/%2_%3_%4.%5")
            .arg(dir, stem, when, key, extension);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        emit errorOccurred(tr("Could not write %1: %2").arg(path, out.errorString()));
        return;
    }
    const qint64 written = out.write(contents);
    out.close();
    if (written != contents.size()) {
        emit errorOccurred(tr("Only %1 of %2 bytes reached %3")
                                    .arg(written).arg(contents.size()).arg(path));
        return;
    }
    emit errorOccurred(tr("Saved %1 kB to %2").arg(contents.size() / 1024).arg(path));
}

void AppController::loadCachedWorkouts()
{
    QString error;
    const QVector<Workout> workouts = m_workoutStore->loadAll(&error);
    if (!error.isEmpty()) {
        emit errorOccurred(tr("Failed to load workouts: %1").arg(error));
        return;
    }

    // **One row per real outing.** A workout this app uploaded from the
    // watch comes back from the next cloud sync as a second row - same
    // outing, different key namespace - and the list showed both.
    //
    // The upload records the cloud's own key against the watch row
    // (workout_sml.uploaded_key), so for these there is an exact
    // cross-reference and no need to guess from timestamps. The watch row
    // is the one kept: its route, curves and laps are stored under its
    // key, and it already carries the marker saying the cloud has it.
    //
    // Only the list is filtered. trainingProgress() and monthSummary()
    // still see everything, because the cloud copy is the one carrying the
    // training stress score and collapsing by time is what those do.
    const QHash<QString, QString> uploaded = m_workoutStore->uploadedCloudKeys();
    QSet<QString> hide;
    for (const Workout &workout : workouts) {
        const QString cloudKey = uploaded.value(workout.key);
        if (!cloudKey.isEmpty())
            hide.insert(cloudKey);
    }

    // The pairs this app did not create itself - a workout the watch sent
    // to the cloud by some other route - have no such mapping, and for
    // those the only cross-reference is the start time. The window is
    // WorkoutSummary's own 90 seconds, already measured and already used
    // by the month totals and the progress figures, so the list now
    // counts a workout the same way they do.
    //
    // Deliberately one-directional: a cloud row is hidden when a local one
    // sits beside it, never the other way round. The 1179 cloud-only rows
    // here are nobody's duplicate and must stay.
    for (const Workout &workout : workouts) {
        if (workout.source == QStringLiteral("cloud"))
            continue;
        for (const Workout &other : workouts) {
            if (other.source != QStringLiteral("cloud"))
                continue;
            if (std::llabs(other.startTime - workout.startTime)
                    <= WorkoutSummary::kSameWorkoutWindowMs)
                hide.insert(other.key);
        }
    }

    QVector<Workout> shown;
    shown.reserve(workouts.size());
    for (const Workout &workout : workouts) {
        if (workout.source == QStringLiteral("cloud") && hide.contains(workout.key))
            continue;
        shown.append(workout);
    }
    m_workoutModel->setWorkouts(shown);
}

void AppController::syncCloudWorkouts()
{
    startCloudSync(false);
}

void AppController::resyncCloudWorkouts()
{
    startCloudSync(true);
}

void AppController::startCloudSync(bool full)
{
    if (!m_cloudAccount.isSignedIn() || m_workoutSyncInProgress)
        return;

    m_workoutSyncKind = QStringLiteral("cloud");
    m_workoutSyncInProgress = true;
    emit workoutSyncInProgressChanged();

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, full](bool ok, const QByteArray &data, const QString &loadError) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            m_workoutSyncSession.reset();
            emit workoutSyncInProgressChanged();
            emit errorOccurred(tr("Failed to load login session: %1").arg(loadError));
            return;
        }

        // Incremental by default: the cursor is the server's own clock
        // from the last successful sync, so a routine one asks for the
        // handful of workouts that reached the cloud since - not all
        // twelve pages of a decade's history, which is what this did
        // before the capture showed the official app doing otherwise.
        // Everything already synced stays in the local database, which is
        // where the monthly totals and the training load are computed
        // from anyway.
        const qint64 since = full ? 0 : m_cloudAccount.workoutCursor;
        fetchCloudWorkoutPage(QString::fromUtf8(data), since, 0, 0, 0);
    });
}

void AppController::fetchCloudWorkoutPage(const QString &sessionKey, qint64 sinceMs, int offset,
                                           int stored, qint64 untilMs)
{
    // One page at a time, stored as it arrives, so a long history does not
    // have to be held in memory to be saved.
    const int pageSize = 100;
    // A server that always returns a full page would otherwise loop for
    // ever. Ten thousand workouts is more than anybody has and far more
    // than a chronic load needs.
    const int maximum = 10000;

    m_cloudClient->listWorkouts(sessionKey, sinceMs, pageSize, offset,
            [this, sessionKey, sinceMs, offset, stored, untilMs, pageSize, maximum](
                    bool listOk, const QVector<Workout> &workouts,
                    const SuuntoCloudClient::WorkoutPageInfo &page,
                    const QString &listError) {
        if (!listOk) {
            m_workoutSyncInProgress = false;
            m_workoutSyncSession.reset();
            emit workoutSyncInProgressChanged();
            // Pages already stored stay stored: a history that got half way
            // is better than one that got nowhere, and the next sync starts
            // over anyway.
            loadCachedWorkouts();
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
                m_workoutSyncInProgress = false;
                m_workoutSyncSession.reset();
                emit workoutSyncInProgressChanged();
                loadCachedWorkouts();
                emit errorOccurred(tr("Failed to save workout: %1").arg(storeError));
                return;
            }
        }

        // The first page's `until` is the one that becomes the next
        // cursor, not the last page's: a workout that reaches the cloud
        // while this run is still paging would otherwise fall into the gap
        // between the two and never be asked for again. Erring early means
        // re-fetching a few, which upsert-by-key makes free.
        const qint64 until = untilMs != 0 ? untilMs : page.untilMs;

        const int now = stored + workouts.size();
        if (workouts.size() == pageSize && now < maximum) {
            fetchCloudWorkoutPage(sessionKey, sinceMs, offset + workouts.size(), now, until);
            return;
        }

        m_workoutSyncInProgress = false;
        m_workoutSyncSession.reset();
        emit workoutSyncInProgressChanged();

        // currentSecsSinceEpoch() is Qt 5.8+ - newer than Sailfish OS's
        // Qt5 (same vintage issue as QRandomGenerator elsewhere in this
        // project); currentMSecsSinceEpoch() has been available since
        // Qt 4.7 and works everywhere.
        m_cloudAccount.lastSync = QDateTime::currentMSecsSinceEpoch() / 1000;
        // Only ever moved forward, and only on a sync that finished: a
        // partial one leaves the old cursor so the next attempt covers the
        // same ground again.
        if (until > m_cloudAccount.workoutCursor)
            m_cloudAccount.workoutCursor = until;
        QString saveError;
        if (!m_cloudAccountStore->save(m_cloudAccount, &saveError))
            emit errorOccurred(tr("Failed to save account: %1").arg(saveError));

        loadCachedWorkouts();
    });
}

void AppController::loadCloudDetails(const QString &key)
{
    // Only worth a request for a cloud workout we haven't already fetched -
    // a BLE one's details come from the watch, and re-fetching on every
    // page open would be a request per tap.
    if (!m_cloudAccount.isSignedIn() || key.startsWith(QStringLiteral("ble_")))
        return;

    // Already fetched, and fetched by a build that stored the same things.
    // An older row is re-fetched rather than patched: it is one request,
    // and merging a new shape into an old one is how a field map ends up
    // half of each.
    const QByteArray stored = m_workoutStore->loadDetails(key);
    if (!stored.isEmpty()) {
        const int schema = QJsonDocument::fromJson(stored)
                                   .object()
                                   .value(QStringLiteral("_schema"))
                                   .toObject()
                                   .value(QStringLiteral("value"))
                                   .toInt();
        if (schema >= kCloudDetailsSchema)
            return;
    }

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, key](bool ok, const QByteArray &data, const QString &) {
        if (!ok)
            return;
        const QString sessionKey = QString::fromUtf8(data);
        m_cloudClient->fetchWorkoutDetail(sessionKey, key,
                [this, key, sessionKey](bool detailOk, const QJsonObject &payload,
                                         const QString &) {
            // Silent on failure: this is an enrichment, and a workout that
            // shows its summary without the extras is still useful.
            QJsonObject fields;
            if (detailOk)
                collectCloudDetailFields(payload, &fields);

            // Then the endpoint the official app itself uses, which is
            // where the fitness and intensity analysis lives - VO2max, the
            // fitness age, the heart-rate zone boundaries. Chained rather
            // than fired alongside so there is one write and one signal,
            // and so a 403 on this half still leaves the detail half
            // stored. Both are allowed to fail; only both failing means
            // nothing happens.
            m_cloudClient->fetchWorkoutExtensions(sessionKey, key,
                    [this, key, fields](bool extOk, const QJsonObject &extPayload,
                                         const QString &extError) {
                QJsonObject merged = fields;
                if (extOk) {
                    collectCloudDetailFields(extPayload, &merged);
                } else if (!extError.isEmpty()) {
                    // Not an errorOccurred: the page is already drawn and
                    // this is an extra. The log is where the nine workouts
                    // that answer 403 can be recognised.
                    qWarning() << "extensions for" << key << "failed:" << extError;
                }
                if (merged.isEmpty())
                    return;

                QJsonObject schema;
                schema.insert(QStringLiteral("value"), kCloudDetailsSchema);
                merged.insert(QStringLiteral("_schema"), schema);

                const QByteArray json = QJsonDocument(merged).toJson(QJsonDocument::Compact);
                m_workoutStore->saveDetails(key, json, nullptr);

                // Promote the ones the watch also reports, so a cloud
                // workout shows them as proper stats rather than only as
                // rows in the field table. The cloud's units match the
                // watch's here (ml/kg, 1-5, seconds), so no conversion.
                auto number = [&merged](const char *name) {
                    return merged.value(QLatin1String(name)).toObject()
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
    });
}

namespace {
// Keeps a copy of a cloud sample-data response, and returns where it went -
// or an empty string if it could not be written. One file per workout,
// overwritten, in the cache.
QString writeCloudSamplesCopy(const QString &key, const QByteArray &body)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/sml-") + key + QStringLiteral(".json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(body);
    file.close();
    return path;
}
} // namespace

void AppController::loadCloudSamples(const QString &key, bool saveRaw)
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
            [this, key, saveRaw, finish](bool ok, const QByteArray &data, const QString &error) {
        if (!ok) {
            finish(tr("Could not read the stored session: %1").arg(error));
            return;
        }
        m_cloudClient->fetchWorkoutSml(QString::fromUtf8(data), key,
                [this, key, saveRaw, finish](bool smlOk, const QByteArray &body,
                                              const QString &smlError) {
            if (!smlOk) {
                finish(tr("Could not download sample data: %1").arg(smlError));
                return;
            }

            // Asked for explicitly: keep the body regardless of whether the
            // parser understands it.
            if (saveRaw) {
                const QString path = writeCloudSamplesCopy(key, body);
                if (path.isEmpty()) {
                    finish(tr("Downloaded %1 kB but could not save it.")
                           .arg(body.size() / 1024));
                    return;
                }
                // The series still get built, so saving a copy is not a
                // different action from downloading one.
                const QByteArray saved = buildCloudSeriesJson(body);
                if (!saved.isEmpty())
                    m_workoutStore->saveSeries(key, saved, nullptr);
                const QByteArray savedLaps = buildCloudLapsJson(body);
                if (!savedLaps.isEmpty())
                    m_workoutStore->saveLaps(key, savedLaps, nullptr);
                m_cloudSamplesInProgress = false;
                emit cloudSamplesInProgressChanged();
                emit workoutDetailsChanged(key);
                emit logbookTestResult(tr("%1 kB saved to %2")
                                        .arg(body.size() / 1024).arg(path));
                return;
            }

            const QByteArray series = buildCloudSeriesJson(body);
            if (series.isEmpty()) {
                // Nothing recognised. Keep the body rather than throwing it
                // away - this is the only copy of the shape this parser was
                // written blind against. See loadCloudSamples()'s header
                // comment.
                const QString path = writeCloudSamplesCopy(key, body);
                if (!path.isEmpty()) {
                    finish(tr("No charts found in %1 kB of sample data. Raw response saved to %2.")
                           .arg(body.size() / 1024).arg(path));
                } else {
                    finish(tr("No charts found in the sample data, and it could not be saved."));
                }
                return;
            }

            m_workoutStore->saveSeries(key, series, nullptr);

            // The laps are in the same response, so there is no second
            // request and no second button: whoever downloads the curves
            // gets the laps.
            const QByteArray laps = buildCloudLapsJson(body);
            if (!laps.isEmpty())
                m_workoutStore->saveLaps(key, laps, nullptr);

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
        // Bookkeeping, not a recorded field - "_schema" above.
        if (it.key().startsWith(QLatin1Char('_')))
            continue;
        const QJsonObject entry = it.value().toObject();
        const QString text = entry.value(QStringLiteral("text")).toString();
        const double value = entry.value(QStringLiteral("value")).toDouble();
        // Skip fields the watch didn't record. Zero is the schema's own
        // "absent" marker for most of these (nillable=0), and a screen of
        // a hundred zeroes would bury the dozen that mean something. A text
        // field is exempt: it is absent by not being there at all, and
        // flattenJson() already drops the empty ones.
        if (text.isEmpty() && qFuzzyIsNull(value))
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("name"), it.key());
        row.insert(QStringLiteral("value"), value);
        row.insert(QStringLiteral("text"), text);
        row.insert(QStringLiteral("unit"), entry.value(QStringLiteral("unit")).toString());
        out.append(row);
    }
    return out;
}

QVariantMap AppController::monthSummary() const
{
    const QDate today = QDate::currentDate();
    const QDate first(today.year(), today.month(), 1);
    const qint64 fromMs = QDateTime(first, QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 toMs = QDateTime(first.addMonths(1), QTime(0, 0)).toMSecsSinceEpoch();

    std::vector<WorkoutSummary::Entry> entries;
    const QVector<Workout> workouts = m_workoutStore->loadAll(nullptr);
    entries.reserve(static_cast<size_t>(workouts.size()));
    for (const Workout &workout : workouts)
        entries.push_back({ workout.startTime, workout.totalTime, workout.trainingStressScore });

    const WorkoutSummary::Totals totals =
            WorkoutSummary::forRange(std::move(entries), fromMs, toMs);

    QVariantMap out;
    out.insert(QStringLiteral("count"), totals.count);
    out.insert(QStringLiteral("seconds"), totals.seconds);
    out.insert(QStringLiteral("month"), QLocale().monthName(first.month()));
    return out;
}

namespace {

// All-time and this-year for one record type, on one row - which is how the
// page shows them, and the only pairing the response offers: `records` and
// `thisYear` are two objects keyed by the same type names.
QVariantMap recordRow(const QString &type, const QJsonObject &allTime, const QJsonObject &year)
{
    QVariantMap row;
    row.insert(QStringLiteral("type"), type);
    // Values pass through untouched. The page knows which types are seconds
    // and which are metres, and which this project will not claim a unit
    // for - see docs/workout-upload.md.
    row.insert(QStringLiteral("value"), allTime.value(QStringLiteral("value")).toDouble());
    row.insert(QStringLiteral("date"), qint64(allTime.value(QStringLiteral("date")).toDouble()));
    row.insert(QStringLiteral("workoutKey"),
                allTime.value(QStringLiteral("workoutKey")).toString());
    row.insert(QStringLiteral("hasAllTime"), !allTime.isEmpty());

    row.insert(QStringLiteral("yearValue"), year.value(QStringLiteral("value")).toDouble());
    row.insert(QStringLiteral("yearDate"), qint64(year.value(QStringLiteral("date")).toDouble()));
    row.insert(QStringLiteral("yearWorkoutKey"),
                year.value(QStringLiteral("workoutKey")).toString());
    row.insert(QStringLiteral("hasYear"), !year.isEmpty());
    return row;
}

} // namespace

QString AppController::suuntoPlusFileName(const QVariantMap &item)
{
    // Measured from the watch's own directory: eight .wfa named by catalogue
    // id beside eight .fea named by pluginId, and one .gui also named by
    // catalogue id. The staged .zip does not survive the install.
    //
    // Only a sports app is named by pluginId. A guide has none at all - its
    // listing does not carry the field - so getting this branch wrong would
    // look for ".fea" and silently report every guide as not installed.
    const QString type = item.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("watchface"))
        return item.value(QStringLiteral("id")).toString() + QStringLiteral(".wfa");
    if (type == QStringLiteral("sequence"))
        return item.value(QStringLiteral("id")).toString() + QStringLiteral(".gui");
    return item.value(QStringLiteral("pluginId")).toString() + QStringLiteral(".fea");
}

namespace {
// The library entry with this catalogue id, or an empty map.
QVariantMap suuntoPlusEntry(const QVariantList &library, const QString &id)
{
    for (const QVariant &value : library) {
        const QVariantMap item = value.toMap();
        if (item.value(QStringLiteral("id")).toString() == id)
            return item;
    }
    return QVariantMap();
}

// The order the three kinds are shown in: watch faces, then sports apps,
// then guides. The cloud's own type names are kept as the grouping key
// rather than translated into a parallel vocabulary here - the page turns
// them into headings, and the install path branches on them too.
int suuntoPlusKindOrder(const QString &type)
{
    if (type == QStringLiteral("watchface"))
        return 0;
    if (type == QStringLiteral("sequence"))
        return 2;
    return 1;
}

void appendSuuntoPlusItems(QVariantList &library,
                            const QVector<SuuntoCloudClient::SuuntoPlusItem> &items)
{
    for (const SuuntoCloudClient::SuuntoPlusItem &item : items) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), item.id);
        entry.insert(QStringLiteral("pluginId"), item.pluginId);
        entry.insert(QStringLiteral("name"), item.name);
        entry.insert(QStringLiteral("description"), item.description);
        entry.insert(QStringLiteral("type"), item.type);
        entry.insert(QStringLiteral("version"), item.version);
        library.append(entry);
    }
}

// Which kind the watch's own file name says a plugin is, or an empty
// string for a file that is not a plugin at all.
//
// .wfa, .gui and .fea are what the library installs. **.dev is a
// development build**: build-app.js names a `"type":"device"` app
// `<id>-q.dev` and zappctl pushes it as `b:/zapp/<id>.dev`, so it never has
// a catalogue entry and the extension is the only thing that says it is a
// sports app. Measured in the suuntoplus-mahle project, whose own apps
// these are.
QString suuntoPlusKindForFile(const QString &file)
{
    if (file.endsWith(QStringLiteral(".wfa"), Qt::CaseInsensitive))
        return QStringLiteral("watchface");
    if (file.endsWith(QStringLiteral(".gui"), Qt::CaseInsensitive))
        return QStringLiteral("sequence");
    if (file.endsWith(QStringLiteral(".fea"), Qt::CaseInsensitive)
            || file.endsWith(QStringLiteral(".dev"), Qt::CaseInsensitive))
        return QStringLiteral("feature");
    return QString();
}

// Grouped by kind, and by name inside a kind. The page draws a heading
// wherever the kind changes from one row to the next, so the sort is what
// makes the three groups whole rather than scattered.
void sortSuuntoPlusLibrary(QVariantList &library)
{
    std::sort(library.begin(), library.end(),
              [](const QVariant &left, const QVariant &right) {
        const QVariantMap a = left.toMap();
        const QVariantMap b = right.toMap();
        const int ka = suuntoPlusKindOrder(a.value(QStringLiteral("type")).toString());
        const int kb = suuntoPlusKindOrder(b.value(QStringLiteral("type")).toString());
        if (ka != kb)
            return ka < kb;
        return a.value(QStringLiteral("name")).toString()
                       .localeAwareCompare(b.value(QStringLiteral("name")).toString()) < 0;
    });
}
} // namespace

void AppController::setSuuntoPlusBusy(bool busy)
{
    if (busy == m_suuntoPlusBusy)
        return;
    m_suuntoPlusBusy = busy;
    // The session rides with the flag: every exit path from a SuuntoPlus
    // operation already clears this, and a second place to remember would
    // be a second place to forget.
    if (!busy)
        m_suuntoPlusSession.reset();
    emit suuntoPlusBusyChanged();
}

QVariantList AppController::suuntoPlusItems() const
{
    QVariantList out;
    QSet<QString> claimed;
    // Which registry entries the rows above and below account for. The
    // registry names a plugin by its pluginId, a watch face and a guide by
    // their catalogue id, and a hand-pushed build by its file's stem - so
    // all of those are what a row can claim.
    QSet<QString> claimedIds;
    for (const QVariant &value : m_suuntoPlusLibrary) {
        QVariantMap entry = value.toMap();
        const QString file = suuntoPlusFileName(entry);
        const bool installed = m_suuntoPlusFiles.contains(file);
        entry.insert(QStringLiteral("installed"), installed);
        entry.insert(QStringLiteral("fileName"), file);
        entry.insert(QStringLiteral("onWatchOnly"), false);
        entry.insert(QStringLiteral("registryOnly"), false);
        if (installed) {
            claimed.insert(file);
            claimedIds.insert(entry.value(QStringLiteral("id")).toString());
            claimedIds.insert(entry.value(QStringLiteral("pluginId")).toString());
        }
        out.append(entry);
    }

    // Everything else the plugin directory holds. A plugin put there by
    // something other than this library - a development build pushed by
    // hand - has no catalogue entry, so matching the library against the
    // directory could only ever hide it: it was on the watch, taking up a
    // slot, and no page listed it or could remove it.
    //
    // Only the three plugin extensions are rows. The directory holds other
    // things, and a row for a file this app cannot reason about would
    // offer to delete something it does not understand.
    QVariantList extras;
    for (const QString &file : m_suuntoPlusFiles) {
        if (claimed.contains(file))
            continue;
        const QString type = suuntoPlusKindForFile(file);
        if (type.isEmpty())
            continue;

        QVariantMap entry;
        // No catalogue id, because there is no catalogue entry: the file
        // name is the handle, and the page removes it by that.
        entry.insert(QStringLiteral("id"), QString());
        entry.insert(QStringLiteral("pluginId"), QString());
        // The file's own stem is the only name there is. Reading a real
        // one would mean fetching the plugin back off the watch and
        // parsing it, for a label.
        entry.insert(QStringLiteral("name"), file.left(file.lastIndexOf(QLatin1Char('.'))));
        entry.insert(QStringLiteral("description"), QString());
        entry.insert(QStringLiteral("type"), type);
        entry.insert(QStringLiteral("version"), QString());
        entry.insert(QStringLiteral("installed"), true);
        entry.insert(QStringLiteral("fileName"), file);
        entry.insert(QStringLiteral("onWatchOnly"), true);
        entry.insert(QStringLiteral("registryOnly"), false);
        claimedIds.insert(entry.value(QStringLiteral("name")).toString());
        extras.append(entry);
    }

    // And what the watch's registry names that neither a library item nor a
    // file accounts for: a plugin whose file is gone. The watch still draws
    // it in its own SuuntoPlus menu, as a row with a button and no name,
    // because the name lived in the file. Removing its file is not what
    // clears it - /Plugin/Uninstall is - and these rows are the only way
    // to reach one.
    for (auto it = m_suuntoPlusRegistry.constBegin();
            it != m_suuntoPlusRegistry.constEnd(); ++it) {
        const QString id = it.key();
        if (id.isEmpty() || claimedIds.contains(id))
            continue;
        // A registry entry whose file is still there is not a leftover -
        // it is one of the rows above, or a plugin the account's library
        // has never heard of, and the file's own row already covers it.
        const QString extension =
                QString::fromStdString(PluginRegistry::extensionForKind(it.value()));
        if (!extension.isEmpty() && m_suuntoPlusFiles.contains(id + extension))
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), id);
        entry.insert(QStringLiteral("pluginId"), id);
        entry.insert(QStringLiteral("name"), id);
        entry.insert(QStringLiteral("description"), QString());
        // The registry says which kind it was, which the file cannot any
        // more: 1 is a sports app, 2 a guide, 4 a hand-pushed build and 8
        // a watch face.
        entry.insert(QStringLiteral("type"),
                      it.value() == 8 ? QStringLiteral("watchface")
                      : it.value() == 2 ? QStringLiteral("sequence")
                                         : QStringLiteral("feature"));
        entry.insert(QStringLiteral("version"), QString());
        entry.insert(QStringLiteral("installed"), false);
        entry.insert(QStringLiteral("fileName"), QString());
        entry.insert(QStringLiteral("onWatchOnly"), false);
        entry.insert(QStringLiteral("registryOnly"), true);
        extras.append(entry);
    }

    // Sorted into the same three groups as the library rows, so a
    // hand-pushed sports app appears among the sports apps rather than in
    // a section of its own. m_suuntoPlusFiles is a QSet, whose order is
    // its hash order, so this is also what makes the extras stable from
    // one read to the next.
    out.append(extras);
    sortSuuntoPlusLibrary(out);
    return out;
}

void AppController::withSuuntoPlusContext(
        const std::function<void(const QString &, const QString &, const QString &)> &then,
        const std::function<void()> &onFailure)
{
    // Whoever asked owns the busy flag, so a failure reports through
    // onFailure rather than clearing one of them from in here. The store
    // and the library page each have their own, and clearing the wrong one
    // leaves a page spinning for ever.
    const std::function<void()> failed = onFailure
            ? onFailure
            : std::function<void()>([this]() { setSuuntoPlusBusy(false); });

    m_whiteboardClient->fetchCapabilities(
            [this, then, failed](bool ok, const std::vector<uint8_t> &payload,
                                  const QString &error) {
        if (!ok) {
            failed();
            emit errorOccurred(tr("Capabilities read failed: %1").arg(error));
            return;
        }
        const std::vector<std::string> caps = Capabilities::parse(payload);
        const QString variant = variantFrom(caps);
        if (variant.isEmpty()) {
            failed();
            emit errorOccurred(tr("No feat_simvariant_ in the watch's %1 capabilities")
                                        .arg(caps.size()));
            return;
        }
        const QString joined = QString::fromStdString(Capabilities::join(caps));
        m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
                [this, then, failed, variant, joined](bool secretOk, const QByteArray &data,
                                                        const QString &secretError) {
            if (!secretOk) {
                failed();
                emit errorOccurred(tr("Could not read the session key: %1").arg(secretError));
                return;
            }
            then(QString::fromUtf8(data), variant, joined);
        });
    });
}

void AppController::refreshSuuntoPlus()
{
    if (m_suuntoPlusBusy)
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    withWatchSession([this](const WatchSession &session) {
        m_suuntoPlusSession = session;
        setSuuntoPlusBusy(true);
        withSuuntoPlusContext([this](const QString &key, const QString &variant,
                                       const QString &capabilities) {
        m_cloudClient->listSuuntoPlusFeatures(key, variant, capabilities,
                [this, key](bool ok,
                             const QVector<SuuntoCloudClient::SuuntoPlusItem> &items,
                             const QString &error) {
            if (!ok) {
                setSuuntoPlusBusy(false);
                emit errorOccurred(tr("Library listing failed: %1").arg(error));
                return;
            }
            m_suuntoPlusLibrary.clear();
            appendSuuntoPlusItems(m_suuntoPlusLibrary, items);

            // Guides are a second listing, not a type inside the first one:
            // features/items never returns a `sequence`, which is why the
            // guide this watch has was missing from the page entirely while
            // sitting in the watch's directory as a .gui. The watch keeps
            // all three kinds side by side, so they belong in one list.
            m_cloudClient->listSuuntoPlusGuides(key,
                    [this](bool guideOk,
                            const QVector<SuuntoCloudClient::SuuntoPlusItem> &guides,
                            const QString &guideError) {
                if (guideOk)
                    appendSuuntoPlusItems(m_suuntoPlusLibrary, guides);
                sortSuuntoPlusLibrary(m_suuntoPlusLibrary);
                // Worth saying, not worth abandoning the apps and watch
                // faces for: the rest of the refresh is still useful.
                if (!guideOk)
                    emit errorOccurred(tr("Guide listing failed: %1").arg(guideError));
                readWatchPluginFiles();
            });
        });
        });
    });
}

void AppController::readWatchPluginFiles()
{
    // What the watch actually has: the directory, not /Plugin/List - see
    // suuntoPlusItems().
    m_whiteboardClient->fetchInstallLocation(
            [this](bool locOk, const QString &location, const QString &locError) {
        if (!locOk) {
            setSuuntoPlusBusy(false);
            emit suuntoPlusChanged();
            emit errorOccurred(tr("Could not ask the watch where plugins go: %1")
                                        .arg(locError));
            return;
        }
        m_suuntoPlusLocation = location;
        m_whiteboardClient->listDirectory(location,
                [this](bool listOk, const std::vector<uint8_t> &body,
                        const QString &listError) {
            if (!listOk) {
                setSuuntoPlusBusy(false);
                emit suuntoPlusChanged();
                emit errorOccurred(listError);
                return;
            }
            m_suuntoPlusFiles.clear();
            // The reply is JSON somewhere inside the body; the filenames are
            // all that is wanted from it.
            const QByteArray raw(reinterpret_cast<const char *>(body.data()),
                                  static_cast<int>(body.size()));
            const int open = raw.indexOf('{');
            const int close = raw.lastIndexOf('}');
            if (open >= 0 && close > open) {
                const QJsonObject doc =
                        QJsonDocument::fromJson(raw.mid(open, close - open + 1)).object();
                for (const QJsonValue &v : doc.value(QStringLiteral("files")).toArray())
                    m_suuntoPlusFiles.insert(v.toString());
            }
            emit suuntoPlusChanged();
            readWatchPluginRegistry();
        });
    });
}

void AppController::readWatchPluginRegistry()
{
    // The third answer about what is on the watch, and the only one that
    // says what the watch's own menu will draw.
    //
    // This reads `index.db` out of the plugin directory rather than
    // /Plugin/List, because the list is incomplete twice over: it named
    // fifteen things where the directory held seventeen files, and it does
    // not name a plugin whose file has been deleted at all - while the
    // watch still draws that plugin, nameless, in its own menu. The file
    // is where those rows come from. Measured: 408 bytes, twenty entries,
    // every byte accounted for. See src/ble/pluginregistry.h.
    //
    // Worth saying rather than worth failing for: the library and the
    // directory are already in hand, and they are what the page is mostly
    // about.
    const QString path = m_suuntoPlusLocation + QStringLiteral("/index.db");
    m_whiteboardClient->readWatchFile(path,
            [this, path](bool ok, const std::vector<uint8_t> &payload, const QString &error) {
        setSuuntoPlusBusy(false);
        if (!ok) {
            emit suuntoPlusChanged();
            emit errorOccurred(tr("Could not read the watch's plugin registry (%1): %2")
                                        .arg(path, error));
            return;
        }
        m_suuntoPlusRegistry.clear();
        for (const PluginRegistry::Entry &entry : PluginRegistry::parse(payload)) {
            if (entry.id.empty())
                continue;
            m_suuntoPlusRegistry.insert(QString::fromStdString(entry.id),
                                         entry.kind);
        }
        emit suuntoPlusChanged();
    });
}

void AppController::setStoreBusy(bool busy)
{
    if (busy == m_storeBusy)
        return;
    m_storeBusy = busy;
    if (!busy)
        m_storeSession.reset();
    emit storeBusyChanged();
}

QVariantList AppController::storeCategories() const
{
    return m_storeCategories;
}

QVariantMap AppController::storeItem(const QString &id) const
{
    return m_storeItemsById.value(id).toMap();
}

QVariantList AppController::storeItems(const QString &categoryId) const
{
    QVariantList out;
    for (const QVariant &value : m_storeCategories) {
        const QVariantMap category = value.toMap();
        if (category.value(QStringLiteral("id")).toString() != categoryId)
            continue;
        // Read back as a string list, the type it went in as - a QVariant
        // round-trip through QVariantList would work and is one assumption
        // more than this needs.
        for (const QString &id :
                     category.value(QStringLiteral("itemIds")).toStringList()) {
            // A reference with no record behind it is dropped rather than
            // shown empty. The catalogue is one reply, so this would mean
            // the store contradicted itself inside a single response.
            const QVariant item = m_storeItemsById.value(id);
            if (item.isValid())
                out.append(item);
        }
        break;
    }
    return out;
}

void AppController::refreshStore()
{
    if (m_storeBusy)
        return;
    if (!m_cloudAccount.isSignedIn()) {
        emit errorOccurred(tr("Sign in to the Suunto cloud first."));
        return;
    }

    withWatchSession([this](const WatchSession &session) {
        m_storeSession = session;
        setStoreBusy(true);
        withSuuntoPlusContext([this](const QString &key, const QString &variant,
                                       const QString &capabilities) {
        m_cloudClient->fetchStoreHome(key, variant, capabilities,
                [this](bool ok, const SuuntoCloudClient::StoreHome &home,
                        const QString &error) {
            setStoreBusy(false);
            if (!ok) {
                emit errorOccurred(tr("Could not read the store: %1").arg(error));
                return;
            }
            m_storeItemsById.clear();
            for (const SuuntoCloudClient::StoreItem &item : home.items) {
                QVariantMap entry;
                entry.insert(QStringLiteral("id"), item.id);
                entry.insert(QStringLiteral("type"), item.type);
                entry.insert(QStringLiteral("title"), item.title);
                entry.insert(QStringLiteral("subTitle"), item.subTitle);
                entry.insert(QStringLiteral("description"), item.description);
                entry.insert(QStringLiteral("iconUrl"), item.iconUrl);
                entry.insert(QStringLiteral("bannerUrl"), item.bannerUrl);
                entry.insert(QStringLiteral("richText"), item.richText);
                entry.insert(QStringLiteral("owner"), item.ownerTitle);
                entry.insert(QStringLiteral("version"), item.version);
                entry.insert(QStringLiteral("avgRating"), item.avgRating);
                entry.insert(QStringLiteral("reviews"), item.reviews);
                entry.insert(QStringLiteral("inLibrary"), item.inLibrary);
                m_storeItemsById.insert(item.id, entry);
            }
            m_storeCategories.clear();
            for (const SuuntoCloudClient::StoreCategory &category : home.categories) {
                QVariantMap entry;
                entry.insert(QStringLiteral("id"), category.id);
                entry.insert(QStringLiteral("title"), category.title);
                entry.insert(QStringLiteral("count"), category.itemIds.size());
                entry.insert(QStringLiteral("itemIds"), category.itemIds);
                m_storeCategories.append(entry);
            }
            emit storeChanged();
        });
        }, [this]() { setStoreBusy(false); });
    });
}

void AppController::addStoreItem(const QString &id)
{
    changeStoreLibrary(id, true);
}

void AppController::removeStoreItem(const QString &id)
{
    changeStoreLibrary(id, false);
}

void AppController::changeStoreLibrary(const QString &id, bool add)
{
    if (m_storeBusy)
        return;
    const QVariantMap item = m_storeItemsById.value(id).toMap();
    if (item.isEmpty()) {
        emit errorOccurred(tr("%1 is not in the store listing").arg(id));
        return;
    }
    // Verbatim, never mapped: the store calls a watch face a sportsapp.
    const QString type = item.value(QStringLiteral("type")).toString();
    const QString title = item.value(QStringLiteral("title")).toString();

    setStoreBusy(true);
    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, id, type, title, add](bool ok, const QByteArray &data,
                                           const QString &error) {
        if (!ok) {
            setStoreBusy(false);
            emit errorOccurred(tr("Could not read the session key: %1").arg(error));
            return;
        }
        const QString key = QString::fromUtf8(data);
        auto done = [this, id, title, add](bool changeOk, const QString &changeError) {
            setStoreBusy(false);
            if (!changeOk) {
                emit errorOccurred(add ? tr("Could not add %1: %2").arg(title, changeError)
                                        : tr("Could not remove %1: %2").arg(title, changeError));
                return;
            }
            // The row is updated here rather than by re-reading the whole
            // catalogue: one PUT does not change the other 229 items, and
            // a refresh costs a capabilities read over BLE.
            QVariantMap updated = m_storeItemsById.value(id).toMap();
            updated.insert(QStringLiteral("inLibrary"), add);
            m_storeItemsById.insert(id, updated);
            emit storeChanged();
            emit errorOccurred(add ? tr("Added %1 to your library").arg(title)
                                    : tr("Removed %1 from your library").arg(title));
            // And re-read the library, so the SuuntoPlus page shows the
            // change without being asked. It costs a capabilities read and
            // a directory listing over BLE, which is the only way to know
            // the new item's installed state rather than assume it.
            refreshSuuntoPlus();
        };

        if (add)
            m_cloudClient->addToStoreLibrary(key, type, id, done);
        else
            m_cloudClient->removeFromStoreLibrary(key, type, id, done);
    });
}

void AppController::installSuuntoPlusItem(const QString &id)
{
    if (m_suuntoPlusBusy)
        return;

    const QVariantMap target = suuntoPlusEntry(m_suuntoPlusLibrary, id);
    if (target.isEmpty()) {
        emit errorOccurred(tr("%1 is not in the library").arg(id));
        return;
    }
    const QString name = target.value(QStringLiteral("name")).toString();

    // A guide's zip comes from its own endpoint. Asking features/plugins for
    // a guide id answers an error, not a zip.
    const bool guide = target.value(QStringLiteral("type")).toString()
            == QStringLiteral("sequence");

    // The session is held across the cloud download too, not just the write
    // to the watch. Letting go in between and taking it again would be
    // tidier for the daemon by a second or two, and it would also mean an
    // install that fails halfway through because somebody else took the
    // watch while the zip was coming down.
    withWatchSession([this, target, id, name, guide](const WatchSession &session) {
    m_suuntoPlusSession = session;
    setSuuntoPlusBusy(true);
    withSuuntoPlusContext([this, target, id, name, guide](const QString &key, const QString &,
                                                            const QString &capabilities) {
        auto downloaded =
                [this, target, name](bool dlOk, const QByteArray &zip, const QString &dlError) {
            if (!dlOk) {
                setSuuntoPlusBusy(false);
                emit errorOccurred(tr("Download of %1 failed: %2").arg(name, dlError));
                return;
            }
            // The staged name is the installed name with .zip: the watch
            // replaces it with .wfa, .fea or .gui itself.
            const QString staged = suuntoPlusFileName(target).section(QLatin1Char('.'), 0, 0)
                    + QStringLiteral(".zip");
            m_whiteboardClient->installPlugin(staged, zip, nullptr,
                    [this, name](bool instOk, const QString &instError) {
                if (!instOk) {
                    setSuuntoPlusBusy(false);
                    emit errorOccurred(tr("Install of %1 failed: %2").arg(name, instError));
                    return;
                }
                // Re-read rather than assume: the install reporting success
                // is not evidence the file is there, which this project
                // learned the long way.
                setSuuntoPlusBusy(false);
                emit errorOccurred(tr("Installed %1").arg(name));
                refreshSuuntoPlus();
            });
        };

        if (guide)
            m_cloudClient->downloadSuuntoPlusGuide(key, id, capabilities, downloaded);
        else
            m_cloudClient->downloadSuuntoPlusPlugin(key, id, capabilities, downloaded);
    });
    });
}

void AppController::removeSuuntoPlusFromLibrary(const QString &id)
{
    if (m_suuntoPlusBusy)
        return;

    const QVariantMap target = suuntoPlusEntry(m_suuntoPlusLibrary, id);
    if (target.isEmpty()) {
        emit errorOccurred(tr("%1 is not in the library").arg(id));
        return;
    }
    // The library's own word for the kind. SuuntoCloudClient knows both
    // vocabularies, so "sequence" reaches the guides path.
    const QString type = target.value(QStringLiteral("type")).toString();
    const QString name = target.value(QStringLiteral("name")).toString();

    setSuuntoPlusBusy(true);
    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this, id, type, name](bool ok, const QByteArray &data, const QString &error) {
        if (!ok) {
            setSuuntoPlusBusy(false);
            emit errorOccurred(tr("Could not read the session key: %1").arg(error));
            return;
        }
        m_cloudClient->removeFromStoreLibrary(QString::fromUtf8(data), type, id,
                [this, id, name](bool removeOk, const QString &removeError) {
            setSuuntoPlusBusy(false);
            if (!removeOk) {
                emit errorOccurred(tr("Could not remove %1: %2").arg(name, removeError));
                return;
            }
            // Dropped from the list here rather than re-read: exactly one
            // item changed and the rest of the page - including every
            // other item's installed state - is still right.
            for (int i = 0; i < m_suuntoPlusLibrary.size(); ++i) {
                if (m_suuntoPlusLibrary.at(i).toMap().value(QStringLiteral("id")).toString()
                        == id) {
                    m_suuntoPlusLibrary.removeAt(i);
                    break;
                }
            }
            if (m_storeItemsById.contains(id)) {
                QVariantMap stored = m_storeItemsById.value(id).toMap();
                stored.insert(QStringLiteral("inLibrary"), false);
                m_storeItemsById.insert(id, stored);
                emit storeChanged();
            }
            emit suuntoPlusChanged();
            emit errorOccurred(tr("Removed %1 from your library").arg(name));
        });
    });
}

void AppController::removeSuuntoPlusItem(const QString &id)
{
    if (m_suuntoPlusBusy)
        return;

    const QVariantMap target = suuntoPlusEntry(m_suuntoPlusLibrary, id);
    if (target.isEmpty() || m_suuntoPlusLocation.isEmpty()) {
        emit errorOccurred(tr("Refresh the list first."));
        return;
    }
    deleteSuuntoPlusFile(suuntoPlusFileName(target),
                          target.value(QStringLiteral("name")).toString());
}

void AppController::removeSuuntoPlusFile(const QString &fileName)
{
    if (m_suuntoPlusBusy)
        return;
    if (m_suuntoPlusLocation.isEmpty()) {
        emit errorOccurred(tr("Refresh the list first."));
        return;
    }
    // The name comes from the watch's own directory listing, and nothing
    // matches it against the library - so it is checked here instead:
    // a name, in the plugin directory, with a plugin's extension.
    if (suuntoPlusKindForFile(fileName).isEmpty() || fileName.contains(QLatin1Char('/'))) {
        emit errorOccurred(tr("%1 is not a plugin file.").arg(fileName));
        return;
    }
    if (!m_suuntoPlusFiles.contains(fileName)) {
        emit errorOccurred(tr("The watch no longer has %1. Refresh the list.").arg(fileName));
        return;
    }
    deleteSuuntoPlusFile(fileName, fileName);
}

void AppController::removeSuuntoPlusRegistryEntry(const QString &id)
{
    if (m_suuntoPlusBusy)
        return;
    if (id.isEmpty()) {
        emit errorOccurred(tr("That entry has no id to remove."));
        return;
    }

    withWatchSession([this, id](const WatchSession &session) {
        m_suuntoPlusSession = session;
        setSuuntoPlusBusy(true);
        m_whiteboardClient->uninstallPlugin(id, [this, id](bool ok, const QString &error) {
            setSuuntoPlusBusy(false);
            if (!ok) {
                emit errorOccurred(tr("Uninstalling %1 failed: %2").arg(id, error));
                return;
            }
            emit errorOccurred(tr("Uninstalled %1").arg(id));
            refreshSuuntoPlus();
        });
    });
}

void AppController::deleteSuuntoPlusFile(const QString &fileName, const QString &label)
{
    // Two steps, and the second one is the one that was missing: deleting
    // the file leaves the watch's registry naming a plugin it can no
    // longer name - a row with a button and no label in the watch's own
    // SuuntoPlus menu. /Plugin/Uninstall is what clears that.
    //
    // The uninstall goes first, because it may take the file with it; the
    // delete afterwards is therefore allowed to fail, and only a pair of
    // failures is reported as one. The id is the file's stem: the registry
    // names a sports app by its pluginId, which is exactly what the file
    // is called.
    const QString id = fileName.left(fileName.lastIndexOf(QLatin1Char('.')));

    withWatchSession([this, fileName, label, id](const WatchSession &session) {
        m_suuntoPlusSession = session;
        setSuuntoPlusBusy(true);
        m_whiteboardClient->uninstallPlugin(id,
                [this, fileName, label](bool uninstallOk, const QString &uninstallError) {
            const QString path = m_suuntoPlusLocation + QLatin1Char('/') + fileName;
            m_whiteboardClient->deleteFile(path,
                    [this, label, uninstallOk, uninstallError]
                    (bool ok, const QString &error) {
                setSuuntoPlusBusy(false);
                if (!ok && !uninstallOk) {
                    emit errorOccurred(tr("Could not remove %1. Uninstall: %2. Delete: %3")
                                                .arg(label, uninstallError, error));
                    return;
                }
                emit errorOccurred(tr("Removed %1 from the watch").arg(label));
                refreshSuuntoPlus();
            });
        });
    });
}

QVariantList AppController::personalRecords() const
{
    const QJsonArray groups =
            QJsonDocument::fromJson(m_workoutStore->loadCloudCache(kRecordsCacheKind)).array();

    QVariantList out;
    for (const QJsonValue &groupValue : groups) {
        const QJsonObject group = groupValue.toObject();
        const QJsonObject records = group.value(QStringLiteral("records")).toObject();
        const QJsonObject year = group.value(QStringLiteral("thisYear")).toObject();

        // `types` is the server's own order for this activity, and the only
        // ordering information in the response - `records` is a JSON object,
        // whose keys Qt hands back alphabetically, which would put the
        // marathon before the 5 km.
        QVariantList rows;
        for (const QJsonValue &typeValue : group.value(QStringLiteral("types")).toArray()) {
            const QString type = typeValue.toString();
            if (!records.contains(type) && !year.contains(type))
                continue;
            rows.append(recordRow(type, records.value(type).toObject(),
                                   year.value(type).toObject()));
        }
        // An activity the account has a `types` list for but no record in -
        // the response carries several - would otherwise be a heading with
        // nothing under it.
        if (rows.isEmpty())
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("activityId"),
                      group.value(QStringLiteral("activityId")).toInt());
        entry.insert(QStringLiteral("rows"), rows);
        out.append(entry);
    }
    return out;
}

qint64 AppController::recordsFetchedAt() const
{
    qint64 fetchedAt = 0;
    m_workoutStore->loadCloudCache(kRecordsCacheKind, &fetchedAt);
    return fetchedAt;
}

void AppController::syncPersonalRecords()
{
    if (!m_cloudAccount.isSignedIn() || m_recordsSyncInProgress)
        return;

    m_recordsSyncInProgress = true;
    emit recordsSyncInProgressChanged();

    m_tokenVault->loadSecret(CloudAccountStore::TokenSecretName,
            [this](bool ok, const QByteArray &data, const QString &loadError) {
        if (!ok) {
            m_recordsSyncInProgress = false;
            emit recordsSyncInProgressChanged();
            emit errorOccurred(tr("Failed to load login session: %1").arg(loadError));
            return;
        }

        // The phone's own zone, which is what decides where this year
        // starts. QTimeZone gives the IANA name the endpoint wants
        // ("Europe/Helsinki"); QDateTime's offset would not.
        const QString zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
        m_cloudClient->fetchPersonalRecords(QString::fromUtf8(data), zone,
                [this](bool recordsOk, const QJsonArray &payload, const QString &error) {
            m_recordsSyncInProgress = false;
            emit recordsSyncInProgressChanged();

            if (!recordsOk) {
                // Loud, unlike the extensions: this is a page the user
                // opened to see these, so an empty one needs a reason.
                emit errorOccurred(tr("Failed to fetch records: %1").arg(error));
                return;
            }

            QString saveError;
            if (!m_workoutStore->saveCloudCache(kRecordsCacheKind,
                                                 QJsonDocument(payload).toJson(
                                                         QJsonDocument::Compact),
                                                 QDateTime::currentMSecsSinceEpoch(),
                                                 &saveError)) {
                emit errorOccurred(tr("Failed to save records: %1").arg(saveError));
                return;
            }
            emit personalRecordsChanged();
        });
    });
}

QVariantMap AppController::trainingProgress() const
{
    return progressFrom(m_workoutStore->loadAll(nullptr));
}

QVariantList AppController::workoutLaps(const QString &key) const
{
    const QJsonArray laps = QJsonDocument::fromJson(m_workoutStore->loadLaps(key)).array();
    QVariantList out;
    for (const QJsonValue &value : laps) {
        // Whatever is stored, verbatim. This used to copy four named
        // fields, which silently dropped the per-lap speed, heart rate,
        // ascent and energy the moment they were written at the other end:
        // the stored JSON had them, the page asked for them, and this
        // accessor in between did not know about them. A whitelist here
        // means every new field needs an edit in three places and fails
        // quietly when it gets one in two.
        out.append(value.toObject().toVariantMap());
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

// The middle of a workout's own bounding box, in real degrees, for handing
// to a map application. Not the start point: the receiving app is centred on
// whatever it is given and cannot be told a zoom, so the centre keeps the
// whole outing in view where a start point would put it at the edge.
//
// Empty map when the workout has no stored track - a treadmill run, or a
// cloud workout whose polyline was absent - which is the page's cue not to
// offer the action at all.
// The stored track as real degrees, { latitude, longitude } per point, for
// the map item - which needs to project them itself against whatever tile
// grid it chose, so a pre-fitted 0..1 route is no use to it.
QVariantList AppController::workoutTrack(const QString &key) const
{
    const QByteArray packed = m_workoutStore->loadRoute(key);
    const int count = packed.size() / (2 * static_cast<int>(sizeof(qint32)));
    QVariantList out;
    if (count < 2)
        return out;

    out.reserve(count);
    const char *p = packed.constData();
    for (int i = 0; i < count; ++i) {
        qint32 rawLat = 0, rawLon = 0;
        std::memcpy(&rawLat, p, sizeof(qint32));
        p += sizeof(qint32);
        std::memcpy(&rawLon, p, sizeof(qint32));
        p += sizeof(qint32);
        QVariantMap point;
        point.insert(QStringLiteral("latitude"), rawLat / 1e7);
        point.insert(QStringLiteral("longitude"), rawLon / 1e7);
        out.append(point);
    }
    return out;
}

QVariantMap AppController::workoutCenter(const QString &key) const
{
    const QByteArray packed = m_workoutStore->loadRoute(key);
    const int count = packed.size() / (2 * static_cast<int>(sizeof(qint32)));
    QVariantMap out;
    if (count < 1)
        return out;

    double minLat = 0, maxLat = 0, minLon = 0, maxLon = 0;
    const char *p = packed.constData();
    for (int i = 0; i < count; ++i) {
        qint32 rawLat = 0, rawLon = 0;
        std::memcpy(&rawLat, p, sizeof(qint32));
        p += sizeof(qint32);
        std::memcpy(&rawLon, p, sizeof(qint32));
        p += sizeof(qint32);
        const double lat = rawLat / 1e7;
        const double lon = rawLon / 1e7;
        if (i == 0) {
            minLat = maxLat = lat;
            minLon = maxLon = lon;
        } else {
            minLat = std::min(minLat, lat);
            maxLat = std::max(maxLat, lat);
            minLon = std::min(minLon, lon);
            maxLon = std::max(maxLon, lon);
        }
    }

    out.insert(QStringLiteral("latitude"), (minLat + maxLat) / 2.0);
    out.insert(QStringLiteral("longitude"), (minLon + maxLon) / 2.0);
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

    // Web Mercator, the projection the tiles use. This was equirectangular
    // with a cos(latitude) correction, which looks the same for a five
    // kilometre route and is not the same - and once a base map is drawn
    // underneath, a route projected any other way does not sit on its own
    // roads. One projection for both, in Mercator, is the only version of
    // this that can be right.
    QVector<Mercator::Global> projected;
    projected.reserve(count);
    double minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (int i = 0; i < count; ++i) {
        const Mercator::Global g = Mercator::fromLatLon(lats[i], lons[i]);
        projected.append(g);
        if (i == 0) {
            minX = maxX = g.x;
            minY = maxY = g.y;
        } else {
            minX = std::min(minX, g.x);
            maxX = std::max(maxX, g.x);
            minY = std::min(minY, g.y);
            maxY = std::max(maxY, g.y);
        }
    }

    const double spanX = maxX - minX;
    const double spanY = maxY - minY;
    const double span = std::max(spanX, spanY);
    if (span <= 0)
        return points;

    // Centre the smaller axis so the shape keeps its proportions.
    const double offsetX = (span - spanX) / 2.0;
    const double offsetY = (span - spanY) / 2.0;

    for (int i = 0; i < count; ++i) {
        QVariantMap point;
        point.insert(QStringLiteral("x"), (projected[i].x - minX + offsetX) / span);
        // No inversion: Mercator's y already increases southwards, which is
        // the direction a canvas's y grows, so north comes out up by itself.
        point.insert(QStringLiteral("y"), (projected[i].y - minY + offsetY) / span);
        points.append(point);
    }
    return points;
}

void AppController::syncWatchWorkouts()
{
    if (m_workoutSyncInProgress || m_logbookTestInFlight)
        return;

    withWatchSession([this](const WatchSession &session) {
        m_workoutSyncSession = session;
        m_workoutSyncKind = QStringLiteral("watch");
        m_workoutSyncInProgress = true;
        emit workoutSyncInProgressChanged();
        startWatchWorkoutSync();
    });
}

void AppController::startWatchWorkoutSync()
{
    // **Who is this watch.** One read, logged, before anything else: a
    // forum report of a failing sync on a Suunto 5 could not be acted on
    // because nothing in the journal said which watch it was or what
    // firmware it ran. The official app reads /Info at the same point in
    // its own sync, so the resource exists on these models; its payload
    // shape is not decoded here, so the printable runs are logged as they
    // arrive. Failure is ignored - this identifies, it does not gate.
    m_whiteboardClient->readValue(QStringLiteral("/Info"),
            [this](bool ok, const std::vector<uint8_t> &body, const QString &error) {
        if (!ok) {
            qWarning().noquote() << QStringLiteral("watch /Info unavailable: %1").arg(error);
        } else {
            const QByteArray raw(reinterpret_cast<const char *>(body.data()),
                                  static_cast<int>(body.size()));
            QStringList words;
            QString word;
            // **Identifiers are dropped on the way to the log.** A real
            // read of this resource answers brand, model, variant,
            // firmware - and the serial, the BLE MAC and the WiFi MAC. The
            // first four are what a bug report needs; the rest identify
            // the person, and this line exists to be pasted into a public
            // forum thread. So anything that is a long run of hex is left
            // out: that covers the serial, both MACs and the internal id,
            // and keeps "Suunto", "Race", "Tianjin" and "2.53.42".
            const auto isIdentifier = [](const QString &candidate) {
                if (candidate.size() < 10)
                    return false;
                for (QChar c : candidate) {
                    if (!isxdigit(c.toLatin1()))
                        return false;
                }
                return true;
            };
            for (char c : raw) {
                if (c >= 0x20 && c < 0x7f) {
                    word.append(QLatin1Char(c));
                    continue;
                }
                if (word.size() >= 3 && !isIdentifier(word))
                    words.append(word);
                word.clear();
            }
            if (word.size() >= 3 && !isIdentifier(word))
                words.append(word);
            // No hex fallback either: the raw bytes carry the same
            // identifiers the filter just removed.
            qWarning().noquote() << QStringLiteral("watch /Info: %1").arg(
                    words.isEmpty() ? tr("%1 bytes, nothing printable").arg(raw.size())
                                     : words.join(QStringLiteral(" | ")));
        }
        startWatchWorkoutSyncAfterInfo();
    });
}

void AppController::startWatchWorkoutSyncAfterInfo()
{
    m_whiteboardClient->fetchLogEntries(QStringLiteral("/Logbook/Entries"),
            [this](bool ok, const std::vector<LogEntries::Entry> &entries, const QString &error) {
        if (!ok) {
            m_workoutSyncInProgress = false;
            m_workoutSyncSession.reset();
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
        m_workoutSyncSession.reset();
        emit workoutSyncInProgressChanged();
        loadCachedWorkouts();
        // "ble_<id>" - the same derivation workoutFromDecoded() uses, so
        // the keys this sync stored need no second trip through the store.
        // The session is already released: sending a workout onwards is
        // network work and must not hold the watch.
        QVector<QString> syncedKeys;
        syncedKeys.reserve(logbookIds.size());
        for (const QString &logbookId : logbookIds)
            syncedKeys.append(QStringLiteral("ble_%1").arg(logbookId));
        autoUploadAfterSync(syncedKeys);
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
