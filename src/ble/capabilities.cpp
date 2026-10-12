#include "capabilities.h"

#include <algorithm>

namespace Capabilities {
namespace {

// The paged reply's fixed header, as documented in mdswirecodec.h.
constexpr size_t kPagedHeaderSize = 19;
constexpr size_t kItemCountOffset = 14;

bool looksLikeCapability(const std::string &text)
{
    // feat_zappwf_v5, plugin_maxzappsize_2, hw_display_amoledr466,
    // feat_simvariant_Tianjin - letters, digits and underscores, and the
    // variant name is capitalised, which is why case is not restricted.
    if (text.size() < 4)
        return false;
    const char first = text[0];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')))
        return false;
    for (char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace

size_t itemsInPage(const std::vector<uint8_t> &pagedBody)
{
    if (pagedBody.size() < kPagedHeaderSize)
        return 0;
    return static_cast<size_t>(pagedBody[kItemCountOffset])
            | (static_cast<size_t>(pagedBody[kItemCountOffset + 1]) << 8);
}

std::vector<std::string> parse(const std::vector<uint8_t> &payload)
{
    std::vector<std::string> found;
    std::string current;
    for (uint8_t byte : payload) {
        if (byte == 0x00) {
            if (looksLikeCapability(current))
                found.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(static_cast<char>(byte));
    }
    if (looksLikeCapability(current))
        found.push_back(current);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

std::string join(const std::vector<std::string> &capabilities)
{
    std::string out;
    for (const std::string &capability : capabilities) {
        if (!out.empty())
            out.push_back(',');
        out += capability;
    }
    return out;
}

} // namespace Capabilities
