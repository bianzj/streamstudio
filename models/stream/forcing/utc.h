// ==========================================================================
// 文件：utc.h
// 功能：UTC 日期时间与 Unix 秒数互转、ISO-8601 文本解析/格式化的声明（实现见 utc.cpp）
// 主要内容：
//   1. daysFromCivil / civilFromDays / isLeapYear：公历日期与天数换算
//   2. UtcTime 结构与 utcFromSeconds / secondsFromUtc
//   3. parseUtc / formatUtc / compactUtc：文本接口
// 约定：全部为 UTC；秒数是相对 1970-01-01T00:00:00Z 的 double；不处理闰秒
// ==========================================================================

#pragma once
#include <cstdint>
#include <string>

namespace stream {

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's algorithm).
int64_t daysFromCivil(int year, unsigned month, unsigned day);
void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day);

// 结构：分解后的 UTC 时间；month 1-12，day 1-31，hour 0-23，minute 0-59，second 可含小数。
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
