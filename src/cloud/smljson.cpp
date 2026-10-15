#include "smljson.h"

#include "../fit/activity.h"

#include "iso8601.h"
#include "../ble/smldecoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <map>
#include <sstream>

namespace SmlJson {

namespace {

// snprintf's %g honours the C library's *current* locale, and on a Finnish
// phone that means "1,35" - which is not JSON, and which the server
// rejected with "Workout could not be saved". Caught only on real hardware:
// the tests here ran under a C locale, where the bug is invisible.
//
// Rather than touching the process-wide locale (which would be a
// thread-unsafe side effect on a Qt app's behalf), the separator is fixed
// up afterwards. A formatted double contains no other comma - %g does not
// group thousands without the ' flag - so this is unambiguous.
void forceDecimalPoint(std::string *text)
{
    for (char &c : *text) {
        if (c == ',')
            c = '.';
    }
}

// Shortest representation that reads back as the same double, which is what
// a JSON writer should emit and what the captured upload shows (an altitude
// appears as 205.4000000000001 because that is genuinely the nearest
// double, not because the app padded it).
// `precision` is the schema's own <FRM> precision= for this field, or -1
// when it has none. It is an *output* rounding, not a storage one: the
// watch stores heart rate as whole bpm, the canonical value is 80/60 =
// 1.3333..., and a captured upload writes "HR": 1.33 because Sample.HR is
// precision=2. Sample.Altitude has no precision= at all, which is why the
// same capture carries 205.4000000000001 in full.
//
// Rounding first and then taking the shortest round-tripping form gives
// exactly the capture's output: 1.33 stays 1.33, and 2.0 at precision=1
// prints as "2" rather than "2.0".
std::string formatNumber(double v, int precision)
{
    if (!std::isfinite(v))
        return "null"; // JSON has no NaN/Infinity; "no reading" is the honest answer
    if (precision >= 0 && precision < 18) {
        const double factor = std::pow(10.0, precision);
        v = std::round(v * factor) / factor;
    }
    // Integral values are written without a decimal point, matching the
    // capture ("Distance": 2, not 2.0).
    if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return std::string(buf); // integral: no separator to worry about
    }
    for (int precision = 15; precision <= 17; ++precision) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        // sscanf reads back under the same locale that wrote it, so the
        // round-trip check stays valid before the separator is normalised.
        double back = 0;
        if (std::sscanf(buf, "%lf", &back) == 1 && back == v) {
            std::string out(buf);
            forceDecimalPoint(&out);
            return out;
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string out(buf);
    forceDecimalPoint(&out);
    return out;
}

std::string quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            } else {
                out += c;
            }
        }
    }
    out += "\"";
    return out;
}

// A node in the object being built. std::map keeps keys sorted, which is
// also the order the captured documents use.
struct Node
{
    std::map<std::string, Node> children;
    std::string literal;   // a finished JSON scalar, when this is a leaf
    bool isLeaf = false;
    bool isArray = false;  // rendered as [ { ...children... } ]

    void write(std::string *out) const
    {
        if (isLeaf) {
            *out += literal;
            return;
        }
        if (isArray)
            *out += "[";
        *out += "{";
        bool first = true;
        for (const auto &entry : children) {
            if (!first)
                *out += ",";
            first = false;
            *out += quote(entry.first);
            *out += ":";
            entry.second.write(out);
        }
        *out += "}";
        if (isArray)
            *out += "]";
    }
};

