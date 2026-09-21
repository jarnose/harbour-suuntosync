// Qt-free golden-vector test for Sbem::heatshrinkDecompress/parseContainer,
// same discipline as test_mdswirecodec.cpp/test_suuntoauth.cpp: validated
// with plain g++ against a real captured byte stream before this ever
// touches BLE/Qt code.
//
// tests/fixtures/logbook_data_heatshrink.bin is the MDS-chunk-stripped,
// still-Heatshrink-compressed bytes for one real /Logbook/byId/<id>/Data
// response, captured live from Jarno's Suunto Race (see
// docs/logbook-data-format.md for the full pipeline and provenance).
//
// Build: g++ -std=c++17 -I../src/ble -o /tmp/test_sbemcontainer \
//   test_sbemcontainer.cpp ../src/ble/sbemcontainer.cpp \
//   ../src/ble/heatshrink/heatshrink_decoder.c && /tmp/test_sbemcontainer

#include "../src/ble/sbemcontainer.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    } else {
        std::printf("ok: %s\n", what);
    }
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const std::vector<uint8_t> compressed = readFile("fixtures/logbook_data_heatshrink.bin");
    check(compressed.size() == 27719, "fixture is the expected 27719 compressed bytes");

    const std::vector<uint8_t> decompressed = Sbem::heatshrinkDecompress(compressed);
    check(decompressed.size() == 83045, "decompresses to the expected 83045 bytes");

    check(decompressed.size() >= 8
        && std::string(decompressed.begin(), decompressed.begin() + 8) == "SBEM0103",
        "decompressed stream starts with the SBEM0103 magic");

    const std::vector<Sbem::Chunk> chunks = Sbem::parseContainer(decompressed);
    check(chunks.size() == 7527, "parses to the expected 7527 TLV chunks");

    // Every byte after the 8-byte magic must be consumed by some chunk -
    // i.e. the TLV walk is exact, not just "didn't crash". Cross-checked
    // independently in Python during the original capture analysis.
    size_t consumed = 8;
    for (const auto &c : chunks)
        consumed += 2 + (c.value.size() >= 0xFF ? 4 : 0) + c.value.size();
    check(consumed == decompressed.size(), "TLV walk consumes every decompressed byte exactly");

    std::map<uint8_t, int> histogram;
    for (const auto &c : chunks)
        histogram[c.id]++;
    // Golden counts from the same real capture, verified independently in
    // Python (see docs/logbook-data-format.md).
    check(histogram[0x0c] == 732, "chunk id 0x0c appears 732 times");
    check(histogram[0x0f] == 1492, "chunk id 0x0f (matches CHUNK_HEARTRATE in suunto_nautic_parser.c) appears 1492 times");
    check(histogram[0x12] == 1553, "chunk id 0x12 (matches CHUNK_PROFILE_1HZ) appears 1553 times");
    check(histogram[0x16] == 1483, "chunk id 0x16 (matches CHUNK_EXTENDED_STATUS) appears 1483 times");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
    return 1;
}
