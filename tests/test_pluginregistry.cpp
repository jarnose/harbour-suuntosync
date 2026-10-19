// The watch's plugin registry, against bytes shaped like the real file.
//
// The real one (408 bytes, 20 entries, off a Suunto Race on 2026-10-19) is
// not in this repository - it names the plugins on one person's watch - so
// the vectors here are built from its shape, including the two oddities it
// actually contained: an entry with a third value, and the four
// hand-pushed .dev builds that started this whole investigation.
#include "../src/ble/pluginregistry.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

int checks = 0;
void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

void appendEntry(std::vector<uint8_t> &out, const char *id,
                  const std::vector<uint32_t> &values)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(id[i]));
    for (uint32_t v : values) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    out.insert(out.end(), { 0xFF, 0xFF, 0xFF, 0xFF });
}

} // namespace

int main()
{
    std::vector<uint8_t> file;
    appendEntry(file, "zwwtch01", { 8, 0 });          // a watch face
    appendEntry(file, "zzbrnren", { 1, 0 });          // a sports app
    appendEntry(file, "zzsurfen", { 1, 0, 54 });      // and one with a third value
    appendEntry(file, "fp03pfbd", { 2, 0, 3 });       // a guide
    appendEntry(file, "mahlea01", { 4, 0 });          // hand-pushed builds
    appendEntry(file, "mahleb01", { 4, 0 });

    const std::vector<PluginRegistry::Entry> entries = PluginRegistry::parse(file);
    check(entries.size() == 6, "six entries");
    check(entries[0].id == "zwwtch01", "first id");
    check(entries[0].kind == 8, "first kind is a watch face");
    check(entries[1].kind == 1, "second kind is a sports app");
    check(entries[2].values.size() == 3, "the third value is kept");
    check(entries[2].values[2] == 54, "and its value");
    check(entries[3].kind == 2, "the guide");
    check(entries[4].id == "mahlea01" && entries[4].kind == 4, "a dev build");
    check(entries[5].id == "mahleb01", "the last entry is reached");

    // Every entry maps onto the extension the directory listing uses, which
    // is what lets a registry entry be matched against a file.
    check(PluginRegistry::extensionForKind(1) == ".fea", "kind 1 is .fea");
    check(PluginRegistry::extensionForKind(2) == ".gui", "kind 2 is .gui");
    check(PluginRegistry::extensionForKind(4) == ".dev", "kind 4 is .dev");
    check(PluginRegistry::extensionForKind(8) == ".wfa", "kind 8 is .wfa");
    check(PluginRegistry::extensionForKind(3).empty(), "an unknown kind has no extension");

    // A short read must not produce a half-invented entry.
    std::vector<uint8_t> truncated = file;
    truncated.resize(truncated.size() - 6);
    const std::vector<PluginRegistry::Entry> partial = PluginRegistry::parse(truncated);
    check(partial.size() == 5, "a truncated tail is dropped");

    // Padding comes off an id, since a shorter one would be padded rather
    // than terminated.
    std::vector<uint8_t> padded;
    appendEntry(padded, "mahle   ", { 4, 0 });
    check(PluginRegistry::parse(padded).at(0).id == "mahle", "a padded id is trimmed");

    std::printf("ok (%d checks)\n", checks);
    return 0;
}
