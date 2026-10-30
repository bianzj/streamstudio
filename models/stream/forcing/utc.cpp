#include "utc.h"

#include <cmath>
#include <cstdio>

namespace stream {

int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& year, unsigned& month, unsigned& day)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int>(y + (month <= 2));
}

bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

UtcTime utcFromSeconds(double seconds)
{
    const double days = std::floor(seconds / 86400.0);
    double remainder = seconds - days * 86400.0;
    UtcTime time;
    unsigned month = 1, day = 1;
    civilFromDays(static_cast<int64_t>(days), time.year, month, day);
    time.month = static_cast<int>(month);
    time.day = static_cast<int>(day);
    time.hour = static_cast<int>(remainder / 3600.0);
    remainder -= time.hour * 3600.0;
    time.minute = static_cast<int>(remainder / 60.0);
    time.second = remainder - time.minute * 60.0;
    return time;
}

double secondsFromUtc(const UtcTime& time)
{
    return static_cast<double>(daysFromCivil(time.year, static_cast<unsigned>(time.month), static_cast<unsigned>(time.day))) * 86400.0 +
           time.hour * 3600.0 + time.minute * 60.0 + time.second;
}

bool parseUtc(const std::string& text, double& seconds)
{
    UtcTime time;
    char zone = 0;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d%n", &time.year, &time.month, &time.day, &time.hour, &time.minute, &consumed) != 5)
        return false;
    const char* rest = text.c_str() + consumed;
    if (*rest == ':') {
        double second = 0;
        int more = 0;
        if (std::sscanf(rest + 1, "%lf%n", &second, &more) != 1) return false;
        time.second = second;
        rest += 1 + more;
    }
    zone = *rest;
    if (zone != 'Z' || rest[1] != '\0') return false;
    if (time.year < 1900 || time.year > 2200 || time.month < 1 || time.month > 12 || time.day < 1 || time.hour < 0 || time.hour > 23 ||
        time.minute < 0 || time.minute > 59 || time.second < 0 || time.second >= 60)
        return false;
    static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int last = monthDays[time.month - 1] + (time.month == 2 && isLeapYear(time.year) ? 1 : 0);
    if (time.day > last) return false;
    seconds = secondsFromUtc(time);
    return true;
}

std::string formatUtc(double seconds)
{
    const UtcTime time = utcFromSeconds(std::round(seconds));
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", time.year, time.month, time.day, time.hour, time.minute,
                  static_cast<int>(std::round(time.second)) % 60);
    return text;
}

std::string compactUtc(double seconds)
{
    const UtcTime time = utcFromSeconds(std::round(seconds));
    char text[24];
    std::snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02dZ", time.year, time.month, time.day, time.hour, time.minute);
    return text;
}

}  // namespace stream
