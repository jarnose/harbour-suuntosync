#pragma once

#include "activity.h"

#include <cstdint>
#include <vector>

// Writes a FIT activity file. The inverse of fit/activity.*, which reads
// one.
//
// Where the facts come from, since getting any of them wrong produces a
// file that opens nowhere:
//
//  - The container - the 14-byte header, the definition and data record
//    headers, the base-type bytes - is what fitdecoder.cpp already reads,
//    so it is derived from code proven against real Garmin and Suunto
//    files rather than from a specification nobody here has.
//  - The field numbers for `record`, `lap` and `session` likewise come
//    from the decoder's own read functions. Note that `session` numbers
//    ascent and descent 22 and 23 while `lap` numbers them 21 and 22 -
//    an asymmetry in the format, not a typo here.
//  - The field numbers for `file_id` and `activity`, which the decoder
//    never reads, were read out of muktihari/fit's generated profile
//    (`profile/mesgdef/file_id_gen.go`, `activity_gen.go`), where the
//    message's fields are indexed by their own numbers.
//
// A reading the activity does not have is written as the base type's
// **invalid** value rather than omitted, which is what lets every message
// of a kind share one definition record. The decoder treats those as
// absent (see `kBase` and `Message::getInt`), so a round trip gives back
// what went in - which is what tests/test_fitencoder.cpp checks.
namespace Fit {

// Empty if the activity has no samples and no summary worth writing.
std::vector<uint8_t> encodeActivity(const Activity &activity);

} // namespace Fit
