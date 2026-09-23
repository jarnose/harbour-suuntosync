#include "mdswirecodec.h"

#include <array>
#include <stdexcept>

namespace Mds {

namespace {

constexpr uint8_t kFrameDelimiter = 0x7E;
constexpr uint8_t kEscape = 0x7D;
constexpr uint8_t kEscapedDelimiter = 0x5E; // 0x7D 0x5E -> literal 0x7E
constexpr uint8_t kEscapedEscape = 0x5D;    // 0x7D 0x5D -> literal 0x7D
constexpr uint8_t kSync = 0xA5;
constexpr uint8_t kTypeGetRequest = 0x0A;
constexpr uint8_t kTypeStreamStartTrigger = 0x10;
constexpr uint8_t kTypeStreamStopTrigger = 0x11;
constexpr uint8_t kTypeHandleFetch = 0x0D;
constexpr uint8_t kGetVerb = 0x01;

std::array<uint32_t, 256> makeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}

void appendEscaped(std::vector<uint8_t> *out, uint8_t byte)
{
    if (byte == kFrameDelimiter) {
        out->push_back(kEscape);
        out->push_back(kEscapedDelimiter);
    } else if (byte == kEscape) {
        out->push_back(kEscape);
        out->push_back(kEscapedEscape);
    } else {
        out->push_back(byte);
    }
}

// Shared by every encoder: SYNC TYPE LEN_LO LEN_HI REQID_LO REQID_HI BODY
// CRC32_LE, SLIP-escaped and delimited.
std::vector<uint8_t> encodeFrame(uint8_t type, uint16_t requestId, const std::vector<uint8_t> &body)
{
    std::vector<uint8_t> inner;
    const uint16_t bodyLen = static_cast<uint16_t>(body.size());
    inner.push_back(kSync);
    inner.push_back(type);
    inner.push_back(static_cast<uint8_t>(bodyLen & 0xFF));
    inner.push_back(static_cast<uint8_t>((bodyLen >> 8) & 0xFF));
    inner.push_back(static_cast<uint8_t>(requestId & 0xFF));
    inner.push_back(static_cast<uint8_t>((requestId >> 8) & 0xFF));
    inner.insert(inner.end(), body.begin(), body.end());

    const uint32_t crc = Mds::crc32(inner.data(), inner.size());
    inner.push_back(static_cast<uint8_t>(crc & 0xFF));
    inner.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    inner.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    inner.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));

    std::vector<uint8_t> framed;
    framed.reserve(inner.size() + 2);
    framed.push_back(kFrameDelimiter);
    for (uint8_t b : inner)
        appendEscaped(&framed, b);
    framed.push_back(kFrameDelimiter);
    return framed;
}

} // namespace

uint32_t crc32(const uint8_t *data, size_t length)
{
    static const std::array<uint32_t, 256> table = makeCrcTable();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> encodeGetRequest(uint16_t requestId, const std::string &path)
{
    // BODY = verb(1)=0x01, const(2)=0x80 0x00, pathLen(1), path
    std::vector<uint8_t> body;
    body.reserve(4 + path.size());
    body.push_back(kGetVerb);
    body.push_back(0x80);
    body.push_back(0x00);
    body.push_back(static_cast<uint8_t>(path.size()));
    body.insert(body.end(), path.begin(), path.end());
    return encodeFrame(kTypeGetRequest, requestId, body);
}

namespace {

// Start and stop differ only in the message type - the body (the resource
// handle the ack named, the fixed 0x01 0x80 0x00, and a trailing zero) is
// byte-for-byte identical in the capture.
std::vector<uint8_t> streamTriggerBody(const std::vector<uint8_t> &ackBody, const char *what)
{
    if (ackBody.size() < 6)
        throw std::invalid_argument(std::string(what) + ": ackBody shorter than 6 bytes");
    std::vector<uint8_t> body(ackBody.begin(), ackBody.begin() + 6);
    body.push_back(0x00);
    return body;
}

} // namespace

std::vector<uint8_t> encodeStreamStartTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody)
{
    return encodeFrame(kTypeStreamStartTrigger, requestId,
                        streamTriggerBody(ackBody, "encodeStreamStartTrigger"));
}

