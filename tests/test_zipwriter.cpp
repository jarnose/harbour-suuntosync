// Qt-free test for the minimal ZIP writer.
//
//   g++ -std=c++17 ../src/cloud/zipwriter.cpp test_zipwriter.cpp -lz \
//       -o /tmp/test_zipwriter && /tmp/test_zipwriter
//
// The structural assertions are here; the real proof is that the archive
// this writes to /tmp/test_zipwriter_out.zip is readable by an independent
// implementation, which the shell wrapper checks with Python's zipfile
// (see the sibling run in the session notes). A format whose only consumer
// is a server we cannot test against deserves a second opinion.

#include "../src/cloud/zipwriter.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

uint32_t read32(const std::vector<uint8_t> &d, size_t pos)
{
    return static_cast<uint32_t>(d[pos]) | (static_cast<uint32_t>(d[pos + 1]) << 8)
            | (static_cast<uint32_t>(d[pos + 2]) << 16)
            | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

uint16_t read16(const std::vector<uint8_t> &d, size_t pos)
{
    return static_cast<uint16_t>(d[pos]) | (static_cast<uint16_t>(d[pos + 1]) << 8);
}

} // namespace

int main()
{
    // Repetitive content, so deflate has something to do and a failure to
    // compress would be obvious in the size.
    std::string samples = "{\"Samples\":[";
    for (int i = 0; i < 400; ++i)
        samples += "{\"Attributes\":{\"suunto/sml\":{\"Sample\":{\"HR\":1.35}}}},";
    samples.back() = ']';
    samples += "}";

    const std::vector<ZipWriter::Entry> entries = {
        { "samples.json", samples },
        { "summary.json", "{\"Samples\":[{\"Attributes\":{}}]}" },
    };
    const std::vector<uint8_t> zip = ZipWriter::build(entries);

    check(!zip.empty(), "an archive was produced");
    if (zip.empty())
        return 1;

    check(zip[0] == 'P' && zip[1] == 'K' && zip[2] == 3 && zip[3] == 4,
           "starts with a local file header signature");
    check(zip.size() < samples.size() / 2,
           "deflate actually compressed (" + std::to_string(zip.size()) + " bytes from "
           + std::to_string(samples.size()) + ")");

    // End-of-central-directory sits in the last 22 bytes (no comment).
    const size_t eocd = zip.size() - 22;
    check(read32(zip, eocd) == 0x06054B50, "ends with an end-of-central-directory record");
    check(read16(zip, eocd + 8) == 2, "records two entries on this disk");
    check(read16(zip, eocd + 10) == 2, "records two entries in total");

    const uint32_t centralSize = read32(zip, eocd + 12);
    const uint32_t centralStart = read32(zip, eocd + 16);
    check(centralStart + centralSize == eocd,
           "central directory offset and size meet the EOCD record");
    check(read32(zip, centralStart) == 0x02014B50,
           "central directory starts with its own signature");

    std::ofstream out("/tmp/test_zipwriter_out.zip", std::ios::binary);
    out.write(reinterpret_cast<const char *>(zip.data()),
               static_cast<std::streamsize>(zip.size()));
    out.close();
    std::printf("(archive written to /tmp/test_zipwriter_out.zip for independent checking)\n");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
