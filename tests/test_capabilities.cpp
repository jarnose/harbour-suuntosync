// Build and run:
//   g++ -std=c++14 -o t test_capabilities.cpp ../src/ble/capabilities.cpp -I ../src && ./t
//
// The vector is the real first page of /Dev/Capabilities from the Suunto
// Race, lifted out of btsnoop_watchface_ok_20261012.log - header included,
// so the item count is checked against what the watch actually sent.

#include "ble/capabilities.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

const uint8_t kFirstPage[] = {
    0xf0, 0x7a, 0x00, 0x01, 0x80, 0x00, 0x64, 0x00, 0x00, 0x7a, 0xe5, 0x24,
    0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x30, 0x00,
    0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x00, 0x6a, 0x00,
    0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0xa1, 0x00,
    0x00, 0x00, 0xb0, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0xce, 0x00,
    0x00, 0x00, 0x66, 0x65, 0x61, 0x74, 0x5f, 0x67, 0x75, 0x69, 0x64, 0x65,
    0x73, 0x5f, 0x76, 0x35, 0x00, 0x66, 0x65, 0x61, 0x74, 0x5f, 0x67, 0x75,
    0x69, 0x64, 0x65, 0x74, 0x65, 0x6d, 0x70, 0x6c, 0x61, 0x74, 0x65, 0x5f,
    0x76, 0x38, 0x00, 0x66, 0x65, 0x61, 0x74, 0x5f, 0x74, 0x72, 0x61, 0x69,
    0x6e, 0x69, 0x6e, 0x67, 0x70, 0x6c, 0x61, 0x6e, 0x5f, 0x76, 0x31, 0x00,
    0x66, 0x65, 0x61, 0x74, 0x5f, 0x77, 0x62, 0x73, 0x74, 0x6f, 0x72, 0x61,
    0x67, 0x65, 0x5f, 0x76, 0x31, 0x00,
};

} // namespace

int main()
{
    const std::vector<uint8_t> page(std::begin(kFirstPage), std::end(kFirstPage));

    // The header says ten, and the five real pages said 10, 10, 10, 10, 9 -
    // which is the 49 capabilities this watch reports. Advancing a paged read
    // by the payload's size instead would re-read page one for ever.
    check(Capabilities::itemsInPage(page) == 10, "the paged header reports ten items");
    check(Capabilities::itemsInPage(std::vector<uint8_t>()) == 0,
           "a short body reports no items");

    const std::vector<uint8_t> payload(page.begin() + 19, page.end());
    const std::vector<std::string> caps = Capabilities::parse(payload);
    check(!caps.empty(), "capabilities come out of the payload");

    bool sorted = true;
    for (size_t i = 1; i < caps.size(); ++i) {
        if (!(caps[i - 1] < caps[i]))
            sorted = false;
    }
    check(sorted, "they come back sorted and without duplicates");

    const char *const wanted[] = { "feat_guides_v5", "feat_guidetemplate_v8" };
    for (const char *name : wanted) {
        bool present = false;
        for (const std::string &cap : caps) {
            if (cap == name)
                present = true;
        }
        check(present, std::string("found ") + name);
    }

    // The offset table in front of the strings must not leak through, and
    // neither must a fragment left at the page boundary.
    for (const std::string &cap : caps)
        check(cap.size() >= 4, "nothing shorter than four characters: " + cap);

    const std::string joined = Capabilities::join(caps);
    check(joined.find(",,") == std::string::npos, "no empty entry in the parameter");
    check(joined.compare(0, caps.front().size(), caps.front()) == 0,
           "the parameter starts with the first capability");

    if (failures == 0)
        std::printf("\nAll assertions passed.\n");
    else
        std::printf("\n%d assertion(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
