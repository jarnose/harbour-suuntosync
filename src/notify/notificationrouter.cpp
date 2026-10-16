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

// Packages whose notifications carry no usable category. One entry, because
// one is what has been measured: Sailfish's mail server, which posts with
// no category and would otherwise land under Other.
const Mapping kPackageMappings[] = {
    { "messageserver5", Ancs::CategoryEmail },
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

const char *dropReason(const Candidate &candidate)
{
    // Our own, including the one the probe button sends - forwarding those
    // would be a loop with extra steps.
    if (candidate.appId.find("suuntosync") != std::string::npos)
        return "our own notification";

    // A notification with no title renders as an empty box. The official
    // Android app drops these too, and logs the extras it could not find a
    // title among.
    if (blank(candidate.title))
        return "no title";

    for (const char *prefix : kIgnoredPrefixes) {
        if (startsWith(candidate.category, prefix))
            return "category stays on the phone";
    }
    return nullptr;
}

bool shouldForward(const Candidate &candidate)
{
    return dropReason(candidate) == nullptr;
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

uint8_t categoryFor(const std::string &sailfishCategory, const std::string &appId)
{
    // The category first: it is the more specific of the two, and an app
    // that sets one means it.
    const uint8_t byCategory = categoryFor(sailfishCategory);
    if (byCategory != Ancs::CategoryOther)
        return byCategory;

    for (const Mapping &mapping : kPackageMappings) {
        if (appId.find(mapping.prefix) != std::string::npos)
            return mapping.category;
    }
    return Ancs::CategoryOther;
}

Ancs::Notification toWatchNotification(const Candidate &candidate, uint32_t nowSeconds)
{
    Ancs::Notification out;
    out.appId = candidate.appId;
    out.title = candidate.title;
    out.message = candidate.message;
    out.categoryId = categoryFor(candidate.category, candidate.appId);
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

Ancs::Notification incomingCall(const std::string &caller, const std::string &number,
                                  uint32_t callId, uint32_t nowSeconds)
{
    Ancs::Notification out;
    // The sending application, as the watch sees it. voicecall-ui's own name,
    // so a watch that groups by application groups calls with calls.
    out.appId = "voicecall-ui";

    // Who above what. A name when the address book has one, with the number
    // under it - which is worth the line, since it says whether this is the
    // mobile or the landline. Otherwise the number is all there is to say
    // who, and the message carries what.
    //
    // A withheld number with no name would leave an empty title, which
    // renders as an empty box - and which dropReason() refuses for exactly
    // that reason on an ordinary notification.
    if (!blank(caller)) {
        out.title = caller;
        out.message = blank(number) ? "Incoming call" : number;
    } else {
        out.title = blank(number) ? "Unknown caller" : number;
        out.message = "Incoming call";
    }
    // Category 1, as the official app sends it - confirmed on the wire in
    // btsnoop_s7_call_20261016.log, where this watch takes one. What froze
    // it three times on 2026-10-16 was almost certainly not the category
    // but the three words an ordinary notification carries beside it: the
    // category selects the firmware path that reads them, and ours came
    // from captures older than this firmware. Ancs::encodeAdd() swaps in
    // raceCallProfile(), which replays what the official app sends.
    //
    // **This is the experiment**, not a settled answer. If the watch
    // freezes again the constants were not it either, and category 0 -
    // which it demonstrably survives - is where this belongs.
    out.categoryId = Ancs::CategoryIncomingCall;
    out.date = nowSeconds;
    out.notificationId = Ancs::notificationIdFor(static_cast<int32_t>(callId), out.categoryId,
                                                  out.appId);

    // One label, like every other notification and like all four captured
    // requests. This was built without one at first, on the reasoning that a
    // Dismiss button on a ringing call would imply rejecting the call and
    // nobody has established whether the watch reports a press back. The
    // watch settled it: the notification timed out three times while, in the
    // same Whiteboard session, a removal was answered and an ordinary
    // notification was taken. With no labels the structure's label-array
    // offset points at the end of the pool - past the structure - with a
    // count of zero, and the watch appears to refuse that silently, which is
    // how it refuses every malformed frame.
    //
    // The reasoning was also inconsistent: that uncertainty applies to every
    // notification this sends, and they all carry the label.
    // **No label on a ring**, the other way this imitates the official
    // app: its incoming call carries none. The paragraph above argued a
    // label was needed because a label-less frame was refused three times,
    // and docs/notifications.md later records that the official app's own
    // label-less call is answered 200 - so those refusals were the
    // non-response pattern, not the label. Everything from +40 onward in
    // its frame is uninitialised memory, the label count included, so the
    // watch does not read the label array for this form at all.

    Ancs::truncateToFit(out);
    return out;
}

} // namespace NotificationRouter
