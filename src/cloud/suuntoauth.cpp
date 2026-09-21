#include "suuntoauth.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace SuuntoAuth {

namespace {

uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::vector<uint8_t> md_pad_be64len(const uint8_t *data, size_t len)
{
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
        msg.push_back(0x00);
    uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
    return msg;
}

} // namespace

std::vector<uint8_t> sha1(const uint8_t *data, size_t len)
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    const std::vector<uint8_t> msg = md_pad_be64len(data, len);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[chunk + i * 4]) << 24) | (uint32_t(msg[chunk + i * 4 + 1]) << 16)
                    | (uint32_t(msg[chunk + i * 4 + 2]) << 8) | uint32_t(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::vector<uint8_t> out;
    for (uint32_t h : { h0, h1, h2, h3, h4 }) {
        out.push_back(uint8_t(h >> 24)); out.push_back(uint8_t(h >> 16));
        out.push_back(uint8_t(h >> 8)); out.push_back(uint8_t(h));
    }
    return out;
}

std::vector<uint8_t> sha256(const uint8_t *data, size_t len)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                       0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };

    const std::vector<uint8_t> msg = md_pad_be64len(data, len);

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(msg[chunk + i * 4]) << 24) | (uint32_t(msg[chunk + i * 4 + 1]) << 16)
                    | (uint32_t(msg[chunk + i * 4 + 2]) << 8) | uint32_t(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            hh = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::vector<uint8_t> out;
    for (uint32_t x : h) {
        out.push_back(uint8_t(x >> 24)); out.push_back(uint8_t(x >> 16));
        out.push_back(uint8_t(x >> 8)); out.push_back(uint8_t(x));
    }
    return out;
}

std::vector<uint8_t> hmacSha1(const std::vector<uint8_t> &key, const uint8_t *data, size_t len)
{
    constexpr size_t kBlockSize = 64;
    std::vector<uint8_t> k = key;
    if (k.size() > kBlockSize)
        k = sha1(k.data(), k.size());
    k.resize(kBlockSize, 0x00);

    std::vector<uint8_t> ipad(kBlockSize), opad(kBlockSize);
    for (size_t i = 0; i < kBlockSize; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    std::vector<uint8_t> inner = ipad;
    inner.insert(inner.end(), data, data + len);
    const std::vector<uint8_t> innerHash = sha1(inner.data(), inner.size());

    std::vector<uint8_t> outer = opad;
    outer.insert(outer.end(), innerHash.begin(), innerHash.end());
    return sha1(outer.data(), outer.size());
}

std::vector<uint8_t> pbkdf2HmacSha1(const std::vector<uint8_t> &password,
                                     const std::vector<uint8_t> &salt, uint32_t iterations,
                                     size_t dkLen)
{
    constexpr size_t kHashLen = 20; // SHA-1 output size
    std::vector<uint8_t> out;
    out.reserve(dkLen);

    uint32_t blockIndex = 1;
    while (out.size() < dkLen) {
        std::vector<uint8_t> saltBlock = salt;
        saltBlock.push_back(uint8_t(blockIndex >> 24));
        saltBlock.push_back(uint8_t(blockIndex >> 16));
        saltBlock.push_back(uint8_t(blockIndex >> 8));
        saltBlock.push_back(uint8_t(blockIndex));

        std::vector<uint8_t> u = hmacSha1(password, saltBlock.data(), saltBlock.size());
        std::vector<uint8_t> t = u;
        for (uint32_t iter = 1; iter < iterations; ++iter) {
            u = hmacSha1(password, u.data(), u.size());
            for (size_t i = 0; i < kHashLen; ++i)
                t[i] ^= u[i];
        }
        out.insert(out.end(), t.begin(), t.end());
        ++blockIndex;
    }
    out.resize(dkLen);
    return out;
}

namespace {

const char kB64Std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64EncodeWithAlphabet(const std::vector<uint8_t> &data, const char *alphabet,
                                      bool pad)
{
    std::string out;
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(alphabet[(n >> 18) & 0x3F]);
        out.push_back(alphabet[(n >> 12) & 0x3F]);
        out.push_back(alphabet[(n >> 6) & 0x3F]);
        out.push_back(alphabet[n & 0x3F]);
    }
    const size_t rem = data.size() - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(alphabet[(n >> 18) & 0x3F]);
        out.push_back(alphabet[(n >> 12) & 0x3F]);
        if (pad) { out.push_back('='); out.push_back('='); }
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(alphabet[(n >> 18) & 0x3F]);
        out.push_back(alphabet[(n >> 12) & 0x3F]);
        out.push_back(alphabet[(n >> 6) & 0x3F]);
        if (pad) out.push_back('=');
    }
    return out;
}

