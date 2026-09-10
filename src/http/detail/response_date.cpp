#include "sparenode/http/detail/response_date.hpp"

#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace sparenode::http::detail
{
std::string format_response_date(const std::chrono::system_clock::time_point instant)
{
    const auto date = std::chrono::floor<std::chrono::days>(instant);
    const std::chrono::year_month_day calendar{date};
    const int year = static_cast<int>(calendar.year());
    if (year < 0 || year > 9999)
    {
        throw std::out_of_range("HTTP date requires a four-digit year");
    }
    constexpr std::array<std::string_view, 7> weekdays{"Sun", "Mon", "Tue", "Wed",
                                                       "Thu", "Fri", "Sat"};
    constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::chrono::hh_mm_ss time{std::chrono::floor<std::chrono::seconds>(instant - date)};
    std::ostringstream result;
    result.imbue(std::locale::classic());
    result << weekdays[std::chrono::weekday{date}.c_encoding()] << ", " << std::setfill('0')
           << std::setw(2) << static_cast<unsigned int>(calendar.day()) << ' '
           << months[static_cast<unsigned int>(calendar.month()) - 1] << ' ' << std::setw(4) << year
           << ' ' << std::setw(2) << time.hours().count() << ':' << std::setw(2)
           << time.minutes().count() << ':' << std::setw(2) << time.seconds().count() << " GMT";
    return result.str();
}
} // namespace sparenode::http::detail