std::vector<uint8_t> encodeStreamStopTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody)
{
    return encodeFrame(kTypeStreamStopTrigger, requestId,
                        streamTriggerBody(ackBody, "encodeStreamStopTrigger"));
}

std::vector<uint8_t> encodeEntriesFetchTrigger(uint16_t requestId, const std::vector<uint8_t> &ackBody)
{
    if (ackBody.size() < 3)
        throw std::invalid_argument("encodeEntriesFetchTrigger: ackBody shorter than 3 bytes");
    std::vector<uint8_t> body;
    body.reserve(7);
    body.push_back(0xF0);
    body.push_back(ackBody[1]);
    body.push_back(ackBody[2]);
    body.push_back(0x01);
    body.push_back(0x80);
    body.push_back(0x00);
    body.push_back(0x00);
    return encodeFrame(kTypeHandleFetch, requestId, body);
}

std::vector<uint8_t> encodeParameterisedFetch(uint16_t requestId,
                                                const std::vector<uint8_t> &ackBody,
                                                const std::vector<FetchParameter> &parameters)
{
    if (ackBody.size() < 6)
        throw std::invalid_argument("encodeParameterisedFetch: ackBody shorter than 6 bytes");
    if (parameters.size() > 255)
        throw std::invalid_argument("encodeParameterisedFetch: too many parameters");

    std::vector<uint8_t> body(ackBody.begin(), ackBody.begin() + 6);
    body.push_back(static_cast<uint8_t>(parameters.size()));
    for (const FetchParameter &p : parameters) {
        body.push_back(static_cast<uint8_t>(p.typeCode & 0xFF));
        body.push_back(static_cast<uint8_t>((p.typeCode >> 8) & 0xFF));
        body.insert(body.end(), p.bytes.begin(), p.bytes.end());
    }
    return encodeFrame(kTypeHandleFetch, requestId, body);
}

std::vector<uint8_t> encodeTimelineFileFetch(uint16_t requestId,
                                               const std::vector<uint8_t> &ackBody,
                                               int64_t newerThanMs,
                                               const std::string &filename)
{
    std::vector<uint8_t> timestamp(8);
    for (int i = 0; i < 8; ++i)
        timestamp[i] = static_cast<uint8_t>((static_cast<uint64_t>(newerThanMs) >> (8 * i)) & 0xFF);

    std::vector<uint8_t> name(filename.begin(), filename.end());
    name.push_back(0x00);

    return encodeParameterisedFetch(requestId, ackBody,
                                     { { kParamInt64, timestamp },
                                       { kParamString, name } });
}

std::vector<uint8_t> encodeFileReadRequest(uint16_t requestId,
                                             const std::vector<uint8_t> &ackBody,
                                             const std::string &filename,
                                             uint32_t offset)
{
    std::vector<uint8_t> name(filename.begin(), filename.end());
    name.push_back(0x00);

    std::vector<uint8_t> off(4);
    for (int i = 0; i < 4; ++i)
        off[i] = static_cast<uint8_t>((offset >> (8 * i)) & 0xFF);

    return encodeParameterisedFetch(requestId, ackBody,
                                     { { kParamString, name },
                                       { kParamInt32, off } });
}

