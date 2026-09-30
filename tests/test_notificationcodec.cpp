// Qt-free golden-vector test for the notification encoder.
//
//   g++ -std=c++17 ../src/ble/mdswirecodec.cpp ../src/ble/notificationcodec.cpp
//       test_notificationcodec.cpp -o /tmp/t && /tmp/t   (one line)
//
// The three vectors are real PUT bodies captured off a Suunto Race on
// 2026-09-26, and unlike every other golden vector in this project they are
// inline rather than in tests/fixtures/. That is deliberate: they contain
// no GPS, no heart rate and no serial - an app id, the word "Testi" and
// some Finnish filler - so the reason fixtures are kept out of the
// repository does not apply, and keeping them here means CI can run this
// suite. It is the newest encoder and the one whose failure would be
// silent, so it is the one worth running on every push.
//
// A differs from B in app, title, message and button count; C is B
// updated, with a 69-character title and a 54-character message. Between
// them they move every field the encoder computes.

#include "../src/ble/notificationcodec.h"
#include "../src/ble/mdswirecodec.h"

#include <cstdio>
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

const char *kCaptureA =
        "f012040180000207000ab4dd21091288146a000001028c05b86a011f012144000000"
        "01000000570000000000000001000000010000005d000000000000002a0000000000"
        "0000000000000100000002000000680000006f72672e6c696e656167656f732e6574"
        "617200546573746900383a343920504d0000000078000000004b0100800000000000"
        "00004469736d69737300536e6f6f7a6500"
        ;

const char *kCaptureB =
        "f012040180000207000111ea45091289146a00000102bc08b86a011f012144000000"
        "010000005600000000000000010000000100000065000000000000002a0000000000"
        "000000000000010000000100000078000000636f6d2e6d616e642e6e6f7469746573"
        "7400546573746920696c6d6f6974757300416c61726976696e2074656b7374690000"
        "000080000000007373004469736d69737300"
        ;

const char *kCaptureC =
        "f012040180000207000111ea450912e5146a010001022309b86a011f012144000000"
        "01000000560000000000000001000000010000009c000000000000002a0000000000"
        "0000000000000100000001000000d4000000636f6d2e6d616e642e6e6f7469746573"
        "7400546573746920696c6d6f6974757320746f7369207069746b616c6c612074656b"
        "7374696c6c61206a6f737461206e61686461616e206d69737461207365206b61746b"
        "65616100506964656d706920616c61726976696e2074656b737469206a6f74746120"
        "6e61686461616e206d69737461207365206b61746b6561610000dc00000000000000"
        "4469736d69737300"
        ;

std::vector<uint8_t> fromHex(const std::string &hex)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string toHex(const std::vector<uint8_t> &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

// The encoder returns a framed message; the vectors are bodies. Decoding
// the frame back is the honest comparison and exercises the round trip.
std::vector<uint8_t> bodyOf(const std::vector<uint8_t> &framed)
{
    Mds::Decoder decoder;
    std::vector<Mds::Frame> frames = decoder.feed(framed.data(), framed.size());
    if (frames.size() != 1)
        return {};
    return frames.front().body;
}

// Each LabelData entry ends in three bytes of uninitialised padding: A
// carries 4b 01 00 and B carries 73 73 00, which is "ss" left over from a
// previous "Dismiss". The encoder writes zeros, so both sides are zeroed
// before comparing - reproducing somebody else's stale buffer would be the
// wrong thing to assert.
std::vector<uint8_t> zeroLabelPadding(std::vector<uint8_t> bytes)
{
    if (bytes.size() < 86)
        return bytes;
    auto u32 = [&bytes](size_t at) {
        return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8)
                | (static_cast<uint32_t>(bytes[at + 2]) << 16)
                | (static_cast<uint32_t>(bytes[at + 3]) << 24);
    };
    const uint32_t count = u32(78);
    const size_t base = 18 + u32(82);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = base + i * 8 + 5;
        if (at + 3 <= bytes.size())
            bytes[at] = bytes[at + 1] = bytes[at + 2] = 0;
    }
    return bytes;
}

// The handle the Race gave for .../Ancs/Notification/Add, as captured.
const std::vector<uint8_t> kAddAck = { 0xf0, 0x12, 0x04, 0x01, 0x80, 0x00 };

