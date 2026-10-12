#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The watch's own capability list, read from suunto://<serial>/Dev/Capabilities.
//
// It decides what the cloud will offer: both
// GET /v1/suuntoplus/features/items and POST /v1/suuntoplus/watchface/push/
// library carry it as a sorted, comma-joined `capabilities=` parameter, so
// nothing can be listed or installed before it has been read. See
// docs/suuntoplus-and-watchfaces.md.
//
// The resource is a paged read, and its paging differs from /Summary's in a
// way that matters: **the offset counts items, not bytes.** The official app
// asked for 0, 10, 20, 30, 40, and each reply's 19-byte header carries the
// number of items it returned at byte 14 - 10, 10, 10, 10, 9, which is the
// 49 capabilities this watch reports. Advancing by the payload's size
// instead would re-read the same page for ever.
//
// Qt-free, like every other decoder here, so it can be tested against a real
// reply with plain g++.
namespace Capabilities {

// How many items the page in this reply holds, read from the paged header.
// 0 if the body is too short to carry one.
size_t itemsInPage(const std::vector<uint8_t> &pagedBody);

// Every capability in a concatenated payload, sorted and deduplicated -
// which is also the order the cloud's parameter uses.
//
// The payload is an offset table followed by NUL-terminated strings, and the
// strings are taken rather than the table walked: the table's own encoding
// was not pinned down, the strings are unambiguous, and a capability list
// read two ways has to agree with the official app's parameter to be worth
// anything. It does - 49 of 49 against the captured request.
std::vector<std::string> parse(const std::vector<uint8_t> &payload);

// The `capabilities=` parameter's value, before URL encoding.
std::string join(const std::vector<std::string> &capabilities);

} // namespace Capabilities
