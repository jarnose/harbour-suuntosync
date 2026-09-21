#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Codec for the Movesense/Suunto "Whiteboard" BLE wire protocol used by the
// watch<->phone MDS channel (GATT service 61353090-8231-49cc-b57a-886370740041,
// write-without-response characteristic 17816557-5652-417f-909f-3aee61e5fa85,
// notify characteristic 34802252-7185-4d5d-b431-630e7050e8f0).
//
// Deliberately Qt-free (STL only): every rule here was reverse engineered
// from a real Bluetooth HCI snoop log (Suunto Race, official app, Android,
// captured 2026-09-21) and is meant to be validated against those literal
// captured bytes with a plain g++ test harness *before* any of it touches
// QLowEnergyService - see tests/test_mdswirecodec.cpp and
// ~/.claude/plans/agile-hopping-harp.md (Phase 6).
//
// Wire format, empirically confirmed against >10 independent captured
// messages (both directions, single- and multi-BLE-packet):
//
//   0x7E [ SYNC(1)=0xA5 TYPE(1) LEN_LO LEN_HI REQID_LO REQID_HI BODY(LEN bytes) CRC32_LE(4) ] 0x7E
//
// - Everything between the two 0x7E delimiters is SLIP byte-stuffed
//   (0x7E -> 0x7D 0x5E, 0x7D -> 0x7D 0x5D) - confirmed present in the
//   capture (hundreds of real 0x7D 0x5E / 0x7D 0x5D pairs, and every lone
//   0x7D byte in the whole trace is part of exactly one such pair).
// - A single logical message may be split across several consecutive BLE
//   ATT PDUs (Write Without Response going out, Handle Value Notification
//   coming back): only the first fragment carries the leading 0x7E and only
//   the last carries the trailing 0x7E - fragments must be concatenated
//   *before* SLIP-unescaping and CRC verification. Confirmed on a real
//   64-byte two-fragment notification.
// - LEN is a little-endian uint16 counting BODY only (i.e. NOT counting
//   SYNC/TYPE/LEN/REQID, and NOT counting the CRC trailer).
// - CRC32 is the standard IEEE 802.3 / zlib CRC-32 (poly 0xEDB88320,
//   init/xorout 0xFFFFFFFF), computed over SYNC..end-of-BODY (i.e.
//   everything after the opening 0x7E up to but excluding the CRC itself),
//   encoded little-endian. Verified byte-for-byte against 4+ independent
//   real messages.
// - REQID is a little-endian uint16 the watch echoes back unchanged in its
//   response - use it to correlate requests with responses.
// - For a GET request (TYPE=0x0A, confirmed for /System/Mode and every
//   /Logbook/... path captured), BODY = [0x01][0x80 0x00][PATHLEN(1)][PATH
//   ASCII bytes]. The 0x01 verb byte and the 0x80 0x00 pair were constant
//   across every GET observed; their exact meaning (verb code vs. a fixed
//   client/whiteboard id) isn't confirmed, and PUT/SUBSCRIBE verb bytes are
//   UNKNOWN (no such request appeared in this capture - needed before
//   Phase 7's notification push can be encoded).
// - Response BODY contents are opaque here (type-specific; the codec only
//   validates the generic envelope, e.g. it decoded TYPE=0x02 and TYPE=0x05
//   response envelopes correctly without knowing what either one means) -
//   interpreting a given response's BODY is the caller's job.
namespace Mds {

struct Frame
{
    uint8_t type = 0;
    uint16_t requestId = 0;
    std::vector<uint8_t> body;
};

// IEEE 802.3 / zlib CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout
// 0xFFFFFFFF) - matches Python's zlib.crc32(), which is what every trailer
// in the capture was cross-checked against.
uint32_t crc32(const uint8_t *data, size_t length);

// Builds a full SLIP-framed, CRC-checked GET request for the given
// Whiteboard resource path (e.g. "/Logbook/Entries"), ready to be written
// (in MTU-sized chunks, via Write Without Response) to the write
// characteristic. Byte-for-byte reproduces the real app's request for the
// same (requestId, path) pair (checked against the captured
// "/Logbook/Entries" GET, requestId 0x04AF).
std::vector<uint8_t> encodeGetRequest(uint16_t requestId, const std::string &path);

// A GET request always timed out with zero response from the watch (real
// hardware, 2026-09-21) until this was tried: the real app's very first
// write after StartNotify(), BEFORE any /System/Mode or /Logbook/... GET, is
// a TYPE=0x12, reqId=0 message that no GET request in the capture resembles
// - it isn't a path-based GET at all (its body is 32 bytes of what looks
// like a fixed capability/version announcement, not ASCII). The watch's
// reply is TYPE=0x13, reqId=0. This function is a literal, byte-for-byte
// replay of that captured request - NOT a from-scratch encoding, because
// the body's field-level meaning isn't understood well enough to construct
// safely from parameters. It's reused verbatim on the theory that it's a
// fixed per-protocol-version handshake rather than something that needs to
// vary per connection/session; that theory is what actually sending this
// on real hardware will confirm or refute, not anything checkable here.
// Call this once, right after StartNotify() succeeds and before any
// encodeGetRequest() call - see MdsWhiteboardClient.
std::vector<uint8_t> literalSessionHandshakeRequest();

// Builds the TYPE=0x10 "start the bulk data stream" trigger for a paginated/
// streamed resource such as /Logbook/byId/<id>/Data - send this after the
// initial encodeGetRequest() for that path gets back its TYPE=0x02 ack, and
// a flood of TYPE=0x01, requestId=0 notifications carrying the (still
// Heatshrink-compressed) payload should follow (see
// docs/logbook-data-format.md for that side of the pipeline).
//
// ackBody is the *body* of that TYPE=0x02 ack, unmodified. The trigger body
// is simply ackBody's first 6 bytes with a single 0x00 byte appended in
// place of the ack's own trailing 2 bytes - confirmed byte-for-byte
// (including the resulting CRC32) against a real captured trigger (frame
// 7868, requestId 0x0535, built from the ack at frame 7828) - see
// tests/test_mdswirecodec.cpp.
//
// What's NOT confirmed: the real capture this was derived from didn't go
// straight from the ack to this trigger - the official app ran a long
// handle-based "walk" of intermediate 0x0b/0x0d/0x03/0x05 exchanges in
// between (see docs/logbook-data-format.md's provenance section), which
// this function skips entirely. Whether that walk is genuinely required to
// "warm up" the resource before the watch will honour this trigger, or is
// just the app fetching UI-only metadata (size, a display name, "bytes" as
// a unit string - all seen in that walk's responses) that isn't needed for
// the data fetch itself, is unknown until tried on real hardware. Throws
// std::invalid_argument if ackBody is shorter than 6 bytes.
std::vector<uint8_t> encodeStreamStartTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody);

// Incremental SLIP frame reassembler + envelope parser/validator. Feed it
// raw bytes as they arrive from the notify characteristic, in order,
// regardless of how they were split across BLE PDUs; each call returns the
// complete, CRC-verified Frame(s) that became available. A frame whose CRC
// doesn't check out is silently dropped (not returned) rather than throwing,
// since a real BLE link can lose/corrupt a fragment.
class Decoder
{
public:
    std::vector<Frame> feed(const uint8_t *data, size_t length);

private:
    bool m_inFrame = false;
    bool m_escapeNext = false;
    std::vector<uint8_t> m_buffer;

    bool tryParse(Frame *out) const;
};

} // namespace Mds