std::vector<uint8_t> encodePagedReadRequest(uint16_t requestId, const std::vector<uint8_t> &ackBody,
                                              uint32_t offset)
{
    if (ackBody.size() < 6)
        throw std::invalid_argument("encodePagedReadRequest: ackBody shorter than 6 bytes");
    std::vector<uint8_t> body(ackBody.begin(), ackBody.begin() + 6);
    body.push_back(0x01);
    body.push_back(0x06);
    body.push_back(0x00);
    body.push_back(static_cast<uint8_t>(offset & 0xFF));
    body.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>((offset >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((offset >> 24) & 0xFF));
    return encodeFrame(kTypeHandleFetch, requestId, body);
}

std::vector<uint8_t> literalSessionHandshakeRequest()
{
    // Captured verbatim (Suunto Race, official Android app, frame 3138 of
    // the 2026-09-21 HCI snoop log) - the very first write on the
    // connection, before any GET. Already fully SLIP-framed with a valid
    // CRC (verified: TYPE=0x12, reqId=0, 32-byte body) - send as-is.
    static const uint8_t kBytes[] = {
        0x7e, 0xa5, 0x12, 0x20, 0x00, 0x00, 0x00, 0x09, 0x09, 0x20, 0x16,
        0x45, 0x56, 0x41, 0x10, 0x04, 0x41, 0x10, 0x10, 0x00, 0x00, 0x00,
        0x04, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x04, 0xa8, 0x67, 0x7e,
    };
    return std::vector<uint8_t>(std::begin(kBytes), std::end(kBytes));
}

bool Decoder::tryParse(Frame *out) const
{
    // m_buffer holds the already-unescaped bytes between the delimiters:
    // SYNC TYPE LEN_LO LEN_HI REQID_LO REQID_HI BODY... CRC32(4)
    constexpr size_t kHeaderSize = 6;
    constexpr size_t kCrcSize = 4;
    if (m_buffer.size() < kHeaderSize + kCrcSize)
        return false;
    if (m_buffer[0] != kSync)
        return false;

    const uint16_t length = m_buffer[2] | (static_cast<uint16_t>(m_buffer[3]) << 8);
    if (m_buffer.size() != kHeaderSize + length + kCrcSize)
        return false;

    const size_t crcOffset = kHeaderSize + length;
    const uint32_t computed = Mds::crc32(m_buffer.data(), crcOffset);
    const uint32_t received = m_buffer[crcOffset]
            | (static_cast<uint32_t>(m_buffer[crcOffset + 1]) << 8)
            | (static_cast<uint32_t>(m_buffer[crcOffset + 2]) << 16)
            | (static_cast<uint32_t>(m_buffer[crcOffset + 3]) << 24);
    if (computed != received)
        return false;

    out->type = m_buffer[1];
    out->requestId = m_buffer[4] | (static_cast<uint16_t>(m_buffer[5]) << 8);
    out->body.assign(m_buffer.begin() + kHeaderSize, m_buffer.begin() + crcOffset);
    return true;
}

std::vector<Frame> Decoder::feed(const uint8_t *data, size_t length)
{
    std::vector<Frame> frames;

    for (size_t i = 0; i < length; ++i) {
        const uint8_t b = data[i];

        if (b == kFrameDelimiter && !m_escapeNext) {
            if (m_inFrame && !m_buffer.empty()) {
                Frame frame;
                if (tryParse(&frame))
                    frames.push_back(std::move(frame));
                // Whether or not it parsed, a delimiter always ends the
                // current frame - a corrupt/undecodable one is just dropped.
            }
            m_buffer.clear();
            m_inFrame = true; // this delimiter also opens the next frame
            m_escapeNext = false;
            continue;
        }

        if (!m_inFrame)
            continue; // stray byte outside any frame - ignore

        if (m_escapeNext) {
            if (b == kEscapedDelimiter)
                m_buffer.push_back(kFrameDelimiter);
            else if (b == kEscapedEscape)
                m_buffer.push_back(kEscape);
            else
                m_buffer.push_back(b); // malformed escape - keep raw byte rather than drop silently
            m_escapeNext = false;
            continue;
        }

        if (b == kEscape) {
            m_escapeNext = true;
            continue;
        }

        m_buffer.push_back(b);
    }

    return frames;
}

} // namespace Mds
