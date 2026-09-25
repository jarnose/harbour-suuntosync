#include "sbemtable.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace SbemDescriptors {

namespace {

const char kMagic[] = "SBEM0103";
constexpr size_t kMagicSize = 8;

// Byte size per format base type, matching BSML::DecoderBase::getFormatSize()
// as decompiled from libmds.so - and matching tools/generate_sbem_tables.py,
// which is the same table for the compiled-in copy. utf8 is variable
// (NUL-terminated) and d0 is a zero-byte "unchanged" marker.
struct FormatEntry
{
    const char *name;
    Format format;
    int8_t size;
};

const FormatEntry kFormats[] = {
    { "uint8", Format::UInt8, 1 },      { "int8", Format::Int8, 1 },
    { "bool", Format::Bool, 1 },        { "enum", Format::Enum, 1 },
    { "uint16", Format::UInt16, 2 },    { "int16", Format::Int16, 2 },
    { "dint16", Format::DeltaInt16, 2 },{ "uint32", Format::UInt32, 4 },
    { "int32", Format::Int32, 4 },      { "float32", Format::Float32, 4 },
    { "float64", Format::Float64, 8 },  { "local64", Format::Local64, 8 },
    { "dint8", Format::DeltaInt8, 1 },  { "duint8", Format::DeltaUInt8, 1 },
    { "d0", Format::Unchanged, 0 },     { "utf8", Format::Utf8, -1 },
};

// Every <MOD> decode expression seen on a real watch, as value = raw *
// scale + offset. Listed literally rather than parsed, so an expression
// this build has never seen is noticed instead of being silently
// mis-scaled - which is also how the offline generator does it.
//
// Latitude, longitude and heading deliberately deviate from the schema:
// it converts them to radians because that is what the app works in, and
// degrees are what everything else here uses.
struct ModEntry
{
    const char *expression;
    double scale;
    double offset;
};

const ModEntry kMods[] = {
    { "x/1000", 1.0 / 1000, 0.0 },
    { "x/10", 1.0 / 10, 0.0 },
    { "x/5", 1.0 / 5, 0.0 },
    { "x/50", 1.0 / 50, 0.0 },
    { "x/60", 1.0 / 60, 0.0 },
    { "x/100", 1.0 / 100, 0.0 },
    { "x/(1000*128)", 1.0 / 128000, 0.0 },
    { "x/5-1000", 1.0 / 5, -1000.0 },
    { "x+85000", 1.0, 85000.0 },
    { "x*3.6", 3.6, 0.0 },
    { "PI*x/(10^7*180)", 1e-7, 0.0 },  // degrees, see above
    { "PI*x/100/180", 1.0 / 100, 0.0 }, // degrees; the 9 Baro's compass
};

// The prefixes the offline generator strips, so a name from a watch and a
// name from the compiled-in table compare equal.
const char *const kStrippedPrefixes[] = {
    "Samples.TimelineSample.Attributes.suunto/sml.",
    "Samples+TimelineSample.",
    "Samples.TimelineSample.",
};

std::string shortenPath(const std::string &path)
{
    for (const char *prefix : kStrippedPrefixes) {
        const size_t len = std::strlen(prefix);
        if (path.compare(0, len, prefix) == 0)
            return path.substr(len);
    }
    return path;
}

// The value of a <TAG> line, or an empty string. Lines are newline
// separated and a tag appears at most once.
std::string tagValue(const std::string &text, const char *tag)
{
    const std::string needle = std::string("<") + tag + ">";
    const size_t at = text.find(needle);
    if (at == std::string::npos)
        return std::string();
    const size_t from = at + needle.size();
    const size_t end = text.find('\n', from);
    return text.substr(from, (end == std::string::npos) ? std::string::npos : end - from);
}

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, (at == std::string::npos)
                                             ? std::string::npos : at - start));
        if (at == std::string::npos)
            break;
        start = at + 1;
    }
    return parts;
}

bool readChunkHeader(const std::vector<uint8_t> &payload, size_t *pos,
                      uint32_t *id, uint32_t *length)
{
    // Both the id and the length escape through 0xFF to a 32-bit value -
    // the rule decompiled from BSML::SmlStreamParser::parseChunkHeader.
    auto readField = [&payload, pos](uint32_t *out) {
        if (*pos >= payload.size())
            return false;
        uint32_t value = payload[(*pos)++];
        if (value == 0xFF) {
            if (*pos + 4 > payload.size())
                return false;
            std::memcpy(&value, payload.data() + *pos, sizeof(value));
            *pos += 4;
        }
        *out = value;
        return true;
    };
    return readField(id) && readField(length);
}

} // namespace

Table::Table(const Table &other)
    : m_entries(other.m_entries)
    , m_unscaled(other.m_unscaled)
    , m_unknownFormats(other.m_unknownFormats)
    , m_unknownMods(other.m_unknownMods)
{
    relink();
}

Table::Table(Table &&other) noexcept
    : m_entries(std::move(other.m_entries))
    , m_unscaled(std::move(other.m_unscaled))
    , m_unknownFormats(other.m_unknownFormats)
    , m_unknownMods(other.m_unknownMods)
{
    // Moving a vector keeps its elements where they are, so the pointers
    // Descriptor holds stay valid - but relink anyway rather than relying
    // on that from a distance.
    relink();
}

Table &Table::operator=(Table other)
{
    m_entries = std::move(other.m_entries);
    m_unscaled = std::move(other.m_unscaled);
    m_unknownFormats = other.m_unknownFormats;
    m_unknownMods = other.m_unknownMods;
    relink();
    return *this;
}

