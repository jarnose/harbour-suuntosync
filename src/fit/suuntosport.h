#pragma once

#include <string>

// A FIT sport, as Suunto's own SML ActivityType numbers it.
//
// Two different id spaces for the same sports, joined by name rather than
// by number: cycling is 2 in FIT and 4 on the watch. The left-hand side is
// every string `Fit::sportKey()` can return - all of them, enumerated from
// that function rather than from memory - and the right-hand side is read
// off the watch's own table in docs/activity-types.md.
//
// It is here, Qt-free, because the first version of this table lived in
// AppController and was written by guessing both ends: six ids were wrong
// and six of sportKey()'s names were missing, so a real e-bike ride filed
// itself as "Unspecified sport" and nothing noticed until it had been
// uploaded. tests/test_fitsml.cpp now walks the whole vocabulary.
namespace Fit {

// 1, "Unspecified sport", for a sport with no counterpart - which is a
// real value in that vocabulary rather than a sentinel.
int suuntoActivityId(const std::string &sportKey);

} // namespace Fit
