#pragma once

#include <cstdint>
#include <string>

// Parses the one ISO 8601 shape Suunto's health API emits:
//
//     2026-09-21T22:54:00.000+03:00
//
// Qt has QDateTime::fromString(Qt::ISODate), but its handling of fractional
// seconds and of the ":" inside the zone offset has moved across Qt
// versions, and Sailfish OS ships a Qt 5.6-era library - a mismatch this
// project has already been bitten by three times (currentSecsSinceEpoch,
// QRandomGenerator, Column::topPadding). Parsing the fixed shape by hand is
// a dozen lines, is Qt-free, and can be tested here against the real
// captured strings rather than against a build on the phone.
//
// Tolerant of the variations seen in the real capture: the fractional part
// may be absent or any number of digits, and the zone may be "Z",
// "+HH:MM", "-HH:MM" or "+HHMM".
namespace Iso8601 {

// Milliseconds since the Unix epoch, UTC. Returns false rather than
// guessing if the string isn't the expected shape - a health entry with an
// unparseable timestamp should be skipped, not silently placed in 1970
// (which is exactly the failure this project already hit once with the
// watch's own local64 clock).
bool parseToUnixMs(const std::string &text, int64_t *outMs);

// The inverse, for the upload side (samples.json's TimeISO8601 carries a
// local time plus its offset, so the offset has to be passed in rather than
// assumed): formats as YYYY-MM-DDTHH:MM:SS.mmm+HH:MM.
std::string formatLocal(int64_t unixMs, int offsetMinutes);

} // namespace Iso8601
