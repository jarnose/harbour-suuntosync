// Qt-free test for the notification routing decisions.
//
//   g++ -std=c++17 ../src/ble/mdswirecodec.cpp ../src/ble/notificationcodec.cpp
//       ../src/notify/notificationrouter.cpp test_notificationrouter.cpp -o /tmp/t && /tmp/t
//
// The category strings here are real: they are the file names under
// /usr/share/lipstick/notificationcategories on the phone, read off the
// device rather than taken from the freedesktop specification. That matters
// because the specification's categories ("email", "im.received") are not
// what Sailfish actually sends.

#include "../src/notify/notificationrouter.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok)
        ++g_failures;
}

NotificationRouter::Candidate candidate(const std::string &category,
                                         const std::string &title = "Testi",
                                         const std::string &appId = "org.example.app")
{
    NotificationRouter::Candidate c;
    c.category = category;
    c.title = title;
    c.message = "Alarivin teksti";
    c.appId = appId;
    c.phoneId = 477;
    return c;
}

void checkCategory(const char *sailfish, uint8_t expected)
{
    const uint8_t got = NotificationRouter::categoryFor(sailfish);
    check(got == expected, std::string(sailfish) + " -> " + std::to_string(expected) + " (got "
                                   + std::to_string(got) + ")");
}

} // namespace

int main()
{
    // The real categories, mapped.
    checkCategory("x-nemo.messaging.sms", Ancs::CategoryEmail);
    checkCategory("x-nemo.messaging.sms.preview", Ancs::CategoryEmail);
    checkCategory("x-nemo.messaging.mms", Ancs::CategoryEmail);
    checkCategory("x-nemo.messaging.im", Ancs::CategoryEmail);
    checkCategory("x-nemo.messaging.group", Ancs::CategoryEmail);
    checkCategory("im.received", Ancs::CategoryEmail);
    checkCategory("harbour-whisperfish-message", Ancs::CategoryEmail);

    // Longest prefix wins: voicemail and missed calls sit under the same
    // roots as the plain messages and must not be swallowed by them.
    checkCategory("x-nemo.messaging.voicemail", Ancs::CategoryVoicemail);
    checkCategory("x-nemo.messaging.voicemail.group.preview", Ancs::CategoryVoicemail);
    checkCategory("x-nemo.messaging.voicemail-SMS", Ancs::CategoryVoicemail);
    checkCategory("x-nemo.call.missed", Ancs::CategoryMissedCall);
    checkCategory("x-nemo.call.missed.group", Ancs::CategoryMissedCall);
    checkCategory("harbour-whisperfish-call", Ancs::CategoryIncomingCall);

    // Anything unrecognised, including no category at all, is Other rather
    // than a guess.
    checkCategory("", Ancs::CategoryOther);
    checkCategory("x-nemo.something.new", Ancs::CategoryOther);
    checkCategory("x-nemo", Ancs::CategoryOther);

    // What stays on the phone.
    check(NotificationRouter::shouldForward(candidate("x-nemo.messaging.sms")),
          "a text message is forwarded");
    check(!NotificationRouter::shouldForward(candidate("x-nemo.battery")),
          "the battery warning is not");
    check(!NotificationRouter::shouldForward(candidate("x-jolla.lipstick.diskspace")),
          "nor is a disk-space notice");
    check(!NotificationRouter::shouldForward(candidate("x-nemo.messaging.error")),
          "nor a messaging error");
    check(!NotificationRouter::shouldForward(candidate("x-nemo.messaging.sms", "")),
          "nor one with no title");
    check(!NotificationRouter::shouldForward(candidate("x-nemo.messaging.sms", "   ")),
          "nor one whose title is only spaces");
    check(!NotificationRouter::shouldForward(
                  candidate("", "Testi", "io.github.jarnose.suuntosync")),
          "nor our own, which would be a loop");
    check(NotificationRouter::shouldForward(candidate("x-nemo.system-update")) == false,
          "nor a system update");

    // The reason, not just the verdict: the daemon logs it, and "ignoring
    // x-nemo.messaging.im" is a lie when the real problem was an empty
    // title.
    check(NotificationRouter::dropReason(candidate("x-nemo.messaging.sms")) == nullptr,
          "a forwarded notification has no drop reason");
    check(std::string(NotificationRouter::dropReason(candidate("x-nemo.messaging.im", "")))
                  == "no title",
          "an empty title says so");
    check(std::string(NotificationRouter::dropReason(candidate("x-nemo.battery")))
                  == "category stays on the phone",
          "an ignored category says that instead");

    // The package fallback, for the apps that set no category at all.
    check(NotificationRouter::categoryFor("", "messageserver5") == Ancs::CategoryEmail,
          "Sailfish's mail server has no category and is still Email");
    check(NotificationRouter::categoryFor("", "org.example.whatever") == Ancs::CategoryOther,
          "an unknown package with no category stays Other");
    check(NotificationRouter::categoryFor("x-nemo.call.missed", "messageserver5")
                  == Ancs::CategoryMissedCall,
          "a category that says something beats the package table");

    // The conversion.
    NotificationRouter::Candidate mail = candidate("", "Jarno Selanpaa", "messageserver5");
    check(NotificationRouter::toWatchNotification(mail, 1).categoryId == Ancs::CategoryEmail,
          "and the whole conversion uses it");

    const Ancs::Notification out = NotificationRouter::toWatchNotification(
            candidate("x-nemo.messaging.sms"), 1790939702);
    check(out.categoryId == Ancs::CategoryEmail, "a text message converts to category 6");
    check(out.date == 1790939702u, "the date is the one passed in, not the clock");
    check(out.title == "Testi" && out.message == "Alarivin teksti", "title and message carry");
    check(out.labels.size() == 1 && out.labels.front().text == "Dismiss", "one button");
    check(out.notificationId
                  == Ancs::notificationIdFor(477, Ancs::CategoryEmail, "org.example.app"),
          "the id is derived from the phone's own id");
    check(!out.modifyExisting, "and it is an add, not an update");

    // A long message must come back inside the watch's limit, because the
    // encoder refuses rather than truncating on its own.
    NotificationRouter::Candidate big = candidate("x-nemo.messaging.sms");
    big.message = std::string(500, 'x');
    const Ancs::Notification trimmed = NotificationRouter::toWatchNotification(big, 1);
    check(Ancs::encodedSize(trimmed) <= Ancs::maximumEncodedSize(),
          "a 500-character message is trimmed to fit ("
                  + std::to_string(Ancs::encodedSize(trimmed)) + ")");
    check(trimmed.title == "Testi", "and the title survives it");

    if (g_failures == 0) {
        std::printf("\nAll assertions passed.\n");
        return 0;
    }
    std::printf("\n%d assertion(s) FAILED.\n", g_failures);
    return 1;
}
