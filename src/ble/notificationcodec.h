#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Encoder for a phone notification pushed to the watch over Whiteboard:
// PUT /Device/Connectivity/Ble/Ancs/Notification/Add, and its matching
// .../Notification/Del.
//
// Qt-free (STL only), like every other decoder here, so it is testable with
// plain g++ - see tests/test_notificationcodec.cpp, which rebuilds three
// real captured requests byte for byte.
//
// Two independent sources agree on what the fields mean, which is why this
// is an encoder rather than a byte replay:
//
//  - three btsnoop captures from a Suunto Race on 2026-09-26, differing in
//    app, title, message length and button count;
//  - the official Android app's own
//    `com.suunto.connectivity.notifications.MdsNotification` and
//    `AncsPackages`, decompiled.
//
// docs/notifications.md carries the full account, including the category
// table and the rules the app uses to pick a title and a message.
namespace Ancs {

// The standard ANCS CategoryID values. The watch's own enum, read off it
// during the schema walk, is in this order, and the Android app's mapping
// table produces exactly these numbers - see docs/notifications.md.
enum CategoryId : uint8_t {
    CategoryOther = 0,
    CategoryIncomingCall = 1,
    CategoryMissedCall = 2,
    CategoryVoicemail = 3,
    CategorySocial = 4,
    CategorySchedule = 5,
    CategoryEmail = 6,
    CategoryNews = 7,
    CategoryHealthAndFitness = 8,
    CategoryBusinessAndFinance = 9,
    CategoryLocation = 10,
    CategoryEntertainment = 11,
};

// The handful of values in the request structure that are per watch.
//
// The layout is not: both watches agree on the length rule, the four
// one-byte fields, the string-offset base, the label array and its
// alignment. What differs is five constants, and they differ the way the
// SBEM descriptor ids differ - because they come from the watch's own
// metadata, which is per firmware.
//
// Captured, both of them, which is the only reason this exists rather than
// a guess:
//
//                        Race        9 Baro
//   structure type       0x1209      0x1207
//   form byte            0x6a        0x00
//   prologue             01 1f 01 21 01 00 00 00
//   word at rel 28       1           0
//   word at rel 44       42          0
//
// On the Baro three of the four unknown constants are simply zero, which
// says what they probably are: optional metadata the older firmware does
// not populate. Both of the Race's odd ones - the 1 at 28 and the 42 at 44
// - point at a NUL byte inside the header, and so does the Baro's 0, so
// both spellings mean "an empty string".
struct Profile
{
    uint8_t structureTypeLow;
    uint8_t form;
    uint8_t prologue[4];
    uint32_t wordAt28;
    uint32_t wordAt44;
};

const Profile &raceProfile();
const Profile &baroProfile();

// Picks a profile from the handle the watch gave for .../Notification/Add.
// Its third byte is the resource's own id on that firmware - 0x04 on a
// Race, 0x03 on a 9 Baro - and that is the only thing available to tell
// them apart at the moment the request is built.
//
// **Returns nullptr for any other watch**, deliberately. Guessing a
// profile would produce a notification the watch refuses with 400, which
// is what a Race's profile sent to a 9 Baro actually did; saying "this
// watch has not been captured" is more useful than that. One capture of
// the official app adds a model.
//
// (Noted and not relied upon: 2 * ack[2] + 1 happens to give both type
// bytes. Two points fit any line, and the same arithmetic does not hold
// for the other structure-carrying resources in the capture.)
const Profile *profileForAck(const std::vector<uint8_t> &ackBody);

// A button on the watch. `supportsReply` was false in all three captures;
// it is encoded because the field exists, not because it was exercised.
struct Label
{
    std::string text;
    bool supportsReply = false;
};

struct Notification
{
    // Identifies this notification for a later update or removal. Use
    // notificationIdFor() so that an update lands on the same one.
    uint32_t notificationId = 0;

    // False posts a new notification, true replaces an existing one with
    // the same id. Confirmed: the third capture is the second one updated,
    // and this is the only byte of the fixed part that moved.
    bool modifyExisting = false;

    uint8_t categoryId = CategoryOther;

    // Both constants in the official app - a literal 1 and a literal 2 -
    // and constant across all three captures. 2 is ANCS's
    // EventFlagImportant.
    uint8_t categoryCount = 1;
    uint8_t eventFlags = 2;

    // Unix seconds. The watch displays this, so it is local wall-clock time
    // to the user; the capture's value matched the clock on the wall.
    uint32_t date = 0;

    std::string appId;   // the posting application's identifier
    std::string title;
    std::string message;

    std::vector<Label> labels;
};

// The id the official app would use: abs(((sourceId * 31 + categoryId) * 31
// + javaHashCode(appId))), clamped at zero, all in 32-bit wrapping
// arithmetic. Confirmed against both captured ids - Etar's notification 1
// and the test app's notification 0 - so an update or a removal we send can
// address a notification by the same rule the watch has already seen.
//
// javaHashCode is over UTF-16 code units; Android package names are ASCII,
// where that is the same thing. Nothing here needs the app's id to match
// the official app's, so a different rule would work too - this one is
// used because it is known to produce ids the watch accepts.
uint32_t notificationIdFor(int32_t sourceId, uint8_t categoryId, const std::string &appId);

// The single-byte length that prefixes the request structure caps how much
// can be sent. This is the largest total the encoder will produce.
size_t maximumEncodedSize();

// How many bytes encodeRequestData() would produce for this notification.
size_t encodedSize(const Notification &notification);

// Shortens `message`, and then `title` if that is not enough, on UTF-8
// character boundaries, until the notification fits. The watch cuts long
// text off on its own screen anyway; what it cannot do is accept a
// structure whose length field overflowed.
void truncateToFit(Notification &notification);

// The AncsRequestData parameter's payload: a one-byte length, two bytes of
// structure header, then the structure itself. Exposed for testing; the
// two functions below are what a caller wants.
std::vector<uint8_t> encodeRequestData(const Notification &notification,
                                         const Profile &profile);

// A complete PUT frame, ready to write to the notify characteristic in
// MTU-sized chunks.
//
// `addAckBody` is the body of the TYPE=0x02 ack to
// GET /Device/Connectivity/Ble/Ancs/Notification/Add, which names the
// handle - `f0 12 04 01 80 00 ...` on a Race, `f0 12 03 01 80 00 ...` on a
// 9 Baro, so it genuinely differs per watch and cannot be compiled in.
//
// Throws std::invalid_argument if the notification is too large
// (truncateToFit() first), the ack body is too short, or the watch is not
// one whose profile has been captured.
std::vector<uint8_t> encodeAdd(uint16_t requestId, const std::vector<uint8_t> &addAckBody,
                                const Notification &notification);

// The same, for a watch whose profile the caller has in hand - which is
// what the tests use to encode a Race's request and a 9 Baro's from the
// same notification.
std::vector<uint8_t> encodeAdd(uint16_t requestId, const std::vector<uint8_t> &addAckBody,
                                const Notification &notification, const Profile &profile);

// The matching removal, for when the phone's notification is dismissed.
// `removeAckBody` is the ack for .../Notification/Del - Del, not Remove -
// a different resource with its own handle.
std::vector<uint8_t> encodeRemove(uint16_t requestId, const std::vector<uint8_t> &removeAckBody,
                                   uint32_t notificationId);

} // namespace Ancs
