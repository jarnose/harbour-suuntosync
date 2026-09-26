// Qt-free golden-vector test for the GPS ephemeris chunk encoder.
//
//   g++ -std=c++17 ../src/ble/mdswirecodec.cpp test_ephemerischunk.cpp
//       -o /tmp/test_ephemerischunk && /tmp/test_ephemerischunk
//
// The fixtures are the first and last chunk bodies of a real upload to a
// Suunto 9 Baro on 2026-09-25, lifted out of the btsnoop capture. The
// blob they carry is the same file the phone had just downloaded, so
// ephemeris_9baro_2026-09-25.bin supplies the data the encoder is fed.

#include "../src/ble/mdswirecodec.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

std::vector<uint8_t> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
}

// The encoder returns a whole SLIP-framed frame; the fixtures are the
// bodies the capture carried. Decoding the frame back is the honest way to
// compare, and it exercises the codec's own round trip - slicing at a
// fixed offset would trip over the escaping the CRC can introduce.
std::vector<uint8_t> bodyOf(const std::vector<uint8_t> &framed)
{
    Mds::Decoder decoder;
    std::vector<Mds::Frame> frames = decoder.feed(framed.data(), framed.size());
    if (frames.size() != 1)
        return {};
    return frames.front().body;
}

} // namespace

int main()
{
    const std::vector<uint8_t> blob = readFile("fixtures/ephemeris_9baro_2026-09-25.bin");
    const std::vector<uint8_t> first = readFile("fixtures/ephemeris_chunk1_body.bin");
    const std::vector<uint8_t> last = readFile("fixtures/ephemeris_chunk_last_body.bin");
    check(blob.size() == 61440, "blob fixture is 61440 bytes ("
           + std::to_string(blob.size()) + ")");
    check(!first.empty() && !last.empty(), "chunk fixtures read");
    if (blob.empty() || first.empty() || last.empty())
        return 1;

    // The handle the watch gave for .../Upload/0, as captured.
    const std::vector<uint8_t> ack = { 0x01, 0x40, 0x07, 0x01, 0x80, 0x00 };

    // Chunk 1: the first 453 bytes, cumulative 453 of 61440.
    const std::vector<uint8_t> encodedFirst =
            Mds::encodeEphemerisChunk(1, ack, 61440, 453, blob.data(), 453);
    check(bodyOf(encodedFirst) == first,
          "first chunk encodes byte-for-byte to the captured body");

    // The last: 135 full chunks precede it, so 61155 bytes are already
    // across and 285 remain.
    const size_t lastOffset = 135 * 453;
    const std::vector<uint8_t> encodedLast =
            Mds::encodeEphemerisChunk(1, ack, 61440, 61440,
                                        blob.data() + lastOffset, blob.size() - lastOffset);
    check(blob.size() - lastOffset == 285, "the final chunk is 285 bytes ("
           + std::to_string(blob.size() - lastOffset) + ")");
    check(bodyOf(encodedLast) == last,
          "last chunk encodes byte-for-byte to the captured body");

    // The whole file, chunked the way the app does it, must put every byte
    // across exactly once and declare the same total throughout.
    size_t offset = 0;
    size_t chunks = 0;
    std::vector<uint8_t> rebuilt;
    while (offset < blob.size()) {
        const size_t length = std::min<size_t>(453, blob.size() - offset);
        offset += length;
        const std::vector<uint8_t> body = bodyOf(
                Mds::encodeEphemerisChunk(1, ack, static_cast<uint32_t>(blob.size()),
                                            static_cast<uint32_t>(offset),
                                            blob.data() + offset - length, length));
        if (body.size() < 18) {
            check(false, "chunk body too short to parse");
            break;
        }
        rebuilt.insert(rebuilt.end(), body.begin() + 18, body.end());
        ++chunks;
    }
    check(chunks == 136, "the file needs 136 chunks (" + std::to_string(chunks) + ")");
    check(rebuilt == blob, "every byte of the file survives the chunking, in order");

    // A chunk longer than the length field can describe must be refused
    // rather than silently truncated.
    bool threw = false;
    try {
        std::vector<uint8_t> big(70000, 0);
        Mds::encodeEphemerisChunk(1, ack, 70000, 70000, big.data(), big.size());
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "a chunk that cannot fit the 16-bit length is refused");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