std::vector<std::string> splitPath(const std::string &path)
{
    // Two things are going on in these paths, and both were derived from the
    // captured upload rather than guessed - see docs/workout-upload.md.
    //
    // **'+' marks the segment after it as an array.** Not a plain separator:
    // reading it as one produced keys like "Windows+Window", and reading it
    // as a dot produced Windows.Window.Altitude.Avg where the cloud wants
    // Windows[].Altitude[].Avg. Both were rejected with 523.
    //
    // **An element-marker segment is dropped, and its array-ness belongs to
    // its parent.** Two of them are visible: the literal "Array", and a
    // segment that is its parent's name without a trailing 's'
    // ("Windows.Window" -> Windows[]). The captured document settles the
    // shape:
    //
    //   Windows.Window.ActivityId      -> Windows[].ActivityId
    //   Windows.Window+Altitude.Min    -> Windows[].Altitude[].Min
    //   Windows.Window.Altitude.Avg    -> Windows[].Altitude[].Avg
    //   Sample.Events+Array.ArrayBegin -> Sample.Events[].ArrayBegin
    //
    // The uncertainty worth stating: only those two markers appear in the
    // material there is, and "Events"/"Array" is not a plural pair, so the
    // rule cannot be derived from the names alone. Another watch model may
    // well carry a third.
    //
    // insert() does the work: an "Array" part sets isArray on the node just
    // descended into, so emitting "Array" in place of a marker makes the
    // parent an array, and emitting it *after* a '+'-marked segment makes
    // that segment one.
    std::vector<std::string> raw;
    std::vector<bool> afterPlus;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t sep = path.find_first_of(".+", start);
        raw.push_back(sep == std::string::npos ? path.substr(start)
                                                : path.substr(start, sep - start));
        afterPlus.push_back(start > 0 && path[start - 1] == '+');
        if (sep == std::string::npos)
            break;
        start = sep + 1;
    }

    std::vector<std::string> parts;
    for (size_t i = 0; i < raw.size(); ++i) {
        const std::string &seg = raw[i];
        const bool isMarker = seg == "Array"
                || (i > 0 && raw[i - 1].size() == seg.size() + 1
                    && raw[i - 1].compare(0, seg.size(), seg) == 0
                    && raw[i - 1].back() == 's');
        if (isMarker) {
            parts.push_back("Array");
            continue;
        }
        parts.push_back(seg);
        if (afterPlus[i])
            parts.push_back("Array");
    }
    return parts;
}

// Descriptors that are envelope fields rather than content: the payload's
// own clock (32/33) becomes the entry's TimeISO8601, and 35 would collide
// with the entry's Source. A captured upload has neither inside the
// suunto/sml object, so emitting them there would be wrong as well as
// redundant.
bool isEnvelopeField(const char *name)
{
    return std::strcmp(name, "TimeISO8601") == 0 || std::strcmp(name, "Source") == 0;
}

// Every path in a table that is an array, as the emitted JSON spells it.
//
// Array-ness has to come from the schema, not from the readings. The '+'
// marker sits on one particular member of a group - Windows.Window+Speed.Min,
// +DownhillGrade.Min - and that member can be nil, in which case no reading
// carries the marker and the group came out as a bare object. The official
// app's upload has DownhillGrade as [{...}] in every window; ours had it as
// {"Avg":0.8} in the one window where Avg was present and omitted it
// elsewhere, which is a type the cloud has no slot for.
std::set<std::string> arrayPaths(const SbemDescriptors::Table &table)
{
    std::set<std::string> paths;
    for (size_t i = 0; i < table.size(); ++i) {
        const SbemDescriptors::Descriptor *d = table.at(i);
        if (!d || !d->name || d->name[0] == '\0')
            continue;
        const std::vector<std::string> parts = splitPath(d->name);
        std::string acc;
        for (const std::string &part : parts) {
            if (part == "Array") {
                if (!acc.empty())
                    paths.insert(acc);
                continue;
            }
            acc += acc.empty() ? part : "." + part;
        }
    }
    return paths;
}

// Places one value at its dotted path. An "Array" segment marks the parent
// as an array rather than becoming a key of its own, and `arrays` marks the
// ones the schema says are arrays whether or not this particular path says
// so.
void insert(Node *root, const std::string &path, const std::string &literal,
             const std::set<std::string> &arrays)
{
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty())
        return;
    Node *node = root;
    std::string acc;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "Array") {
            node->isArray = true;
            continue;
        }
        acc += acc.empty() ? parts[i] : "." + parts[i];
        if (arrays.count(acc) != 0 && i + 1 != parts.size())
            node->children[parts[i]].isArray = true;
        if (i + 1 == parts.size()) {
            Node &leaf = node->children[parts[i]];
            leaf.isLeaf = true;
            leaf.literal = literal;
        } else {
            node = &node->children[parts[i]];
        }
    }
}

// The watch's Lap.Type enum, as names rather than numbers - the captured
// upload has {"Lap":{"Type":"Start"}}, and the first and last entries of a
// real workout confirm 0/1 against this table (which predates the capture,
// in AppController::lapTypeName).
const char *lapTypeName(double value)
{
    switch (static_cast<int>(value)) {
    case 0: return "Start";
    case 1: return "Stop";
    case 2: return "Manual";
    case 3: return "Distance";
    case 4: return "Duration";
    case 5: return "HighIntensity";
    case 6: return "LowIntensity";
    case 7: return "Interval";
    case 8: return "Pause";
    default: return nullptr;
    }
}

