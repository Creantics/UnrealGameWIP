#include "ftcore/StillCam.h"

#include "CsvRead.h"

#include <cctype>

namespace ftcore {

StillCam parseStillCam(std::string_view text)
{
    double v[6] = {};
    std::size_t count = 0;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        if (count == 6 || !detail::parseNumber(part, v[count])) {
            return {};
        }
        ++count;
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (count != 6 || v[4] < -89.0 || v[4] > 89.0 || v[5] < 10.0 || v[5] > 150.0) {
        return {};
    }
    return {true, v[0], v[1], v[2], v[3], v[4], v[5]};
}

WeatherForce parseWeatherForce(std::string_view text, bool* known)
{
    text = detail::trim(text);
    bool dry = text.size() == 3;
    for (std::size_t i = 0; dry && i < 3; ++i) {
        dry = std::tolower(static_cast<unsigned char>(text[i])) == "dry"[i];
    }
    if (known) {
        *known = dry || text.empty();
    }
    return dry ? WeatherForce::dry : WeatherForce::none;
}

std::string_view dryLookFor(std::string_view look)
{
    constexpr std::string_view kRain = "_rain";
    if (look.size() > kRain.size() && look.substr(look.size() - kRain.size()) == kRain) {
        look.remove_suffix(kRain.size());
    }
    return look;
}

} // namespace ftcore