void Table::relink()
{
    for (Entry &entry : m_entries) {
        entry.descriptor.name = entry.name.c_str();
        entry.descriptor.children = entry.children.empty() ? nullptr : entry.children.data();
        entry.descriptor.childCount = static_cast<uint16_t>(entry.children.size());
    }
}

const Descriptor *Table::find(uint16_t id) const
{
    const auto it = std::lower_bound(m_entries.begin(), m_entries.end(), id,
            [](const Entry &entry, uint16_t wanted) { return entry.descriptor.id < wanted; });
    if (it == m_entries.end() || it->descriptor.id != id)
        return nullptr;
    return &it->descriptor;
}

bool Table::hasKnownScale(uint16_t id) const
{
    return !std::binary_search(m_unscaled.begin(), m_unscaled.end(), id);
}

Table Table::parse(const std::vector<uint8_t> &payload)
{
    Table table;
    if (payload.size() < kMagicSize
            || std::memcmp(payload.data(), kMagic, kMagicSize) != 0) {
        return table;
    }

    size_t pos = kMagicSize;
    while (pos < payload.size()) {
        uint32_t chunkId = 0;
        uint32_t length = 0;
        if (!readChunkHeader(payload, &pos, &chunkId, &length))
            break;
        if (pos + length > payload.size())
            break;

        const uint8_t *value = payload.data() + pos;
        const size_t valueSize = length;
        pos += length;

        // Only chunk id 0 defines a descriptor. Anything else in this
        // resource is not something this parser claims to understand.
        if (chunkId != 0 || valueSize < 3)
            continue;

        uint16_t descriptorId = 0;
        std::memcpy(&descriptorId, value, sizeof(descriptorId));

        const char *textStart = reinterpret_cast<const char *>(value) + 2;
        const size_t textMax = valueSize - 2;
        const void *nul = std::memchr(textStart, '\0', textMax);
        const std::string text(textStart,
                                nul ? static_cast<size_t>(static_cast<const char *>(nul) - textStart)
                                    : textMax);

        Entry entry;
        Descriptor &d = entry.descriptor;
        d.id = descriptorId;
        d.format = Format::None;
        d.size = 0;
        d.scale = 1.0;
        d.offset = 0.0;
        d.hasNil = false;
        d.nil = 0.0;
        d.deltaOf = 0;
        d.precision = -1;

        const std::string group = tagValue(text, "GRP");
        if (!group.empty()) {
            for (const std::string &child : split(group, ',')) {
                if (!child.empty())
                    entry.children.push_back(static_cast<uint16_t>(std::atoi(child.c_str())));
            }
            table.m_entries.push_back(std::move(entry));
            continue;
        }

        entry.name = shortenPath(tagValue(text, "PTH"));

        const std::string delta = tagValue(text, "DELTAREF");
        if (!delta.empty())
            d.deltaOf = static_cast<uint16_t>(std::atoi(delta.c_str()));

        const std::string format = tagValue(text, "FRM");
        if (!format.empty()) {
            const std::vector<std::string> parts = split(format, ',');
            std::string base = parts.front();
            if (base.compare(0, 5, "enum:") == 0)
                base = "enum";

            bool known = false;
            for (const FormatEntry &candidate : kFormats) {
                if (base == candidate.name) {
                    d.format = candidate.format;
                    d.size = candidate.size;
                    known = true;
                    break;
                }
            }
            if (!known) {
                // Size unknown: mark it so SbemLayout stops computing
                // offsets in whatever group contains it rather than
                // reporting offsets that are quietly wrong.
                d.format = Format::None;
                d.size = kUnknownSize;
                ++table.m_unknownFormats;
            }

            for (size_t i = 1; i < parts.size(); ++i) {
                const std::string &flag = parts[i];
                if (flag.compare(0, 9, "nillable=") == 0) {
                    d.hasNil = true;
                    d.nil = std::atof(flag.c_str() + 9);
                } else if (flag.compare(0, 10, "precision=") == 0) {
                    d.precision = static_cast<int8_t>(std::atoi(flag.c_str() + 10));
                }
            }
        }

        const std::string mod = tagValue(text, "MOD");
        if (!mod.empty()) {
            // Only the first expression; the second is the inverse.
            const std::string expression = split(mod, ',').front();
            bool known = false;
            for (const ModEntry &candidate : kMods) {
                if (expression == candidate.expression) {
                    d.scale = candidate.scale;
                    d.offset = candidate.offset;
                    known = true;
                    break;
                }
            }
            if (!known) {
                table.m_unscaled.push_back(descriptorId);
                ++table.m_unknownMods;
            }
        }

        table.m_entries.push_back(std::move(entry));
    }

    std::sort(table.m_entries.begin(), table.m_entries.end(),
              [](const Entry &a, const Entry &b) { return a.descriptor.id < b.descriptor.id; });
    std::sort(table.m_unscaled.begin(), table.m_unscaled.end());
    table.relink();
    return table;
}

const Table &Table::builtin()
{
    static const Table table = [] {
        Table built;
        size_t count = 0;
        const Descriptor *all = SbemDescriptors::all(&count);
        built.m_entries.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            Entry entry;
            entry.descriptor = all[i];
            entry.name = all[i].name ? all[i].name : "";
            entry.children.assign(all[i].children, all[i].children + all[i].childCount);
            built.m_entries.push_back(std::move(entry));
        }
        built.relink();
        return built;
    }();
    return table;
}

} // namespace SbemDescriptors
