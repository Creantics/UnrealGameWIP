#include <doctest/doctest.h>
#include "ftcore/BenchPath.h"

#include <string>

TEST_CASE("bench path: parses keys, skips comments and blank lines") {
    ftcore::BenchPath path;
    const char* text =
        "# day street loop\n"
        "\n"
        "0   0 0 170   100 0 170\n"
        "2.5 200 0 170 300 0 170  # a comment after a key\r\n"
        "5   200 200 170 200 300 170";
    REQUIRE(path.parse(text));
    CHECK(path.size() == 3);
    CHECK(path.duration() == doctest::Approx(5.0));
    CHECK(path.key(1).pose.eye[0] == doctest::Approx(200.0));
    CHECK(path.key(2).pose.at[1] == doctest::Approx(300.0));
}

TEST_CASE("bench path: passes through its keys and holds at the ends") {
    ftcore::BenchPath path;
    REQUIRE(path.parse("10 0 0 0 1 0 0\n12 100 0 0 101 0 0\n14 100 100 0 100 101 0\n"));
    CHECK(path.sample(0.0).eye[0] == doctest::Approx(0.0));
    const ftcore::BenchPose b = path.sample(2.0);
    CHECK(b.eye[0] == doctest::Approx(100.0));
    CHECK(b.eye[1] == doctest::Approx(0.0));
    CHECK(path.sample(4.0).eye[1] == doctest::Approx(100.0));
    CHECK(path.sample(-3.0).eye[0] == doctest::Approx(0.0));
    CHECK(path.sample(99.0).at[1] == doctest::Approx(101.0));
    const ftcore::BenchPose mid = path.sample(1.0);
    CHECK(mid.eye[0] > 0.0);
    CHECK(mid.eye[0] < 100.0);
}

TEST_CASE("bench path: a straight evenly timed run is sampled evenly") {
    ftcore::BenchPath path;
    REQUIRE(path.parse("0 0 0 0 0 0 0\n1 100 0 0 0 0 0\n2 200 0 0 0 0 0\n3 300 0 0 0 0 0\n"));
    CHECK(path.sample(1.5).eye[0] == doctest::Approx(150.0));
}

TEST_CASE("bench path: refuses bad text and keeps no keys") {
    ftcore::BenchPath path;
    int bad = -1;
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\n1 1 1 1 1 1\n", &bad)); // six numbers
    CHECK(bad == 2);
    CHECK(path.size() == 0);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\n0 1 1 1 1 1 1\n", &bad)); // time does not rise
    CHECK(bad == 2);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0 x\n1 1 1 1 1 1 1\n", &bad)); // trailing word
    CHECK(bad == 1);
    CHECK_FALSE(path.parse("# only a comment\n0 0 0 0 0 0 0\n", &bad)); // one key
    CHECK(bad == 0);
    CHECK_FALSE(path.parse("", &bad));
}

TEST_CASE("bench path: refuses nan and inf in any value") {
    ftcore::BenchPath path;
    int bad = -1;
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\n1 nan 0 0 0 0 0\n", &bad));
    CHECK(bad == 2);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 inf\n1 0 0 0 0 0 0\n", &bad));
    CHECK(bad == 1);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\ninf 0 0 0 0 0 0\n", &bad));
    CHECK(bad == 2);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\n1 0 0 -inf 0 0 0\n", &bad));
    CHECK(bad == 2);
    CHECK_FALSE(path.parse("0 0 0 0 0 0 0\n1 1e999 0 0 0 0 0\n", &bad)); // out of range
    CHECK(bad == 2);
    CHECK(path.size() == 0);
}

TEST_CASE("bench path: takes kMaxKeys keys and refuses one more") {
    std::string text;
    for (std::size_t i = 0; i < ftcore::BenchPath::kMaxKeys; ++i) {
        text += std::to_string(i) + " 0 0 0 1 0 0\n";
    }
    ftcore::BenchPath path;
    REQUIRE(path.parse(text));
    CHECK(path.size() == ftcore::BenchPath::kMaxKeys);
    text += std::to_string(ftcore::BenchPath::kMaxKeys) + " 0 0 0 1 0 0\n";
    int bad = -1;
    CHECK_FALSE(path.parse(text, &bad));
    CHECK(bad == static_cast<int>(ftcore::BenchPath::kMaxKeys) + 1);
    CHECK(path.size() == 0);
}