int b64DecodeChar(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string base64Encode(const std::vector<uint8_t> &data)
{
    return base64EncodeWithAlphabet(data, kB64Std, true);
}

std::string base64UrlNoPad(const std::vector<uint8_t> &data)
{
    return base64EncodeWithAlphabet(data, kB64Url, false);
}

std::vector<uint8_t> base64Decode(const std::string &data)
{
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : data) {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        const int v = b64DecodeChar(c);
        if (v < 0)
            throw std::runtime_error("base64Decode: invalid character");
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string utf8Replace(const std::vector<uint8_t> &bytes)
{
    std::string out;
    size_t i = 0;
    const size_t n = bytes.size();
    while (i < n) {
        const uint8_t b0 = bytes[i];
        int extra = 0;
        uint32_t cp = 0;
        size_t minCp = 0;
        if (b0 < 0x80) { out.push_back(char(b0)); ++i; continue; }
        else if ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; minCp = 0x80; }
        else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; minCp = 0x800; }
        else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; minCp = 0x10000; }
        else { out += "\xEF\xBF\xBD"; ++i; continue; } // invalid lead byte

        if (i + extra >= n + 1 && i + 1 + extra > n) {
            out += "\xEF\xBF\xBD"; ++i; continue; // truncated sequence
        }
        bool ok = true;
        uint32_t tmp = cp;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= n || (bytes[i + k] & 0xC0) != 0x80) { ok = false; break; }
            tmp = (tmp << 6) | (bytes[i + k] & 0x3F);
        }
        if (!ok || tmp < minCp || (tmp >= 0xD800 && tmp <= 0xDFFF) || tmp > 0x10FFFF) {
            out += "\xEF\xBF\xBD"; ++i; continue;
        }
        // Valid sequence - copy the original bytes through unchanged.
        for (int k = 0; k <= extra; ++k)
            out.push_back(char(bytes[i + k]));
        i += extra + 1;
    }
    return out;
}

