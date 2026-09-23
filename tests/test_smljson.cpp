// Qt-free test for the SML JSON writer - the upload side.
//
//   g++ -std=c++17 ../src/cloud/smljson.cpp ../src/cloud/iso8601.cpp \
//       ../src/ble/smldecoder.cpp ../src/ble/sbemdescriptors.cpp \
//       ../src/ble/sbemcontainer.cpp heatshrink_decoder.o test_smljson.cpp \
//       -o /tmp/test_smljson && /tmp/test_smljson
//
// Fed a real captured /Data payload from the watch, and checked against the
// shape of a real captured *upload* (tests/fixtures/cloud_sml_samples.json,
// taken off the wire from the official app). The two are different workouts,
// so values can't be compared - the structure can, and that is what this
// asserts. It also writes its output to /tmp for eyeballing.

#include "../src/cloud/smljson.h"
#include "../src/ble/sbemcontainer.h"

#include <clocale>
#include <cstdio>
#include <fstream>
#include <cctype>
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

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

int countOf(const std::string &s, const std::string &needle)
{
    int n = 0;
    for (size_t pos = s.find(needle); pos != std::string::npos;
         pos = s.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// Brackets balance and no string is left open - a cheap structural check
// that catches the writer emitting a stray comma or an unterminated quote
// without pulling in a JSON parser.
bool wellFormed(const std::string &s)
{
    int depth = 0;
    bool inString = false, escaped = false;
    for (char c : s) {
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
        else if (c == ',') {
            // ",}" or ",]" would be a trailing comma
        }
    }
    return depth == 0 && !inString
            && !contains(s, ",}") && !contains(s, ",]")
            && !contains(s, "{,") && !contains(s, "[,");
}

std::string buildFromFixture(const std::string &path)
{
    const std::vector<uint8_t> compressed = readFile(path);
    if (compressed.empty()) {
        check(false, "fixture " + path + " could be read");
        return std::string();
    }
    const std::vector<Sbem::Chunk> chunks =
            Sbem::parseContainer(Sbem::heatshrinkDecompress(compressed));
    // +03:00, the offset the captured upload used.
    return SmlJson::buildDocument(chunks, "suunto-2352D0000247", 180);
}

// The bug that got to the server: snprintf's %g honours the C locale, so on
// a Finnish phone every double came out as "1,35" and the whole document
// was invalid JSON. The tests here never saw it because they ran under the
// C locale. This asserts it directly, under a locale that uses a comma.
void testDecimalSeparatorSurvivesACommaLocale()
{
    const char *applied = std::setlocale(LC_NUMERIC, "fi_FI");
    if (!applied)
        applied = std::setlocale(LC_NUMERIC, "de_DE.utf8");
    if (!applied) {
        std::printf("note: no comma-decimal locale available, skipping "
                     "(install fi_FI or de_DE to cover this)\n");
        return;
    }
    // Sanity: confirm the locale really does use a comma, so a passing
    // assertion below means something.
    char probe[32];
    std::snprintf(probe, sizeof(probe), "%.2f", 1.35);
    check(std::string(probe).find(',') != std::string::npos,
           std::string("locale ") + applied + " really formats 1.35 as " + probe);

    const std::string doc = buildFromFixture("fixtures/logbook_data_heatshrink.bin");
    std::setlocale(LC_NUMERIC, "C");

    check(!doc.empty(), "a document was produced under a comma locale");
    // The only commas in valid output separate JSON values, never digits.
    bool digitComma = false;
    for (size_t i = 1; i + 1 < doc.size(); ++i) {
        if (doc[i] == ',' && std::isdigit(static_cast<unsigned char>(doc[i-1]))
                && std::isdigit(static_cast<unsigned char>(doc[i+1]))) {
            digitComma = true;
            std::printf("   first offender near: %s\n", doc.substr(i - 20, 40).c_str());
            break;
        }
    }
    check(!digitComma, "no decimal commas in the output");
}

void testEnvelopeMatchesTheCapturedShape(const std::string &doc)
{
    check(!doc.empty(), "a document was produced");
    check(wellFormed(doc), "brackets balance, no trailing commas");
    check(doc.compare(0, 12, "{\"Samples\":[") == 0, "starts with the Samples envelope");
    check(doc.size() > 2 && doc.compare(doc.size() - 2, 2, "]}") == 0,
           "ends with the Samples envelope");
    check(contains(doc, "\"Attributes\":{\"suunto/sml\":"),
           "each entry carries the suunto/sml attribute wrapper");
    check(contains(doc, "\"Source\":\"suunto-2352D0000247\""), "Source is the watch id");
    check(contains(doc, "\"TimeISO8601\":\"20"), "entries are timestamped");
    // The offset must be carried, not normalised to UTC - the captured
    // upload stamps local time plus offset.
    check(contains(doc, "+03:00"), "timestamps keep the local offset");
}

void testFieldsLandWhereTheSchemaSaysTheyShould(const std::string &doc)
{
    // Dotted descriptor names have to become nested objects, not flat keys.
    check(!contains(doc, "\"Sample.HR\""), "no flattened dotted keys leak through");
    check(contains(doc, "{\"Sample\":{"), "readings nest under Sample");
    check(contains(doc, "\"HR\":"), "heart rate is present");
    check(contains(doc, "\"Altitude\":"), "altitude is present");

    // Hertz, not bpm: a captured upload has "HR": 1.35. A human heart rate
    // in hertz is around 1-3, so a value of 60+ would mean the canonical
    // unit had been converted away somewhere.
    const size_t hr = doc.find("\"HR\":");
    if (hr != std::string::npos) {
        const double v = std::atof(doc.c_str() + hr + 5);
        check(v > 0.3 && v < 4.5, "HR is in hertz (" + std::to_string(v) + ")");
    }
}

void testEventsBecomeAnArray(const std::string &doc)
{
    // "Sample.Events.Array.Lap.Type" must produce Events: [ { Lap: {...} } ].
    if (!contains(doc, "\"Events\"")) {
        std::printf("note: this fixture has no event chunks, skipping array checks\n");
        return;
    }
    check(contains(doc, "\"Events\":[{"), "Events is an array of objects");
    // And the enum is written as the cloud's own name.
    check(contains(doc, "\"Lap\":{\"Type\":\"") || !contains(doc, "\"Lap\":"),
           "a lap type is a name, not a number");
}

void testAgainstTheCapturedUploadsKeys(const std::string &doc)
{
    // Every top-level suunto/sml key the real upload used must be one this
    // writer can produce. (The reverse isn't required: the captured workout
    // was 15 seconds long and exercised fewer chunk types.)
    const char *capturedKeys[] = { "\"Sample\":", "\"HR\":", "\"Altitude\":",
                                    "\"Cadence\":", "\"Distance\":" };
    for (const char *key : capturedKeys)
        check(contains(doc, key), std::string("captured key present: ") + key);
}

} // namespace

int main()
{
    const std::string doc = buildFromFixture("fixtures/logbook_data_heatshrink.bin");
    if (doc.empty()) {
        std::printf("\nNo document built - cannot continue.\n");
        return 1;
    }

    std::printf("built %zu bytes, %d samples\n", doc.size(),
                 countOf(doc, "\"TimeISO8601\""));
    std::ofstream("/tmp/generated_samples.json") << doc;
    std::printf("(written to /tmp/generated_samples.json)\n\n");

    testDecimalSeparatorSurvivesACommaLocale();
    testEnvelopeMatchesTheCapturedShape(doc);
    testFieldsLandWhereTheSchemaSaysTheyShould(doc);
    testEventsBecomeAnArray(doc);
    testAgainstTheCapturedUploadsKeys(doc);

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