std::string literalFor(const Sml::Reading &reading, int offsetMinutes,
                        const SbemDescriptors::Table &table)
{
    if (reading.isText)
        return quote(reading.text);

    // A local64 field is a timestamp, and the cloud wants it as an ISO
    // string rather than a number. Sending the raw milliseconds is what
    // the server rejected with "Header DateTime is not a valid ISO
    // datetime: 1790186340390". Sml::decode has already folded the
    // timestamp's own UTC offset in (see Sbem::decodeLocal64), so it is
    // re-stamped with the document's offset, exactly as the entry's own
    // TimeISO8601 is.
    if (reading.descriptor->format == SbemDescriptors::Format::Local64) {
        return quote(Iso8601::formatLocal(static_cast<int64_t>(reading.value),
                                            offsetMinutes));
    }

    // An enum is written as its name, and the table is the authority on what
    // that name is - the watch spells out every mapping in its own <FRM>,
    // including single-valued enums whose name is a string it has no other
    // way to state (its serial number, its model, its firmware version).
    // Writing the number put `"SerialNumber": 0` into an upload where the
    // official app writes "2352D0000247".
    if (reading.descriptor->format == SbemDescriptors::Format::Enum) {
        const std::string named = table.enumName(reading.descriptor->id, reading.value);
        if (!named.empty())
            return quote(named);
        if (table.hasEnumSpec(reading.descriptor->id)) {
            // The list exists and does not cover this value, which the
            // official app writes as null rather than as the raw number -
            // Settings.EnabledNavigationSystems is enum:1=GPS,3=...,
            // has no entry for 0, and reads 0 on a watch with none
            // selected. A number there is a value the cloud has no meaning
            // for; null says "no reading", which is what it is.
            return "null";
        }
    }

    // Falls back to the hand-written lap table, which is all the compiled-in
    // descriptors have: the generated table carries formats, not value
    // names, so a watch whose own table has not been read yet still gets
    // lap types right.
    const std::string name = reading.descriptor->name;
    if (name == "Sample.Events.Array.Lap.Type"
            || name == "Sample.Events.Array.VerticalLap.Type") {
        if (const char *text = lapTypeName(reading.value))
            return quote(text);
        // An unmapped enum value is written as its number rather than an
        // invented name: wrong data is worse than unfamiliar data.
    }
    if (reading.descriptor->format == SbemDescriptors::Format::Bool)
        return reading.value != 0 ? "true" : "false";

    // Back to the schema's unit. This build keeps latitude, longitude and
    // heading in degrees because that is what the map and the UI want; the
    // cloud's format is the schema's own, which for those three is radians.
    // The captured upload of the same ride has 1.0730863393866827 where
    // this had 61.4833183 - the same position, and a value outside the
    // field's range.
    const double value = reading.value * table.canonicalFactor(reading.descriptor->id);
    return formatNumber(value, reading.descriptor->precision);
}

} // namespace

std::string buildDocument(const std::vector<Sbem::Chunk> &chunks,
                           const std::string &source, int offsetMinutes,
                           int64_t fallbackTimeMs)
{
    return buildDocument(chunks, SbemDescriptors::Table::builtin(), source, offsetMinutes,
                          fallbackTimeMs);
}