namespace {

const std::string kAppVersionCode = "6008013";
const std::string kPackageName = "com.stt.android.suunto";

const std::string kLoginKeyPart1 = "FBkubDYmN28bWVQLLTsWFxcmaRB";
const std::string kLoginKeyPart2 = "fN2AqIBc/IRAoNgshbxgnOGUVGlU3LC0xL0AuXXXXMXY";
const std::string kLoginKeyPart3 = "RWQ4zIi0PWz4hekc1QGNTPlciNhEKV1teYSIkDGYY";

const std::string kTotpKeyPart1 = "FBkubDYmN28bWVQLLTsWWhI+NAtILCNlPQc5Y";
const std::string kTotpKeyPart2 =
        "BgiMRYjKA99Jj4HHFIqLmomOFttBQchNzcZU0QrODcDWz4hekc1QGNTPlciNhEKGl5GPDkzFyVX";
const std::string kTotpObfuscationKey = "Bh8nsTyCeC0Ql2drMen78awk84AE3ZxW";

std::vector<uint8_t> stringToBytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Byte-wise XOR of s's bytes against the repeating bytes of pkg - ports
// com.stt.android.billing.KeyObfuscator.a().
std::vector<uint8_t> xorObfuscate(const std::string &s, const std::string &pkg)
{
    std::vector<uint8_t> out(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = uint8_t(s[i]) ^ uint8_t(pkg[i % pkg.size()]);
    return out;
}

std::string deriveObfuscatedSecret(const std::vector<std::string> &parts, const std::string &pkg)
{
    std::string joined;
    for (const auto &p : parts)
        joined += p;
    const std::vector<uint8_t> raw = base64Decode(joined);
    const std::string mid = utf8Replace(raw);
    const std::vector<uint8_t> xored = xorObfuscate(mid, pkg);
    return utf8Replace(xored);
}

} // namespace

std::string deriveLoginSecret()
{
    return deriveObfuscatedSecret({ kLoginKeyPart1, kLoginKeyPart2, kLoginKeyPart3 }, kPackageName);
}

std::string deriveTotpMasterSecret()
{
    return deriveObfuscatedSecret({ kTotpKeyPart1, kTotpKeyPart2 }, kTotpObfuscationKey);
}

std::string signParams(const std::string &path, const std::vector<Param> &params)
{
    std::string sb = "POST&" + path;
    for (const auto &p : params)
        sb += "&" + p.first + "=" + p.second;
    sb += "&secret=" + deriveLoginSecret();
    const std::vector<uint8_t> sum = sha256(reinterpret_cast<const uint8_t *>(sb.data()), sb.size());
    return base64UrlNoPad(sum);
}

namespace {

// Mirrors Java PBEKeySpec semantics: only the low 8 bits of each UTF-16/rune
// "char" of the master secret are used as the password bytes - i.e. this is
// NOT the raw UTF-8 byte sequence when the secret contains multi-byte runes
// (notably the U+FFFD replacement characters the obfuscation step can
// introduce, whose low byte is 0xFD). Must decode master as a rune sequence
// first, matching Go's `for _, r := range master`.
std::vector<uint8_t> pbkdf2PasswordFromMasterSecret(const std::string &master)
{
    std::vector<uint8_t> pwd;
    size_t i = 0;
    while (i < master.size()) {
        const uint8_t b0 = uint8_t(master[i]);
        uint32_t cp;
        int extra;
        if (b0 < 0x80) { cp = b0; extra = 0; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
        else { cp = b0 & 0x07; extra = 3; }
        for (int k = 1; k <= extra && i + k < master.size(); ++k)
            cp = (cp << 6) | (uint8_t(master[i + k]) & 0x3F);
        pwd.push_back(uint8_t(cp & 0xFF));
        i += extra + 1;
    }
    return pwd;
}

std::string hotp6(const std::vector<uint8_t> &key, uint64_t counter)
{
    uint8_t buf[8];
    for (int i = 0; i < 8; ++i)
        buf[7 - i] = uint8_t((counter >> (i * 8)) & 0xFF);
    const std::vector<uint8_t> mac = hmacSha1(key, buf, 8);
    const uint8_t off = mac[mac.size() - 1] & 0x0F;
    const uint32_t code = ((uint32_t(mac[off]) & 0x7F) << 24) | (uint32_t(mac[off + 1]) << 16)
            | (uint32_t(mac[off + 2]) << 8) | uint32_t(mac[off + 3]);
    char digits[7];
    std::snprintf(digits, sizeof(digits), "%06u", code % 1000000u);
    return std::string(digits);
}

} // namespace

std::vector<uint8_t> pbkdf2KeyForSalt(const std::string &salt)
{
    const std::string master = deriveTotpMasterSecret();
    const std::vector<uint8_t> pwd = pbkdf2PasswordFromMasterSecret(master);
    const std::vector<uint8_t> saltBytes = stringToBytes(salt);
    return pbkdf2HmacSha1(pwd, saltBytes, 100, 32);
}

std::string generateTotp(const std::string &salt, int64_t nowMs)
{
    const std::vector<uint8_t> key = pbkdf2KeyForSalt(salt);
    const uint64_t counter = uint64_t(nowMs) / 30000ULL;
    return hotp6(key, counter);
}

} // namespace SuuntoAuth