void checkVector(const char *name, const char *hex, const Ancs::Notification &notification)
{
    const std::vector<uint8_t> expected = fromHex(hex);
    const std::vector<uint8_t> actual = bodyOf(Ancs::encodeAdd(0x026e, kAddAck, notification));
    check(actual.size() == expected.size(),
          std::string(name) + " encodes to " + std::to_string(expected.size()) + " bytes ("
                  + std::to_string(actual.size()) + ")");
    const bool same = zeroLabelPadding(actual) == zeroLabelPadding(expected);
    check(same, std::string(name) + " encodes byte-for-byte to the captured body");
    if (!same) {
        std::printf("   expected %s\n", toHex(zeroLabelPadding(expected)).c_str());
        std::printf("   actual   %s\n", toHex(zeroLabelPadding(actual)).c_str());
    }
}

} // namespace

int main()
{
    // A: a calendar alert from Etar, two buttons.
    Ancs::Notification a;
    a.notificationId = Ancs::notificationIdFor(1, Ancs::CategoryOther, "org.lineageos.etar");
    a.date = 1790444940;
    a.appId = "org.lineageos.etar";
    a.title = "Testi";
    a.message = "8:49 PM";
    a.labels = { { "Dismiss", false }, { "Snooze", false } };
    check(a.notificationId == 568177674,
          "Etar's notification id is derived, not copied (" + std::to_string(a.notificationId)
                  + ")");
    checkVector("A", kCaptureA, a);

    // B: a test app, one button, longer strings.
    Ancs::Notification b;
    b.notificationId = Ancs::notificationIdFor(0, Ancs::CategoryOther, "com.mand.notitest");
    b.date = 1790445756;
    b.appId = "com.mand.notitest";
    b.title = "Testi ilmoitus";
    b.message = "Alarivin teksti";
    b.labels = { { "Dismiss", false } };
    check(b.notificationId == 1172967681,
          "the test app's notification id is derived too (" + std::to_string(b.notificationId)
                  + ")");
    checkVector("B", kCaptureB, b);

    // C: B again, updated, with much longer text.
    Ancs::Notification c = b;
    c.modifyExisting = true;
    c.date = 1790445859;
    c.title = "Testi ilmoitus tosi pitkalla tekstilla josta nahdaan mista se katkeaa";
    c.message = "Pidempi alarivin teksti jotta nahdaan mista se katkeaa";
    check(c.title.size() == 69 && c.message.size() == 54,
          "C's strings are 69 and 54 characters");
    checkVector("C", kCaptureC, c);

    // The id rule has to survive 32-bit overflow the way Java's does, or an
    // update would address a notification the watch has never seen.
    check(Ancs::notificationIdFor(0, 0, "com.mand.notitest") == 1172967681,
          "a package whose hash is negative still yields the captured id");
    check(Ancs::notificationIdFor(2147483647, 11, "x") <= 2147483647u,
          "the id never comes back negative");

    // Removal is the same handle envelope with one parameter.
    const std::vector<uint8_t> removeAck = { 0xf0, 0x12, 0x05, 0x01, 0x80, 0x00 };
    const std::vector<uint8_t> remove =
            bodyOf(Ancs::encodeRemove(0x027a, removeAck, a.notificationId));
    check(toHex(remove) == "f012050180000107000ab4dd21",
          "removal matches the captured /Notification/Remove body (" + toHex(remove) + ")");

    // Long text must be cut rather than overflow the one-byte length.
    Ancs::Notification big = b;
    big.message = std::string(400, 'x');
    bool threw = false;
    try {
        Ancs::encodeAdd(1, kAddAck, big);
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, "a notification too long for the length byte is refused");
    Ancs::truncateToFit(big);
    check(Ancs::encodedSize(big) <= Ancs::maximumEncodedSize(),
          "truncateToFit brings it inside the limit (" + std::to_string(Ancs::encodedSize(big))
                  + " <= " + std::to_string(Ancs::maximumEncodedSize()) + ")");
    check(!bodyOf(Ancs::encodeAdd(1, kAddAck, big)).empty(), "and it then encodes");
    check(big.title == b.title, "the title survives when trimming the message is enough");

    // Multi-byte characters must not be cut in half.
    Ancs::Notification utf8 = b;
    utf8.message = std::string(60, 'x');
    for (int i = 0; i < 60; ++i)
        utf8.message += "\xc3\xa4";
    Ancs::truncateToFit(utf8);
    size_t continuations = 0;
    for (size_t i = 0; i < utf8.message.size(); ++i)
        if ((static_cast<unsigned char>(utf8.message[i]) & 0xC0) == 0x80)
            ++continuations;
    check(continuations * 2 == utf8.message.size() - 60,
          "every two-byte character in the trimmed message is still whole");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
