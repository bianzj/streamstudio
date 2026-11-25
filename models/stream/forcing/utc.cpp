// ==========================================================================
// 文件：utc.cpp
// 功能：UTC 日期时间与“自 1970-01-01T00:00:00Z 起的秒数”之间的换算、解析与格式化
// 主要模块：
//   1. daysFromCivil / civilFromDays：公历日期 <-> 天数（Hinnant 算法，无时区、无闰秒）
//   2. isLeapYear：公历闰年判定
//   3. utcFromSeconds / secondsFromUtc：秒数 <-> UtcTime 结构
//   4. parseUtc / formatUtc / compactUtc：ISO-8601 文本解析与格式化
// 数据流：app、forcing 各模块把过境时刻、数据包时间戳等以 UTC 秒数（double）传递，
//         只在读写配置/日志/文件名时经本文件转为文本
// 关系：声明见 utc.h；被 era5.cpp、forcing_resample.cpp、station_source.cpp 等调用
// 约定：所有时间均为 UTC；秒数为相对 Unix 纪元的 double（可为负）；不处理闰秒
// ==========================================================================

#include "utc.h"

#include <cmath>
#include <cstdio>

namespace stream {

// 模块：公历日期 -> 距 1970-01-01 的天数（Hinnant 的 days_from_civil 算法）。
// 说明：把 3 月作为一年的起点（月份平移 -3/+9），使闰日落在“年末”，
//       再按 400 年一个纪元（146097 天）折算；719468 为 0000-03-01 到 1970-01-01 的天数。
// 输入：year/month(1-12)/day(1-31)，不校验合法性；输出：天数（1970-01-01 为 0）。
int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// 模块：天数 -> 公历日期，daysFromCivil 的逆运算（Hinnant 的 civil_from_days）。
// 说明：负天数用 z - 146096 做向下取整除法，保证纪元编号对 1970 年之前同样正确。
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

// 模块：公历闰年判定（能被 4 整除且不能被 100 整除，或能被 400 整除）。
bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// 模块：Unix 秒数 -> UtcTime。
// 说明：先对 86400 向下取整得到天数（负秒数也落在正确的日期），
//       余数再拆为时、分、秒；秒保留小数部分。
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

// 模块：UtcTime -> Unix 秒数（utcFromSeconds 的逆运算，不校验字段范围）。
double secondsFromUtc(const UtcTime& time)
{
    return static_cast<double>(daysFromCivil(time.year, static_cast<unsigned>(time.month), static_cast<unsigned>(time.day))) * 86400.0 +
           time.hour * 3600.0 + time.minute * 60.0 + time.second;
}

// 模块：解析 ISO-8601 UTC 文本 YYYY-MM-DDTHH:MM[:SS[.fff]]Z，成功时写出 seconds 并返回 true。
// 说明：必须以大写 Z 结尾且其后无其他字符；不接受时区偏移。
//       年份限制在 1900-2200，并按月份天数与闰年校验日期，非法则返回 false（不抛异常）。
bool parseUtc(const std::string& text, double& seconds)
{
    UtcTime time;
    char zone = 0;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d%n", &time.year, &time.month, &time.day, &time.hour, &time.minute, &consumed) != 5)
        return false;
    // 步骤：%n 记录已解析字符数；其后可选 :SS[.fff] 秒字段，最后必须恰为 'Z'。
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
    // 步骤：校验日是否超过当月天数（2 月逢闰年为 29 天）。
    static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int last = monthDays[time.month - 1] + (time.month == 2 && isLeapYear(time.year) ? 1 : 0);
    if (time.day > last) return false;
    seconds = secondsFromUtc(time);
    return true;
}

// 模块：Unix 秒数 -> YYYY-MM-DDTHH:MM:SSZ 文本（先四舍五入到整秒，小数秒不输出）。
std::string formatUtc(double seconds)
{
    const UtcTime time = utcFromSeconds(std::round(seconds));
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", time.year, time.month, time.day, time.hour, time.minute,
                  static_cast<int>(std::round(time.second)) % 60);
    return text;
}

// 模块：Unix 秒数 -> 文件名用的紧凑形式 YYYYMMDDTHHMMZ（四舍五入到整秒后只取到分钟）。
std::string compactUtc(double seconds)
{
    const UtcTime time = utcFromSeconds(std::round(seconds));
    char text[24];
    std::snprintf(text, sizeof(text), "%04d%02d%02dT%02d%02dZ", time.year, time.month, time.day, time.hour, time.minute);
    return text;
}

}  // namespace stream
