#include "pluginregistry.h"

namespace PluginRegistry {

namespace {
constexpr size_t kIdSize = 8;
constexpr uint32_t kTerminator = 0xFFFFFFFFu;

uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t at)
{
    return static_cast<uint32_t>(bytes[at])
            | (static_cast<uint32_t>(bytes[at + 1]) << 8)
            | (static_cast<uint32_t>(bytes[at + 2]) << 16)
            | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}
} // namespace

std::vector<Entry> parse(const std::vector<uint8_t> &bytes)
{
    std::vector<Entry> entries;
    size_t at = 0;
    while (at + kIdSize + 4 <= bytes.size()) {
        Entry entry;
        // The ids measured are all exactly eight characters, but a shorter
        // one would be padded rather than terminated, so trailing spaces
        // and NULs both come off.
        size_t end = kIdSize;
        while (end > 0 && (bytes[at + end - 1] == ' ' || bytes[at + end - 1] == 0))
            --end;
        entry.id.assign(reinterpret_cast<const char *>(bytes.data() + at), end);
        at += kIdSize;

        bool terminated = false;
        while (at + 4 <= bytes.size()) {
            const uint32_t value = readLe32(bytes, at);
            at += 4;
            if (value == kTerminator) {
                terminated = true;
                break;
            }
            entry.values.push_back(value);
        }
        // An entry whose terminator never arrived is the tail of a short
        // read; nothing is invented from it.
        if (!terminated)
            break;
        if (!entry.values.empty())
            entry.kind = entry.values.front();
        entries.push_back(entry);
    }
    return entries;
}

std::string extensionForKind(uint32_t kind)
{
    switch (kind) {
    case 1: return ".fea";
    case 2: return ".gui";
    case 4: return ".dev";
    case 8: return ".wfa";
    default: return std::string();
    }
}

} // namespace PluginRegistry
