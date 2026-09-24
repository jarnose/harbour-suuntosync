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
// The handle-walk this skips turned out NOT to be required: re-reading the
// capture showed the official app itself runs it only for the *first*
// /Data fetch on a connection (it's schema introspection, cached
// afterwards) and then fetches the 2nd and 3rd workouts with exactly this
// GET -> 0x10 -> stream sequence and no walk at all. Throws
// std::invalid_argument if ackBody is shorter than 6 bytes.
std::vector<uint8_t> encodeStreamStartTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody);

// The matching "stop the bulk stream" request for a stream started with
// encodeStreamStartTrigger() - same body, byte for byte, only the message
// type differs (0x11 rather than 0x10); the watch answers TYPE=0x09 the
// way it answers a start with TYPE=0x08.
//
// **This is not optional.** Real-hardware testing 2026-09-22: a second
// /Data fetch on the same BLE connection gets its GET acked normally and
// its start trigger acked normally, and then the watch simply never sends
// any bulk data - because as far as it's concerned the previous stream on
// that resource is still open. The capture shows the official app sending
// this stop after every single bulk transfer completes, before going on to
// anything else. Without it, exactly one /Data fetch per connection works
// and every one after it times out in silence.
//
// (The handle here is the resource's, not the request's: /Logbook/byId/
// <id>/Data resolves to the same Whiteboard ResourceId - 00 24 0e on
// Jarno's Race - for every logbook id, since the id is a path *parameter*
// rather than part of the resource's identity. That's why the same stop
// body works for whichever workout was just fetched.)
//
// Throws std::invalid_argument if ackBody is shorter than 6 bytes.
std::vector<uint8_t> encodeStreamStopTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody);

// Builds the TYPE=0x0D "fetch by handle" request that returns a structured
// resource's actual data directly from the handle named by a GET's ack -
// for /Logbook/Entries specifically, this skips the entire multi-step
// handle-walk (the 0x0b/0x0d/0x03/0x05 property-by-property descriptor
// walk documented in docs/logbook-data-format.md) and goes straight from
// the initial GET's ack to the real LogEntries array.
//
// ackBody is the body of the TYPE=0x02 ack to the initial GET (same as
// encodeStreamStartTrigger's ackBody parameter). The request body is
// [0xF0][ackBody[1]][ackBody[2]][0x01][0x80][0x00][0x00] - i.e. the same
// 2-byte "handle" the ack names at offset 1-2, wrapped in a fixed 7-byte
// envelope. Confirmed byte-for-byte (CRC32 included) against a real
// captured request (frame reqid 0x04c5, built from the ack at reqid
// 0x04af) whose real response decoded to three LogEntry records whose Id
// values exactly match logbook ids independently observed elsewhere in
// the same capture (used in separate /Logbook/byId/<id>/Data,
// /Summary and /Descriptors requests) - see
// docs/logbook-data-format.md and tests/test_mdswirecodec.cpp.
//
// What's NOT confirmed: whether this shortcut is reliable in general (it
// comes from re-deriving one real capture, the same low-risk-to-try,
// only-checkable-on-real-hardware situation as encodeStreamStartTrigger)
// or specific to this one capture's watch/firmware state. Throws
// std::invalid_argument if ackBody is shorter than 3 bytes.
std::vector<uint8_t> encodeEntriesFetchTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody);

