#include "notificationcodec.h"

#include "mdswirecodec.h"

#include <cstdlib>
#include <stdexcept>

namespace Ancs {

namespace {

// 0x14 prefixes every structure parameter in the capture, whatever the
// resource and whatever the watch. The byte after it, and the type code's
// low byte, are per watch - see Profile.
const uint8_t kStructureTag = 0x14;

// Which watch a handle belongs to. The third byte of the ack is the
// resource's id on that firmware.
const uint8_t kRaceAddResource = 0x04;
const uint8_t kBaroAddResource = 0x03;

// A 32-bit unsigned parameter. Confirmed by the notification id, which
// travels this way in both the add and the remove.
const uint16_t kParamUInt32 = 0x0007;

// String references inside the structure are offsets from the body's own
// first byte, and the string pool always begins right after the fixed
// part.
const uint32_t kFixedPartSize = 68;

void appendU32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void appendString(std::vector<uint8_t> &pool, const std::string &value)
{
    pool.insert(pool.end(), value.begin(), value.end());
    pool.push_back(0);
}

// The structure body, without the length byte or the two header bytes.
std::vector<uint8_t> encodeStructureBody(const Notification &n, const Profile &profile)
{
    // The pool first, because the fixed part is full of offsets into it.
    std::vector<uint8_t> pool;
    const uint32_t appIdOffset = kFixedPartSize;
    appendString(pool, n.appId);
    const uint32_t titleOffset = kFixedPartSize + static_cast<uint32_t>(pool.size());
    appendString(pool, n.title);
    const uint32_t messageOffset = kFixedPartSize + static_cast<uint32_t>(pool.size());
    appendString(pool, n.message);

    // The label array is four-byte aligned relative to the body's start.
    while ((kFixedPartSize + pool.size()) % 4 != 0)
        pool.push_back(0);

    const uint32_t labelArrayOffset = kFixedPartSize + static_cast<uint32_t>(pool.size());
    const size_t labelArrayAt = pool.size();
    pool.resize(pool.size() + 8 * n.labels.size(), 0);

    std::vector<uint32_t> labelTextOffsets;
    for (const Label &label : n.labels) {
        labelTextOffsets.push_back(kFixedPartSize + static_cast<uint32_t>(pool.size()));
        appendString(pool, label.text);
    }

    // Each entry is a 32-bit offset, one byte of supportsReply and three
    // bytes of padding. The capture's padding is uninitialised - one entry
    // carries "ss" left over from a previous "Dismiss" - so it is written
    // as zero here rather than imitated.
    for (size_t i = 0; i < n.labels.size(); ++i) {
        const size_t at = labelArrayAt + i * 8;
        const uint32_t offset = labelTextOffsets[i];
        pool[at] = static_cast<uint8_t>(offset & 0xFF);
        pool[at + 1] = static_cast<uint8_t>((offset >> 8) & 0xFF);
        pool[at + 2] = static_cast<uint8_t>((offset >> 16) & 0xFF);
        pool[at + 3] = static_cast<uint8_t>((offset >> 24) & 0xFF);
        pool[at + 4] = n.labels[i].supportsReply ? 1 : 0;
    }

    std::vector<uint8_t> body;
    body.push_back(n.modifyExisting ? 1 : 0);
    body.push_back(n.categoryId);
    body.push_back(n.categoryCount);
    body.push_back(n.eventFlags);
    appendU32(body, n.date);
    body.insert(body.end(), profile.prologue, profile.prologue + 4);

    // Everything from here to the pool was byte-identical across all three
    // captures. Two of the constants - the 1 at +28 and the 42 at +44 -
    // are offsets to a NUL byte inside the fixed part itself, so they read
    // as empty strings; `subtitle` is presumably one of them, since the
    // official app has no such field to fill. They are replayed as-is,
    // which is safe precisely because they point into the part we replay.
    appendU32(body, appIdOffset);
    appendU32(body, 1);
    appendU32(body, titleOffset);
    appendU32(body, 0);
    appendU32(body, profile.wordAt28);
    appendU32(body, 1);
    appendU32(body, messageOffset);
    appendU32(body, 0);
    appendU32(body, profile.wordAt44);
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, 1);
    appendU32(body, static_cast<uint32_t>(n.labels.size()));
    appendU32(body, labelArrayOffset);

    body.insert(body.end(), pool.begin(), pool.end());
    return body;
}

} // namespace

