#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace usb_reports {

inline constexpr uint8_t hidpp_long = 0x11;
inline constexpr uint8_t hidpp_short = 0x10;
inline constexpr std::array<uint8_t, 2> hyperx_battery_request{0x66, 0x89};

enum class HidppReply { unrelated, error, response };

constexpr HidppReply hidpp_reply(std::span<const uint8_t> report, uint8_t slot,
                                uint8_t feature, uint8_t function) {
    if (report.size() < 7 || (report[0] != hidpp_long && report[0] != hidpp_short))
        return HidppReply::unrelated;
    if (report[1] != slot && !(slot == 0xff && report[1] == 0))
        return HidppReply::unrelated;
    if ((report[2] == 0xff || (report[0] == hidpp_short && report[2] == 0x8f))
        && report[3] == feature && report[4] == function)
        return HidppReply::error;
    if (report[2] == feature && report[3] == function) return HidppReply::response;
    return HidppReply::unrelated;
}

constexpr std::optional<unsigned> percentage(uint8_t value) {
    if (value <= 100) return value;
    return std::nullopt;
}

constexpr std::optional<unsigned> hyperx_percentage(std::span<const uint8_t> report) {
    if (report.size() < 5 || report[0] != hyperx_battery_request[0]
        || (report[1] != hyperx_battery_request[1] && report[1] != 0x0d)
        || (report[2] == 0 && report[3] == 0))
        return std::nullopt;
    return percentage(report[4]);
}

} // namespace usb_reports
