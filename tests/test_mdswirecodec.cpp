// Standalone, Qt-free test harness for MdsWireCodec, run with plain g++
// (see the build command at the bottom of this comment) since this
// environment has no Sailfish/Qt toolchain. Not yet wired into CMakeLists -
// that's a follow-up once the codec has proven itself against more captured
// traffic (see ~/.claude/plans/agile-hopping-harp.md, Phase 6).
//
// Every expected byte string below is copied verbatim from a real Bluetooth
// HCI snoop log capture (Suunto Race, official Android app, 2026-09-21) -
// see wb_traffic.tsv in that session's scratchpad for the full decoded
// trace this was extracted from.
//
//   g++ -std=c++17 -I../src/ble ../src/ble/mdswirecodec.cpp test_mdswirecodec.cpp -o /tmp/test_mdswirecodec && /tmp/test_mdswirecodec

#include "../src/ble/mdswirecodec.h"

#include <cstdio>
#include <cstdlib>
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

std::string toHex(const std::vector<uint8_t> &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

void expectEq(const std::string &name, const std::string &actual, const std::string &expected)
{
    if (actual == expected) {
        std::printf("PASS %s\n", name.c_str());
    } else {
        std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name.c_str(),
                     expected.c_str(), actual.c_str());
        ++g_failures;
    }
}

void expectEqU(const std::string &name, unsigned actual, unsigned expected)
{
    if (actual == expected) {
        std::printf("PASS %s\n", name.c_str());
    } else {
        std::printf("FAIL %s: expected %u, got %u\n", name.c_str(), expected, actual);
        ++g_failures;
    }
}

// --- captured request round-trips (phone -> watch, Write Without Response, handle 0x0012) ---

void testEncodeLogbookEntries()
{
    // frame 6644, t=382.007s: GET /Logbook/Entries, requestId 0x04AF
    const auto encoded = Mds::encodeGetRequest(0x04AF, "/Logbook/Entries");
    expectEq("encodeGetRequest(/Logbook/Entries)", toHex(encoded),
              "7ea50a1400af04018000102f4c6f67626f6f6b2f456e7472696573eecbb1ed7e");
}

void testEncodeSystemMode()
{
    // frame 3154, t=330.787s: GET /System/Mode, requestId 0x0192
    const auto encoded = Mds::encodeGetRequest(0x0192, "/System/Mode");
    expectEq("encodeGetRequest(/System/Mode)", toHex(encoded),
              "7ea50a10009201018000" "0c2f53797374656d2f4d6f6465943595257e");
}

void testEncodeLogbookByIdData()
{
    // frame 7826, t=389.894s: GET /Logbook/byId/1785740504/Data, requestId 0x052A
    const auto encoded = Mds::encodeGetRequest(0x052A, "/Logbook/byId/1785740504/Data");
    expectEq("encodeGetRequest(/Logbook/byId/.../Data)", toHex(encoded),
              "7ea50a21002a050180001d2f4c6f67626f6f6b2f627949642f313738353734303530342f44617461996658537e");
}

void testLiteralHandshakeRequest()
{
    // frame 3138, t=330.608s: the very first write on the connection,
    // before any GET - TYPE=0x12, reqId=0. See mdswirecodec.h's comment on
    // literalSessionHandshakeRequest() for why this is a verbatim byte
    // replay rather than a from-scratch encoding.
    const auto handshake = Mds::literalSessionHandshakeRequest();
    expectEq("literalSessionHandshakeRequest()", toHex(handshake),
              "7ea5122000000009092016455641100441101000000004010200000000000300000000000000004204a8677e");
}

void testEncodeStreamStartTrigger()
{
    // frame 7868, t=390.492s: the TYPE=0x10 trigger that starts the bulk
    // /Logbook/byId/1785740504/Data stream, requestId 0x0535, built from
    // the TYPE=0x02 ack at frame 7828 (requestId 0x052A - a *different*
    // requestId than the trigger itself, since the trigger is its own new
    // request; only its ack's *body* feeds into this). See
    // mdswirecodec.h's doc comment on encodeStreamStartTrigger() for what
    // this does and doesn't confirm about the wider fetch sequence.
    const auto ackBody = fromHex("00240e018000c800");
    const auto encoded = Mds::encodeStreamStartTrigger(0x0535, ackBody);
    expectEq("encodeStreamStartTrigger(...)", toHex(encoded),
              "7ea5100700350500240e01800000516dae727e");
}

// --- captured response decode (watch -> phone, Handle Value Notification, handle 0x0015) ---

void testDecodeSinglePacketResponse()
{
    // frame 4364, t=347.977s: short ack-style response, reqId echoes 0x02A1 (673)
    // from the "/Logbook/UnsynchronisedLogs" GET at frame 4362.
    const auto raw = fromHex("7ea5020800a102f0240d018000c8009ad22ebe7e");
    Mds::Decoder decoder;
    const auto frames = decoder.feed(raw.data(), raw.size());
    expectEqU("decode single-packet response: frame count", frames.size(), 1);
    if (frames.size() == 1) {
        expectEqU("  type", frames[0].type, 0x02);
        expectEqU("  requestId", frames[0].requestId, 0x02A1);
        expectEqU("  body length", frames[0].body.size(), 8);
        expectEq("  body hex", toHex(frames[0].body), "f0240d018000c800");
    }
}

void testDecodeMultiFragmentResponse()
{
    // frames 3178 + 3179: one logical message split across two BLE
    // notification PDUs - only the first carries the leading 0x7E, only the
    // second carries the trailing 0x7E. Fed here as two separate feed()
    // calls to exercise the reassembly path exactly as BLE delivery would.
    const auto frag1 = fromHex(
            "7ea50534009601f00003018000c800230028348e3f001b00001c000000010000000200000022"
            "000000ffffe301001cffff4d6f646500016a016b0185");
    const auto frag2 = fromHex("31d59e7e");

    Mds::Decoder decoder;
    auto frames = decoder.feed(frag1.data(), frag1.size());
    expectEqU("decode multi-fragment response: frames after fragment 1", frames.size(), 0);

    frames = decoder.feed(frag2.data(), frag2.size());
    expectEqU("decode multi-fragment response: frames after fragment 2", frames.size(), 1);
    if (frames.size() == 1) {
        expectEqU("  type", frames[0].type, 0x05);
        expectEqU("  requestId", frames[0].requestId, 0x0196);
        expectEqU("  body length", frames[0].body.size(), 52);
    }
}

void testDecoderRejectsCorruptedCrc()
{
    auto raw = fromHex("7ea5020800a102f0240d018000c8009ad22ebe7e");
    raw[raw.size() - 2] ^= 0xFF; // flip a byte inside the CRC trailer
    Mds::Decoder decoder;
    const auto frames = decoder.feed(raw.data(), raw.size());
    expectEqU("decoder drops a frame with a corrupted CRC", frames.size(), 0);
}

} // namespace

int main()
{
    testEncodeLogbookEntries();
    testEncodeSystemMode();
    testEncodeLogbookByIdData();
    testLiteralHandshakeRequest();
    testEncodeStreamStartTrigger();
    testDecodeSinglePacketResponse();
    testDecodeMultiFragmentResponse();
    testDecoderRejectsCorruptedCrc();

    if (g_failures == 0) {
        std::printf("\nAll tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failures);
    return 1;
}
