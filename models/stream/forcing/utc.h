#pragma once
#include <cstdint>
#include <string>

namespace stream {

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's algorithm).
int64_t daysFromCivil(int year, unsigned month, unsigned day);
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day);

struct UtcTime {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0;
    double second = 0;
};

UtcTime utcFromSeconds(double seconds);
double secondsFromUtc(const UtcTime& time);
// Accepts YYYY-MM-DDTHH:MM[:SS[.fff]]Z.
bool parseUtc(const std::string& text, double& seconds);
std::string formatUtc(double seconds);
// Compact form for file names: YYYYMMDDTHHMMZ.
std::string compactUtc(double seconds);
bool isLeapYear(int year);

}  // namespace stream