// Builds a TYPE=0x0D paged read for a resource that is returned in fixed
// pages rather than as a bulk stream - /Logbook/byId/<id>/Summary and
// /Descriptors both work this way in the capture. Body is the ack's first
// 6 bytes (resource handle + the fixed 0x01 0x80 0x00) followed by
// 0x01 0x06 0x00 and the byte offset to read from, little-endian uint32.
//
// The reply is a TYPE=0x05 whose body carries a fixed 19-byte header and
// then up to 451 payload bytes. Two fields of that header matter:
//   - offset 6, uint16 LE: a status - 100 ("continue", more pages follow)
//     or 200 ("ok", this is the last page).
//   - offset 11, uint16 LE: the payload length, which also equals
//     body.size() - 19 in every captured page.
// Read at offset 0, then advance by each page's payload length until a
// page comes back with status 200. Concatenated, the payloads are an
// ordinary SBEM0103 stream (see sbemcontainer.h).
//
// Byte-for-byte confirmed against the real captured /Summary fetch (frames
// with requestIds 0x0542/0x0543/0x0544, built from the GET ack at 0x0539).
// Throws std::invalid_argument if ackBody is shorter than 6 bytes.
// A handle fetch with parameters.
//
// encodeEntriesFetchTrigger()'s body turns out to be the general case with
// none: [ackBody(6)][0x00]. The captured sleep fetch is the same envelope
// with two - [ackBody(6)][0x02][len16][bytes][len16][bytes] - so /Entries
// is just the zero-parameter spelling of this.
//
// The 16-bit field before each parameter is a TYPE code, not a length -
// three captured parameters settle it, because no length rule fits all
// three:
//
//   0x0008  8-byte timestamp   (NewerThan, ms since epoch)
//   0x000C  11-byte string     ("mdsSlp.sbm" + NUL, length implied by the NUL)
//   0x0006  4-byte integer     (byte offset into the file)
//
// So 6 = 32-bit int, 8 = 64-bit int, 12 = NUL-terminated string. That
// matches protocol_v9 dispatching on a DataType kind rather than carrying
// explicit lengths (docs/logbook-data-format.md). Only these three are
// confirmed; anything else is a guess.
struct FetchParameter
{
    uint16_t typeCode;
    std::vector<uint8_t> bytes;
};

// The three confirmed type codes.
constexpr uint16_t kParamInt32 = 0x0006;
constexpr uint16_t kParamInt64 = 0x0008;
constexpr uint16_t kParamString = 0x000C;

std::vector<uint8_t> encodeParameterisedFetch(uint16_t requestId,
                                                const std::vector<uint8_t> &ackBody,
                                                const std::vector<FetchParameter> &parameters);

// A write. Same body as the fetch above - the capture shows PUT frames with
// zero parameters ("f0 53 0e 01 80 00 00") and with one - and differs only
// in the frame type, 0x0E.
//
// This is what the official app uses to put settings onto the watch: the
// cloud URL and session key the watch needs for its own WiFi downloads
// (which is all "syncing GPS performance" actually is), SuuntoPlus plugin
// ids, the timezone, and deleting a file it asked the watch to render.
std::vector<uint8_t> encodePut(uint16_t requestId, const std::vector<uint8_t> &ackBody,
                                const std::vector<FetchParameter> &parameters);

// The common case: a single NUL-terminated string. Confirmed against three
// captured writes - a URL, a session key and a plugin id - all of which use
// type code 0x000C.
std::vector<uint8_t> encodePutString(uint16_t requestId, const std::vector<uint8_t> &ackBody,
                                      const std::string &value);

// The sleep/activity timeline fetch: asks the watch to render everything
// newer than `newerThanMs` into `filename` on its own filesystem, which is
// then read back through /Dev/FileSystem/Stream. See
// docs/watch-push-resources.md - this is what /Daily/Sleep/Timeline/Data
// does, and it is why the MDS-level "/Sleep/<serial>/Entries" path never
// existed on the wire.
std::vector<uint8_t> encodeTimelineFileFetch(uint16_t requestId,
                                               const std::vector<uint8_t> &ackBody,
                                               int64_t newerThanMs,
                                               const std::string &filename);

// Reads `filename` from the watch's filesystem at `offset`, through a
// handle obtained by GET /Dev/FileSystem/Stream. The reply carries about
// 450 bytes of file content after its own header; repeated reads at
// increasing offsets walk the whole file. This is how the sleep timeline
// comes back - see docs/watch-push-resources.md.
std::vector<uint8_t> encodeFileReadRequest(uint16_t requestId,
                                             const std::vector<uint8_t> &ackBody,
                                             const std::string &filename,
                                             uint32_t offset);

std::vector<uint8_t> encodePagedReadRequest(uint16_t requestId, const std::vector<uint8_t> &ackBody,
                                              uint32_t offset);

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