uint32_t notificationIdFor(int32_t sourceId, uint8_t categoryId, const std::string &appId)
{
    uint32_t hash = 0;
    for (unsigned char c : appId)
        hash = hash * 31u + c;

    uint32_t value = static_cast<uint32_t>(sourceId) * 31u;
    value += categoryId;
    value *= 31u;
    value += hash;

    const int32_t signedValue = static_cast<int32_t>(value);
    if (signedValue < 0) {
        // Math.abs(Integer.MIN_VALUE) is itself, and the app then clamps.
        if (value == 0x80000000u)
            return 0;
        return static_cast<uint32_t>(-signedValue);
    }
    return value;
}

size_t maximumEncodedSize()
{
    // The length byte covers the form byte and the structure body, so the
    // body can reach 254. Ahead of them: 15 bytes of frame and parameter
    // header, the length byte, and the structure tag.
    return 15 + 1 + 1 + 1 + 254;
}

size_t encodedSize(const Notification &notification)
{
    // The profile does not change any length: it is five constants, all of
    // them fixed-width, so a Race's size is a 9 Baro's.
    return 15 + 1 + 1 + 1 + encodeStructureBody(notification, raceProfile()).size();
}

const Profile &raceProfile()
{
    static const Profile profile = { 0x09, 0x6a, { 0x01, 0x1f, 0x01, 0x21 }, 1, 42 };
    return profile;
}

const Profile &baroProfile()
{
    static const Profile profile = { 0x07, 0x00, { 0x01, 0x00, 0x00, 0x00 }, 0, 0 };
    return profile;
}

const Profile *profileForAck(const std::vector<uint8_t> &ackBody)
{
    if (ackBody.size() < 3)
        return nullptr;
    if (ackBody[2] == kRaceAddResource)
        return &raceProfile();
    if (ackBody[2] == kBaroAddResource)
        return &baroProfile();
    return nullptr;
}

void truncateToFit(Notification &notification)
{
    const size_t limit = maximumEncodedSize();

    std::string *fields[] = { &notification.message, &notification.title };
    for (std::string *field : fields) {
        while (encodedSize(notification) > limit && !field->empty()) {
            size_t length = field->size() - 1;
            while (length > 0 && (static_cast<unsigned char>((*field)[length]) & 0xC0) == 0x80)
                --length;
            field->resize(length);
        }
    }
}

std::vector<uint8_t> encodeRequestData(const Notification &notification,
                                        const Profile &profile)
{
    const std::vector<uint8_t> body = encodeStructureBody(notification, profile);
    if (body.size() + 1 > 255)
        throw std::invalid_argument("Ancs::encodeRequestData: notification too large to encode");

    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(body.size() + 1));
    out.push_back(kStructureTag);
    out.push_back(profile.form);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<uint8_t> encodeAdd(uint16_t requestId, const std::vector<uint8_t> &addAckBody,
                                const Notification &notification, const Profile &profile)
{
    if (addAckBody.size() < 6)
        throw std::invalid_argument("Ancs::encodeAdd: ackBody shorter than 6 bytes");

    std::vector<uint8_t> id;
    appendU32(id, notification.notificationId);

    // The type's high byte is the ack's second byte in all six
    // structure-carrying writes in the capture - the resource family the
    // handle belongs to. Only the low byte is per watch.
    const uint16_t structureType =
            static_cast<uint16_t>(addAckBody[1] << 8) | profile.structureTypeLow;

    return Mds::encodePut(requestId, addAckBody,
                           { { kParamUInt32, id },
                             { structureType, encodeRequestData(notification, profile) } });
}

std::vector<uint8_t> encodeAdd(uint16_t requestId, const std::vector<uint8_t> &addAckBody,
                                const Notification &notification)
{
    const Profile *profile = profileForAck(addAckBody);
    if (!profile) {
        throw std::invalid_argument(
                "Ancs::encodeAdd: this watch's notification layout has not been captured");
    }
    return encodeAdd(requestId, addAckBody, notification, *profile);
}

std::vector<uint8_t> encodeRemove(uint16_t requestId, const std::vector<uint8_t> &removeAckBody,
                                   uint32_t notificationId)
{
    std::vector<uint8_t> id;
    appendU32(id, notificationId);
    return Mds::encodePut(requestId, removeAckBody, { { kParamUInt32, id } });
}

} // namespace Ancs