std::string buildDocument(const std::vector<Sbem::Chunk> &chunks,
                           const SbemDescriptors::Table &table,
                           const std::string &source, int offsetMinutes,
                           int64_t fallbackTimeMs)
{
    // One JSON sample per chunk, in payload order.
    struct Entry
    {
        Node root;
        int64_t timeMs = 0;
        bool used = false;
        // The whole chunk is dropped, not just the field - see the altitude
        // event below. A flag rather than an early exit because readings
        // arrive in one stream and nothing promises this one comes first.
        bool suppressed = false;
    };
    std::vector<Entry> entries(chunks.size());

    const std::set<std::string> arrays = arrayPaths(table);

    Sml::decode(chunks, table, [&entries, offsetMinutes, &table, &arrays](const Sml::Reading &reading) {
        if (reading.chunkIndex >= entries.size() || !reading.descriptor)
            return;
        const char *name = reading.descriptor->name;
        if (!name || name[0] == '\0' || isEnvelopeField(name))
            return;
        Entry &entry = entries[reading.chunkIndex];

        // The watch records an altitude calibration event - an offset pair
        // and the source it came from - and the official app does not pass
        // it on. Its upload of the same ride has exactly one entry fewer
        // than ours did, and that entry is this one; its event types are
        // Activity, ArrayBegin, Lap and Pause, with no Altitude at all.
        //
        // It is also the only thing we sent that it does not, which makes
        // it the obvious suspect for the cloud's 523 on that ride: an
        // indoor workout, which has no barometric calibration and so no
        // such event, uploaded from here without complaint.
        static const char kAltitudeEvent[] = "Sample.Events.Array.Altitude.";
        if (std::strncmp(name, kAltitudeEvent, sizeof(kAltitudeEvent) - 1) == 0) {
            entry.suppressed = true;
            return;
        }
        insert(&entry.root, name, literalFor(reading, offsetMinutes, table), arrays);
        entry.used = true;
        if (reading.timeMs != 0)
            entry.timeMs = reading.timeMs;
    });

    std::string out = "{\"Samples\":[";
    bool first = true;
    for (const Entry &entry : entries) {
        if (!entry.used || entry.suppressed)
            continue;
        // A /Summary payload carries no clock chunk at all, so every entry
        // in it has timeMs == 0. The captured upload stamps summary.json's
        // entries at the end of the workout, which is what fallbackTimeMs
        // supplies. Without a fallback there is still nothing to do but
        // skip: the cloud keys everything by TimeISO8601, and stamping the
        // epoch is the failure this project already hit once with the
        // watch's own local64 clock.
        const int64_t stamp = entry.timeMs != 0 ? entry.timeMs : fallbackTimeMs;
        if (stamp == 0)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"Attributes\":{\"suunto/sml\":";
        entry.root.write(&out);
        out += "},\"Source\":";
        out += quote(source);
        out += ",\"TimeISO8601\":";
        out += quote(Iso8601::formatLocal(stamp, offsetMinutes));
        out += "}";
    }
    out += "]}";

    return first ? std::string() : out;
}

namespace {

// Degrees to radians: the cloud's position fields are radians, which cost
// this project five rounds of a 523 once. See literalFor().
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

// One "Key":value pair, or nothing at all when the file has no reading.
// Absent and zero are different things: a rider can really be at 0 W.
void addNumber(std::string *out, bool *first, const char *key, double value, int precision)
{
    if (!std::isfinite(value))
        return;
    if (!*first)
        *out += ",";
    *first = false;
    *out += quote(key);
    *out += ":";
    *out += formatNumber(value, precision);
}

void addString(std::string *out, bool *first, const char *key, const std::string &value)
{
    if (value.empty())
        return;
    if (!*first)
        *out += ",";
    *first = false;
    *out += quote(key);
    *out += ":";
    *out += quote(value);
}

} // namespace

std::string buildFitSamples(const Fit::Activity &activity, const std::string &source,
                             int offsetMinutes)
{
    std::string out = "{\"Samples\":[";
    bool firstEntry = true;

    for (const Fit::Sample &sample : activity.samples) {
        if (sample.time == 0)
            continue; // no clock, no entry - see buildDocument()

        std::string fields;
        bool firstField = true;
        if (sample.hasPosition()) {
            addNumber(&fields, &firstField, "Latitude", sample.lat * kDegreesToRadians, 17);
            addNumber(&fields, &firstField, "Longitude", sample.lon * kDegreesToRadians, 17);
        }
        // The watch files a GPS fix's altitude separately from the
        // barometric one, and a FIT record carries a single altitude. It
        // goes in as Altitude, the barometric name, because that is the one
        // the cloud charts.
        addNumber(&fields, &firstField, "Altitude", sample.altitude, 1);
        addNumber(&fields, &firstField, "Distance", sample.distance, 1);
        addNumber(&fields, &firstField, "Speed", sample.speed, 2);
        if (!std::isnan(sample.temperature))
            addNumber(&fields, &firstField, "Temperature", sample.temperature + 273.15, 1);
        if (sample.heartRate > 0)
            addNumber(&fields, &firstField, "HR", sample.heartRate / 60.0, 3);
        if (sample.cadence >= 0)
            addNumber(&fields, &firstField, "Cadence", sample.cadence / 60.0, 3);
        if (sample.power >= 0)
            addNumber(&fields, &firstField, "Power", sample.power, 0);
        if (firstField)
            continue; // a record with a clock and nothing else

        if (!firstEntry)
            out += ",";
        firstEntry = false;
        out += "{\"Attributes\":{\"suunto/sml\":{\"Sample\":{";
        out += fields;
        out += "}}},\"Source\":";
        out += quote(source);
        out += ",\"TimeISO8601\":";
        out += quote(Iso8601::formatLocal(sample.time * 1000, offsetMinutes));
        out += "}";
    }
    out += "]}";
    return firstEntry ? std::string() : out;
}

