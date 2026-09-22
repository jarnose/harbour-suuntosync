#include "iso8601.h"

#include <cstdio>
#include <cstdlib>

namespace Iso8601 {

namespace {

// Days from 1970-01-01 to the given civil date, proleptic Gregorian.
// Howard Hinnant's days_from_civil, which is exact for the whole range and
// avoids timegm() (not portable) and QDate (not Qt-free).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The inverse, civil_from_days.
void civilFromDays(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = static_cast<int>(yy + (*m <= 2));
}

bool digits(const std::string &s, size_t pos, size_t n, int *out)
{
    if (pos + n > s.size())
        return false;
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

} // namespace

bool parseToUnixMs(const std::string &text, int64_t *outMs)
{
    // YYYY-MM-DDTHH:MM:SS is the fixed part; everything after is optional.
    int year, month, day, hour, minute, second;
    if (text.size() < 19)
        return false;
    if (!digits(text, 0, 4, &year) || text[4] != '-'
            || !digits(text, 5, 2, &month) || text[7] != '-'
            || !digits(text, 8, 2, &day)
            || (text[10] != 'T' && text[10] != ' ')
            || !digits(text, 11, 2, &hour) || text[13] != ':'
            || !digits(text, 14, 2, &minute) || text[16] != ':'
            || !digits(text, 17, 2, &second)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23
            || minute > 59 || second > 60) {
        return false; // 60 allowed: a leap second lands on the next minute
    }

    size_t pos = 19;
    int millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int scale = 100;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (scale > 0) {
                millis += (text[pos] - '0') * scale;
                scale /= 10;
            }
            ++pos; // extra digits beyond milliseconds are dropped, not an error
        }
    }

    int offsetMinutes = 0;
    if (pos < text.size()) {
        const char z = text[pos];
        if (z == 'Z' || z == 'z') {
            ++pos;
        } else if (z == '+' || z == '-') {
            ++pos;
            int oh, om;
            if (!digits(text, pos, 2, &oh))
                return false;
            pos += 2;
            if (pos < text.size() && text[pos] == ':')
                ++pos;
            if (!digits(text, pos, 2, &om))
                return false;
            pos += 2;
            offsetMinutes = (oh * 60 + om) * (z == '-' ? -1 : 1);
        } else {
            return false; // trailing junk: reject rather than guess
        }
    }
    if (pos != text.size())
        return false;

    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month),
                                        static_cast<unsigned>(day));
    const int64_t localSeconds = days * 86400 + hour * 3600 + minute * 60 + second;
    *outMs = (localSeconds - offsetMinutes * 60) * 1000 + millis;
    return true;
}

std::string formatLocal(int64_t unixMs, int offsetMinutes)
{
    const int64_t localMs = unixMs + static_cast<int64_t>(offsetMinutes) * 60 * 1000;
    // Floor division, so times before 1970 don't round the wrong way.
    int64_t seconds = localMs / 1000;
    int millis = static_cast<int>(localMs % 1000);
    if (millis < 0) {
        millis += 1000;
        --seconds;
    }
    int64_t days = seconds / 86400;
    int rem = static_cast<int>(seconds % 86400);
    if (rem < 0) {
        rem += 86400;
        --days;
    }

    int year;
    unsigned month, day;
    civilFromDays(days, &year, &month, &day);

    const int offsetAbs = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d.%03d%c%02d:%02d",
                   year, month, day, rem / 3600, (rem % 3600) / 60, rem % 60, millis,
                   offsetMinutes < 0 ? '-' : '+', offsetAbs / 60, offsetAbs % 60);
    return std::string(buf);
}

} // namespace Iso8601
