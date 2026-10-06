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

// The same, falling back to the sending package when the category says
// nothing. Email needs this: Sailfish's mail server posts its notifications
// with **no category at all** - measured, `from messageserver5
// category=(none)` - so a mapped package name is the only thing left to go
// on. The table grows by measurement, not by guessing at names.
uint8_t categoryFor(const std::string &sailfishCategory, const std::string &appId);

// The whole conversion. `nowSeconds` is passed in rather than read from the
// clock so that a test is a test.
Ancs::Notification toWatchNotification(const Candidate &candidate, uint32_t nowSeconds);

// An incoming call, which does not come through the notification server at
// all and so cannot come through the Candidate path above.
//
// Measured on the device: a call arriving raises
// `org.nemomobile.voicecall.VoiceCall.statusChanged` with `5 "incoming"` and
// then `playRingtone`, and the first `Notify` of the whole exchange lands ten
// seconds later, when the call has already been missed. There is no
// `x-nemo.call.incoming` category in
// /usr/share/lipstick/notificationcategories either - only `missed` - so
// there is nothing for categoryFor() to map. This builds the notification
// directly instead, which keeps the invented part visible rather than
// dressing a call up as a category Sailfish does not have.
//
// `lineId` is the caller's number, blank when withheld. `callId` is a stable
// number for this particular call - the daemon derives it from the call's
// own D-Bus object path - so the notification can be taken off the watch
// again when the ringing stops.
Ancs::Notification incomingCall(const std::string &lineId, uint32_t callId,
                                  uint32_t nowSeconds);

} // namespace NotificationRouter