std::string buildFitSummary(const Fit::Activity &activity, const std::string &source,
                             int offsetMinutes, int activityId)
{
    const Fit::Summary &s = activity.summary;
    if (s.startTime == 0)
        return std::string();

    const int64_t startMs = static_cast<int64_t>(s.startTime) * 1000;
    const double elapsed = std::isnan(s.elapsed) ? s.timer : s.elapsed;
    const int64_t endMs = startMs + static_cast<int64_t>((std::isnan(elapsed) ? 0.0 : elapsed)
                                                           * 1000.0);

    std::string header;
    bool first = true;
    addNumber(&header, &first, "ActivityType", activityId, 0);
    if (!first)
        header += ",";
    first = false;
    header += quote("DateTime");
    header += ":";
    header += quote(Iso8601::formatLocal(startMs, offsetMinutes));

    // Duration is the whole span and PauseDuration what the clock was
    // stopped for, the way the watch states them - the cloud subtracts one
    // from the other to get the moving time, so sending the timer as
    // Duration would hide the pauses rather than report them.
    addNumber(&header, &first, "Duration", elapsed, 3);
    if (!std::isnan(s.elapsed) && !std::isnan(s.timer) && s.elapsed > s.timer)
        addNumber(&header, &first, "PauseDuration", s.elapsed - s.timer, 3);
    addNumber(&header, &first, "Distance", s.distance, 1);
    addNumber(&header, &first, "Ascent", s.ascent, 1);
    addNumber(&header, &first, "Descent", s.descent, 1);
    // Joules. A FIT file counts kilocalories.
    if (s.calories > 0)
        addNumber(&header, &first, "Energy", s.calories * 4184.0, 1);
    if (s.avgHeartRate > 0 || s.maxHeartRate > 0) {
        if (!first)
            header += ",";
        first = false;
        header += quote("HR");
        header += ":{";
        bool firstHr = true;
        if (s.avgHeartRate > 0)
            addNumber(&header, &firstHr, "Avg", s.avgHeartRate / 60.0, 3);
        if (s.maxHeartRate > 0)
            addNumber(&header, &firstHr, "Max", s.maxHeartRate / 60.0, 3);
        header += "}";
    }
    if (!std::isnan(s.minAltitude) || !std::isnan(s.maxAltitude)) {
        if (!first)
            header += ",";
        first = false;
        header += quote("Altitude");
        header += ":{";
        bool firstAlt = true;
        addNumber(&header, &firstAlt, "Max", s.maxAltitude, 1);
        addNumber(&header, &firstAlt, "Min", s.minAltitude, 1);
        header += "}";
    }
    // Which device really recorded it. The watch puts its own name and
    // serial here; borrowing those for somebody else's ride would file the
    // ride as the watch's, so this says what the file says.
    {
        std::string device;
        bool firstDev = true;
        addString(&device, &firstDev, "Name", s.productName);
        addString(&device, &firstDev, "SerialNumber",
                   s.serial != 0 ? std::to_string(s.serial) : std::string());
        if (!firstDev) {
            if (!first)
                header += ",";
            first = false;
            header += quote("Device");
            header += ":{";
            header += device;
            header += "}";
        }
    }
    if (first)
        return std::string();

    std::string out = "{\"Samples\":[{\"Attributes\":{\"suunto/sml\":{\"Header\":{";
    out += header;
    out += "}}},\"Source\":";
    out += quote(source);
    out += ",\"TimeISO8601\":";
    // The captured upload stamps summary.json at the end of the workout.
    out += quote(Iso8601::formatLocal(endMs != startMs ? endMs : startMs, offsetMinutes));
    out += "}]}";
    return out;
}

} // namespace SmlJson
