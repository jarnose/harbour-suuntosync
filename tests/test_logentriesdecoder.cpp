// Standalone, Qt-free test harness for LogEntries::decode, run with plain
// g++ (this environment has no Sailfish/Qt toolchain):
//
//   g++ -std=c++17 -I../src/ble ../src/ble/logentriesdecoder.cpp test_logentriesdecoder.cpp -o /tmp/test_logentriesdecoder && /tmp/test_logentriesdecoder
//
// The fixture below is the real TYPE=0x05 response body (reqid 0x04c5,
// frame t=383.322s) to Mds::encodeEntriesFetchTrigger(), copied verbatim
// from the same 2026-09-21 HCI snoop log every other test in this project
// uses (see wb_traffic.tsv in that session's scratchpad).
//
// Why this fixture is trusted, not just plausible: the three decoded
// ids below (1785740504, 1785760357, 1788194033) are not invented for
// this test - they are the exact same logbook ids independently observed
// as literal ASCII path segments elsewhere in the very same capture:
// "/Logbook/byId/1785740504/Data", "/Logbook/byId/1785760357/Summary"
// and "/Logbook/byId/1788194033/Summary" (each its own separate captured
// GET request, unrelated to the Entries handle-walk this fixture comes
// from). Three independent matches is strong enough confirmation that
// this decode is correct for this capture, not a coincidence - see
// docs/logbook-data-format.md's "Third decompilation pass" section.

#include "../src/ble/logentriesdecoder.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

std::vector<uint8_t> fromHex(const std::string &hex)
{
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void expectEqU(const std::string &name, unsigned long actual, unsigned long expected)
{
    if (actual == expected) {
        std::printf("PASS %s\n", name.c_str());
    } else {
        std::printf("FAIL %s: expected %lu, got %lu\n", name.c_str(), expected, actual);
        ++g_failures;
    }
}

void testDecodeRealEntriesResponse()
{
    const auto body = fromHex(
            "f02400018000c800"                                  // shared 8-byte prefix
            "0124"                                               // protocol_v9 structure header
            "56240c00030009000c00000000000000"                  // element type tag/sub-length/count/reserved
            "d83c706a5e4f706a010000008bd01b00ace3000000000000"   // entry 0
            "658a706ace98706a01000000005d02107bd1000000000000"   // entry 1
            "f1ac956a06b3956a01000b00f0ac05104175000000000000"); // entry 2

    const auto entries = LogEntries::decode(body);
    expectEqU("decode real /Entries response: count", entries.size(), 3);
    if (entries.size() == 3) {
        expectEqU("  entry 0 id", entries[0].id, 1785740504);
        expectEqU("  entry 0 modificationTimestamp", entries[0].modificationTimestamp, 1785745246);
        expectEqU("  entry 1 id", entries[1].id, 1785760357);
        expectEqU("  entry 1 modificationTimestamp", entries[1].modificationTimestamp, 1785764046);
        expectEqU("  entry 2 id", entries[2].id, 1788194033);
        expectEqU("  entry 2 modificationTimestamp", entries[2].modificationTimestamp, 1788195590);
    }
}

void testDecodeTruncatedResponseReturnsEmpty()
{
    const auto body = fromHex("f02400018000c8000124560001");
    const auto entries = LogEntries::decode(body);
    expectEqU("decode truncated response: count", entries.size(), 0);
}

void testDecodeEmptyResponseReturnsEmpty()
{
    const auto entries = LogEntries::decode({});
    expectEqU("decode empty response: count", entries.size(), 0);
}

} // namespace

int main()
{
    testDecodeRealEntriesResponse();
    testDecodeTruncatedResponseReturnsEmpty();
    testDecodeEmptyResponseReturnsEmpty();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
