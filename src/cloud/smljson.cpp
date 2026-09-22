#include "smljson.h"

#include "iso8601.h"
#include "../ble/smldecoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace SmlJson {

namespace {

// Shortest representation that reads back as the same double, which is what
// a JSON writer should emit and what the captured upload shows (an altitude
// appears as 205.4000000000001 because that is genuinely the nearest
// double, not because the app padded it).
std::string formatNumber(double v)
{
    if (!std::isfinite(v))
        return "null"; // JSON has no NaN/Infinity; "no reading" is the honest answer
    // Integral values are written without a decimal point, matching the
    // capture ("Distance": 2, not 2.0).
    if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return std::string(buf);
    }
    for (int precision = 15; precision <= 17; ++precision) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        double back = 0;
        if (std::sscanf(buf, "%lf", &back) == 1 && back == v)
            return std::string(buf);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
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
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t dot = path.find('.', start);
        const std::string part = dot == std::string::npos
                ? path.substr(start) : path.substr(start, dot - start);
        // The table spells an array two ways for the same thing:
        // "Sample.Events.Array.Lap.Type" and "Sample.Events+Array.ArrayBegin"
        // both mean Events is a list whose element holds the rest. Normalise
        // the suffix form into the segment form so one rule handles both -
        // the captured upload puts ArrayBegin and Lap in the *same* element,
        // which is what falls out of that.
        const size_t plus = part.rfind("+Array");
        if (plus != std::string::npos && plus + 6 == part.size()) {
            parts.push_back(part.substr(0, plus));
            parts.push_back("Array");
        } else {
            parts.push_back(part);
        }
        if (dot == std::string::npos)
            break;
        start = dot + 1;
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

// Places one value at its dotted path. An "Array" segment marks the parent
// as an array rather than becoming a key of its own.
void insert(Node *root, const std::string &path, const std::string &literal)
{
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty())
        return;
    Node *node = root;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "Array") {
            node->isArray = true;
            continue;
        }
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

std::string literalFor(const Sml::Reading &reading)
{
    if (reading.isText)
        return quote(reading.text);

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
    return formatNumber(reading.value);
}

} // namespace

std::string buildDocument(const std::vector<Sbem::Chunk> &chunks,
                           const std::string &source, int offsetMinutes)
{
    // One JSON sample per chunk, in payload order.
    struct Entry
    {
        Node root;
        int64_t timeMs = 0;
        bool used = false;
    };
    std::vector<Entry> entries(chunks.size());

    Sml::decode(chunks, [&entries](const Sml::Reading &reading) {
        if (reading.chunkIndex >= entries.size() || !reading.descriptor)
            return;
        const char *name = reading.descriptor->name;
        if (!name || name[0] == '\0' || isEnvelopeField(name))
            return;
        Entry &entry = entries[reading.chunkIndex];
        insert(&entry.root, name, literalFor(reading));
        entry.used = true;
        if (reading.timeMs != 0)
            entry.timeMs = reading.timeMs;
    });

    std::string out = "{\"Samples\":[";
    bool first = true;
    for (const Entry &entry : entries) {
        if (!entry.used)
            continue;
        // A sample with no clock yet can't be placed on the timeline, and
        // the cloud keys everything by TimeISO8601 - skip rather than stamp
        // it with the epoch (the failure this project already hit once with
        // the watch's own local64 clock).
        if (entry.timeMs == 0)
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"Attributes\":{\"suunto/sml\":";
        entry.root.write(&out);
        out += "},\"Source\":";
        out += quote(source);
        out += ",\"TimeISO8601\":";
        out += quote(Iso8601::formatLocal(entry.timeMs, offsetMinutes));
        out += "}";
    }
    out += "]}";

    return first ? std::string() : out;
}

} // namespace SmlJson
