#include <doctest/doctest.h>

#include "ftcore/FlagNumber.h"

#include <array>
#include <string_view>

namespace fn = ftcore::flagnum;

TEST_CASE("flag number: a whole finite number is read, anything else is bad") {
    const std::string_view line = "-Mesh=/Game/X -Max=21 -AnimDriveLow=0.4 -AnimDriveHigh=abc -Pin= -Drag=+0.5 -Lift=1e-2";
    const fn::Value max = fn::read(line, "Max=");
    CHECK(max.read == fn::Read::Number);
    CHECK(max.value == doctest::Approx(21.f));
    CHECK(fn::read(line, "AnimDriveLow=").value == doctest::Approx(0.4f));
    CHECK(fn::read(line, "AnimDriveHigh=").read == fn::Read::Bad);
    CHECK(fn::read(line, "AnimDriveHigh=").text == "abc");
    CHECK(fn::read(line, "Pin=").read == fn::Read::Bad); // empty, followed by another flag
    CHECK(fn::read(line, "Pin=").text.empty());
    CHECK(fn::read(line, "Drag=").value == doctest::Approx(0.5f));
    CHECK(fn::read(line, "Lift=").value == doctest::Approx(0.01f));
    CHECK(fn::read(line, "Bend=").read == fn::Read::Absent);
    // Trailing junk, a lone sign, a dot, inf and nan are not numbers.
    for (const std::string_view bad : {"-Max=21cm", "-Max=-", "-Max=+", "-Max=.", "-Max=inf", "-Max=nan", "-Max=1,5"}) {
        CHECK_MESSAGE(fn::read(bad, "Max=").read == fn::Read::Bad, bad);
    }
    CHECK(fn::read("-Max=-3", "Max=").value == doctest::Approx(-3.f)); // range is the caller's check
    CHECK(fn::read("-Max=.5", "Max=").value == doctest::Approx(0.5f));
    CHECK(fn::read("-Max=21", "Max=").read == fn::Read::Number); // value at the very end
    CHECK(fn::read("-Max=", "Max=").read == fn::Read::Bad);
}

TEST_CASE("flag number: the key needs a clean left edge, as FParse::Value finds it") {
    // Damping= never reads LocalDamping='s value, in either order.
    CHECK(fn::read("-LocalDamping=abc -Damping=0.2", "Damping=").value == doctest::Approx(0.2f));
    CHECK(fn::read("-LocalDamping=abc -Damping=0.2", "Damping=").read == fn::Read::Number);
    CHECK(fn::read("-LocalDamping=0.3", "Damping=").read == fn::Read::Absent);
    CHECK(fn::read("-Damping=0.2 -LocalDamping=0.3", "LocalDamping=").value == doctest::Approx(0.3f));
    CHECK(fn::read("-AnimDriveDamping=0.5", "Damping=").read == fn::Read::Absent);
    // Case-insensitive, like FParse.
    CHECK(fn::read("-max=4", "Max=").value == doctest::Approx(4.f));
    CHECK(fn::findKey("Max=1", "Max=") == 0);
    CHECK(fn::findKey("-xMax=1", "Max=") == std::string_view::npos);
    CHECK(fn::findKey("-Max=1", "") == std::string_view::npos);
    static_assert(fn::findKey("-Pin=3", "Pin=") == 1);
}

TEST_CASE("flag number: firstBad names the first flag that is given but not a number") {
    const std::array<std::string_view, 4> keys{"Max=", "Pin=", "AnimDriveLow=", "AnimDriveHigh="};
    CHECK(fn::firstBad("-Max=21 -AnimDriveLow=0.4 -AnimDriveHigh=0.6", keys.data(), keys.size()) == -1);
    CHECK(fn::firstBad("-Max=21 -AnimDriveLow= -AnimDriveHigh=0.6", keys.data(), keys.size()) == 2);
    CHECK(fn::firstBad("-Max=21 -AnimDriveHigh=abc", keys.data(), keys.size()) == 3);
    CHECK(fn::firstBad("-Mesh=/Game/X", keys.data(), keys.size()) == -1); // none given
    CHECK(fn::firstBad("-Max=x", nullptr, 4) == -1);
}
