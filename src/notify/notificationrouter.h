#pragma once

#include "../ble/notificationcodec.h"

#include <cstdint>
#include <string>

// Decides what a phone notification becomes on the watch, and what never
// leaves the phone at all.
//
// Qt-free on purpose, like every other decision table in this project, so
// the mapping can be tested with plain g++ - see
// tests/test_notificationrouter.cpp. The daemon converts its D-Bus
// QStrings into a Candidate and asks here.
namespace NotificationRouter {

// What the daemon knows about one notification, with no Qt in it.
struct Candidate
{
    std::string appId;      // x-nemo-owner when present, app_name otherwise
    std::string title;
    std::string message;
    std::string category;   // Sailfish's own category string
    uint32_t phoneId = 0;   // the id the notification server assigned
};

// Why this notification is not going to the watch, or nullptr if it is.
// A reason rather than a bool because the daemon logs one line per
// notification and "ignoring x-nemo.messaging.im" is misleading when what
// was actually wrong was an empty title.
//
// The categories tested are real ones, read off
// /usr/share/lipstick/notificationcategories on the device rather than
// guessed from the freedesktop specification.
const char *dropReason(const Candidate &candidate);

bool shouldForward(const Candidate &candidate);

// The ANCS category, by longest matching prefix, defaulting to Other.
//
// Two things worth knowing about this table. A text message maps to
// **Email**, because that is what the official Android app does and the
// watch has no message category of its own. And an *incoming call* is not
// normally a notification on Sailfish at all - voicecall-ui shows its own
// screen - so category 1 arrives only from a VoIP app that posts one,
// which is also the only way to exercise it on a phone with no SIM.
uint8_t categoryFor(const std::string &sailfishCategory);

// The whole conversion. `nowSeconds` is passed in rather than read from the
// clock so that a test is a test.
Ancs::Notification toWatchNotification(const Candidate &candidate, uint32_t nowSeconds);

} // namespace NotificationRouter
