#include "ftcore/ClothDrive.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string_view>

namespace cd = ftcore::clothdrive;

TEST_CASE("cloth drive: full down to `from`, easing to 0 at `to`, by the mask level") {
    CHECK(cd::weight(0.f, cd::kFrom, cd::kTo) == 1.f);   // pinned rows
    CHECK(cd::weight(0.04f, cd::kFrom, cd::kTo) == 1.f); // row 4 of the Third's cape
    CHECK(cd::weight(0.2f, cd::kFrom, cd::kTo) == 1.f);  // row 7: the last full row
    CHECK(cd::weight(0.6f, cd::kFrom, cd::kTo) == doctest::Approx(0.5f));
    CHECK(cd::weight(1.f, cd::kFrom, cd::kTo) == 0.f);   // the hem
    CHECK(cd::weight(std::nanf(""), cd::kFrom, cd::kTo) == 1.f);
    CHECK(cd::weight(2.f, cd::kFrom, cd::kTo) == 0.f);
    CHECK(cd::weight(-1.f, cd::kFrom, cd::kTo) == 1.f);
    CHECK(cd::weight(0.5f, 0.f, 0.4f) == 0.f);           // beyond `to`
    float last = 2.f;
    for (int i = 0; i <= 100; ++i) {                     // never rises toward the hem
        const float w = cd::weight(static_cast<float>(i) / 100.f, cd::kFrom, cd::kTo);
        CHECK(w <= last);
        CHECK(w >= 0.f);
        last = w;
    }
}

TEST_CASE("cloth drive: the stiffness Chaos gives a weight, and its pull per step") {
    CHECK(cd::stiffness(0.4f, 0.6f, 1.f) == doctest::Approx(0.6f));
    CHECK(cd::stiffness(0.4f, 0.6f, 0.f) == doctest::Approx(0.4f));
    CHECK(cd::stiffness(0.4f, 0.6f, 0.5f) == doctest::Approx(0.5f));
    CHECK(cd::pullPerStep(0.f) == 0.f);
    CHECK(cd::pullPerStep(1.f) == doctest::Approx(1.f));
    CHECK(cd::pullPerStep(0.5f) == doctest::Approx(0.0307f).epsilon(0.01));
    CHECK(cd::pullPerStep(0.6f) == doctest::Approx(0.0621f).epsilon(0.01));
    CHECK(cd::pullPerStep(0.4f) == doctest::Approx(0.0149f).epsilon(0.01));
}

TEST_CASE("cloth drive: the flags are checked before the mesh changes") {
    cd::Args a;
    CHECK(cd::validate(a) == cd::Check::Ok); // nothing asked
    a.map = true;
    a.max = 21.f;
    CHECK(cd::validate(a) == cd::Check::MapWithoutRange);
    a.hasHigh = true;
    a.high = 0.6f;
    CHECK(cd::validate(a) == cd::Check::MapWithoutRange); // Low too
    a.hasLow = true;
    a.low = 0.4f;
    CHECK(cd::validate(a) == cd::Check::Ok);
    a.low = 0.7f;
    CHECK(cd::validate(a) == cd::Check::LowAboveHigh);
    a.low = 0.4f;
    a.high = 1.5f;
    CHECK(cd::validate(a) == cd::Check::RangeOutOfUnit);
    a.high = std::nanf("");
    CHECK(cd::validate(a) == cd::Check::RangeOutOfUnit);
    a.high = 0.6f;
    a.hasFrom = true;
    a.from = 0.5f;
    a.hasTo = true;
    a.to = 0.5f;
    CHECK(cd::validate(a) == cd::Check::GradeOrder);
    a.to = 1.2f;
    CHECK(cd::validate(a) == cd::Check::GradeOutOfUnit);
    a.to = 1.f;
    CHECK(cd::validate(a) == cd::Check::Ok);
    a.hasDamping = true;
    a.damping = -0.1f;
    CHECK(cd::validate(a) == cd::Check::DampingOutOfUnit);
    a.damping = 0.2f;
    a.max = 0.f;
    CHECK(cd::validate(a) == cd::Check::MaxNotPositive);
    a.max = 21.f;
    CHECK(cd::validate(a) == cd::Check::Ok);

    cd::Args uniform; // Low alone, no map: one drive for every vertex
    uniform.hasLow = true;
    uniform.low = 0.3f;
    CHECK(cd::validate(uniform) == cd::Check::Ok);
    uniform.hasHigh = true;
    uniform.high = 0.5f;
    CHECK(cd::validate(uniform) == cd::Check::HighWithoutMap);

    cd::Args grade; // a grade with no map
    grade.hasFrom = true;
    CHECK(cd::validate(grade) == cd::Check::GradeWithoutMap);
    CHECK(std::string_view(cd::describe(cd::Check::MapWithoutRange)).find("-AnimDriveMap") == 0);
}
