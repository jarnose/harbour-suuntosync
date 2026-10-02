#include "notificationrouter.h"

namespace NotificationRouter {

namespace {

struct Mapping
{
    const char *prefix;
    uint8_t category;
};

// Longest prefix wins, so a specific entry may sit anywhere in this array
// rather than depending on its order.
const Mapping kMappings[] = {
    { "x-nemo.call.missed", Ancs::CategoryMissedCall },
    { "x-nemo.messaging.voicemail", Ancs::CategoryVoicemail },
    { "x-nemo.messaging", Ancs::CategoryEmail },
    { "x-nemo.calendar", Ancs::CategorySchedule },
    { "x-nemo.social", Ancs::CategorySocial },
    { "x-nemo.email", Ancs::CategoryEmail },
    { "im.received", Ancs::CategoryEmail },
    // Whisperfish registers its own categories, and its call one is the
    // only route to IncomingCall that needs no SIM.
    { "harbour-whisperfish-call", Ancs::CategoryIncomingCall },
    { "harbour-whisperfish-message", Ancs::CategoryEmail },
};

// Categories that stay on the phone: the system's own notices, and errors
// about the phone rather than about anything a person wants on a wrist.
const char *const kIgnoredPrefixes[] = {
    "x-nemo.battery",
    "x-nemo.system-update",
    "x-nemo.messaging.error",
    "x-nemo.messaging.authorizationrequest",
    "x-jolla.lipstick",
    "x-jolla.cellular.error",
};

bool startsWith(const std::string &text, const char *prefix)
{
    return text.compare(0, std::string(prefix).size(), prefix) == 0;
}

bool blank(const std::string &text)
{
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return false;
    }
    return true;
}

} // namespace

bool shouldForward(const Candidate &candidate)
{
    // Our own, including the one the probe button sends - forwarding those
    // would be a loop with extra steps.
    if (candidate.appId.find("suuntosync") != std::string::npos)
        return false;

    // A notification with no title renders as an empty box. The official
    // Android app drops these too, and logs the extras it could not find a
    // title among.
    if (blank(candidate.title))
        return false;

    for (const char *prefix : kIgnoredPrefixes) {
        if (startsWith(candidate.category, prefix))
            return false;
    }
    return true;
}

uint8_t categoryFor(const std::string &sailfishCategory)
{
    uint8_t best = Ancs::CategoryOther;
    size_t bestLength = 0;
    for (const Mapping &mapping : kMappings) {
        const size_t length = std::string(mapping.prefix).size();
        if (length <= bestLength || !startsWith(sailfishCategory, mapping.prefix))
            continue;
        bestLength = length;
        best = mapping.category;
    }
    return best;
}

Ancs::Notification toWatchNotification(const Candidate &candidate, uint32_t nowSeconds)
{
    Ancs::Notification out;
    out.appId = candidate.appId;
    out.title = candidate.title;
    out.message = candidate.message;
    out.categoryId = categoryFor(candidate.category);
    out.date = nowSeconds;

    // Derived from the phone's own id, so a later close can take the right
    // notification off the watch - and derived the same way the official
    // app derives its ids, so the two could not collide if both ever ran.
    out.notificationId = Ancs::notificationIdFor(static_cast<int32_t>(candidate.phoneId),
                                                  out.categoryId, out.appId);

    // One button. A notification dismissed on the watch ought to disappear
    // there; whether the watch reports the press back is unknown, so this
    // is a label and not yet an action.
    Ancs::Label dismiss;
    dismiss.text = "Dismiss";
    out.labels.push_back(dismiss);

    Ancs::truncateToFit(out);
    return out;
}

} // namespace NotificationRouter
