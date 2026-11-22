#include <doctest/doctest.h>

#include "ftcore/FlagNumber.h"

#include <array>
#include <ostream>
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
    // Empty and followed by another flag: FParse skips the space and reads the next flag as the value (Atof: 0).
    CHECK(fn::read(line, "Pin=").read == fn::Read::Bad);
    CHECK(fn::read(line, "Pin=").text == "-Drag=+0.5");
    CHECK(fn::read(line, "Drag=").value == doctest::Approx(0.5f));
    CHECK(fn::read(line, "Lift=").value == doctest::Approx(0.01f));
    CHECK(fn::read(line, "Bend=").read == fn::Read::Absent);
    // Trailing junk, a lone sign, a sign after '+', a dot, inf and nan are not numbers; nor is a value FParse would cut
    // short at a comma or a ')' (it reads "1,5" as 1).
    for (const std::string_view bad :
         {"-Max=21cm", "-Max=-", "-Max=+", "-Max=+-5", "-Max=++5", "-Max=.", "-Max=inf", "-Max=nan", "-Max=1,5", "-Max=2)"}) {
        CHECK_MESSAGE(fn::read(bad, "Max=").read == fn::Read::Bad, bad);
    }
    CHECK(fn::read("-Max=-3", "Max=").value == doctest::Approx(-3.f)); // range is the caller's check
    CHECK(fn::read("-Max=.5", "Max=").value == doctest::Approx(0.5f));
    CHECK(fn::read("-Max=+.5", "Max=").value == doctest::Approx(0.5f));
    CHECK(fn::read("-Max=21", "Max=").read == fn::Read::Number); // value at the very end
    CHECK(fn::read("-Max=", "Max=").read == fn::Read::Bad);
    CHECK(fn::read("-Max=21 -Pin=3", "Max=").value == doctest::Approx(21.f));
    CHECK(fn::read("-Max=21\t-Pin=3", "Max=").value == doctest::Approx(21.f));
}

TEST_CASE("flag number: quotes and spaces are read as FParse::Value reads them") {
    // A quoted value runs to its closing quote; it must close and be followed by a space or the end.
    const fn::Value quoted = fn::read("-Max=\"21\" -Pin=3", "Max=");
    CHECK(quoted.read == fn::Read::Number);
    CHECK(quoted.value == doctest::Approx(21.f));
    CHECK(quoted.text == "21");
    CHECK(fn::read("-Max=\"21\"", "Max=").value == doctest::Approx(21.f));
    CHECK(fn::read("-Max=\"abc\"", "Max=").read == fn::Read::Bad);
    CHECK(fn::read("-Max=\"21", "Max=").read == fn::Read::Bad);   // never closed
    CHECK(fn::read("-Max=\"21\"x", "Max=").read == fn::Read::Bad); // FParse would drop the x
    CHECK(fn::read("-Max=\"\"", "Max=").read == fn::Read::Bad);
    CHECK(fn::read("-Max=\"2 1\"", "Max=").read == fn::Read::Bad);
    // Spaces after the '=' are skipped, as FParse does.
    CHECK(fn::read("-Max= 21", "Max=").value == doctest::Approx(21.f));
    // A key inside a quoted stretch is not the flag: FParse skips quoted text when it looks for the key.
    CHECK(fn::read("\"-Max=21\"", "Max=").read == fn::Read::Absent);
    CHECK(fn::read("-Mesh=\"/Game/Max=abc\" -Max=4", "Max=").value == doctest::Approx(4.f));
    CHECK(fn::findKey("-Mesh=\"/Game/Max=abc\"", "Max=") == std::string_view::npos);
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

TEST_CASE("flag number: firstBad names the first flag that is given but not a number; listed ties a key to the list") {
    const std::array<std::string_view, 4> keys{"Max=", "Pin=", "AnimDriveLow=", "AnimDriveHigh="};
    CHECK(fn::firstBad("-Max=21 -AnimDriveLow=0.4 -AnimDriveHigh=0.6", keys.data(), keys.size()) == -1);
    CHECK(fn::firstBad("-Max=21 -AnimDriveLow= -AnimDriveHigh=0.6", keys.data(), keys.size()) == 2);
    CHECK(fn::firstBad("-Max=21 -AnimDriveHigh=abc", keys.data(), keys.size()) == 3);
    CHECK(fn::firstBad("-Mesh=/Game/X", keys.data(), keys.size()) == -1); // none given
    CHECK(fn::firstBad("-Max=x", nullptr, 4) == -1);
    static constexpr std::array<std::string_view, 2> known{"Max=", "AnimDriveLow="};
    static_assert(fn::listed("Max=", known.data(), known.size()));
    static_assert(fn::listed("animdrivelow=", known.data(), known.size()));
    static_assert(!fn::listed("Ma=", known.data(), known.size()));
    static_assert(!fn::listed("AnimDriveHigh=", known.data(), known.size()));
    static_assert(!fn::listed("Max=", nullptr, 2));
}
