#include "mdswirecodec.h"

#include <array>

namespace Mds {

namespace {

constexpr uint8_t kFrameDelimiter = 0x7E;
constexpr uint8_t kEscape = 0x7D;
constexpr uint8_t kEscapedDelimiter = 0x5E; // 0x7D 0x5E -> literal 0x7E
constexpr uint8_t kEscapedEscape = 0x5D;    // 0x7D 0x5D -> literal 0x7D
constexpr uint8_t kSync = 0xA5;
constexpr uint8_t kTypeGetRequest = 0x0A;
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
    // SYNC TYPE LEN_LO LEN_HI REQID_LO REQID_HI BODY(verb, 0x80 0x00, pathLen, path)
    std::vector<uint8_t> inner;
    const uint16_t bodyLen = static_cast<uint16_t>(4 + path.size()); // verb(1)+const(2)+pathLen(1)+path
    inner.push_back(kSync);
    inner.push_back(kTypeGetRequest);
    inner.push_back(static_cast<uint8_t>(bodyLen & 0xFF));
    inner.push_back(static_cast<uint8_t>((bodyLen >> 8) & 0xFF));
    inner.push_back(static_cast<uint8_t>(requestId & 0xFF));
    inner.push_back(static_cast<uint8_t>((requestId >> 8) & 0xFF));
    inner.push_back(kGetVerb);
    inner.push_back(0x80);
    inner.push_back(0x00);
    inner.push_back(static_cast<uint8_t>(path.size()));
    inner.insert(inner.end(), path.begin(), path.end());

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
