// The clip import's and the pose stills' plain parts: arguments, an FBX file's bone names, the import checks.

#include "ftcore/AnimImport.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace ftcore::animimport;

namespace {

void PutString(std::vector<std::uint8_t>& out, const std::string& s)
{
    out.push_back('S');
    const auto n = static_cast<std::uint32_t>(s.size());
    for (int b = 0; b < 4; ++b) {
        out.push_back(static_cast<std::uint8_t>((n >> (8 * b)) & 0xff));
    }
    out.insert(out.end(), s.begin(), s.end());
}

void PutModel(std::vector<std::uint8_t>& out, const std::string& name, const std::string& type)
{
    out.push_back('L');
    for (int b = 0; b < 8; ++b) {
        out.push_back(static_cast<std::uint8_t>(b + 1));
    }
    PutString(out, name + std::string("\0\x01Model", 7));
    PutString(out, type);
}

} // namespace

TEST_CASE("socket specs: bone-local and component-space")
{
    SocketSpec s;
    std::string err;
    REQUIRE(ParseSocketSpec("socket_gauntlet_r,hand_r,1.5,-2,3", s, &err));
    CHECK(s.name == "socket_gauntlet_r");
    CHECK(s.bone == "hand_r");
    CHECK_FALSE(s.componentSpace);
    CHECK(s.y == doctest::Approx(-2.0));
    REQUIRE(ParseSocketSpec("a, hand_r ,1,2,3,10,20,30", s, &err));
    CHECK(s.bone == "hand_r");
    CHECK(s.yaw == doctest::Approx(30.0));
    REQUIRE(ParseSocketSpec("g,hand_r,cs,-54.55,27.01,91.61,0,0,2", s, &err));
    CHECK(s.componentSpace);
    CHECK(s.hasAxis);
    CHECK(s.az == doctest::Approx(1.0));
    REQUIRE(ParseSocketSpec("g,hand_r,cs,1,2,3", s, &err));
    CHECK_FALSE(s.hasAxis);

    CHECK_FALSE(ParseSocketSpec("g,hand_r,1,2", s, &err));
    CHECK_FALSE(ParseSocketSpec(",hand_r,1,2,3", s, &err));
    CHECK_FALSE(ParseSocketSpec("g,hand_r,1,x,3", s, &err));
    CHECK(err.find("not a number") != std::string::npos);
    CHECK_FALSE(ParseSocketSpec("g,hand_r,cs,1,2,3,0,0,0", s, &err));
}

TEST_CASE("times, views, frames and still names")
{
    std::vector<double> t;
    std::string err;
    REQUIRE(ParseTimes("0.3,2.3, 3.8,4.5,6,7.2", t, &err));
    REQUIRE(t.size() == 6);
    CHECK(t[2] == doctest::Approx(3.8));
    CHECK_FALSE(ParseTimes("", t, &err));
    CHECK_FALSE(ParseTimes("1,-0.5", t, &err));
    CHECK_FALSE(ParseTimes("1,,2", t, &err));

    std::vector<ShotView> v;
    REQUIRE(ParseViews("front,tq,low:20:-10", v, &err));
    REQUIRE(v.size() == 3);
    CHECK(v[1].yawDeg == doctest::Approx(40.0));
    CHECK(v[2].name == "low");
    CHECK(v[2].pitchDeg == doctest::Approx(-10.0));
    CHECK_FALSE(ParseViews("front,top", v, &err));
    CHECK(err.find("top") != std::string::npos);

    CHECK(FrameAt(2.3, 60.0) == 138);
    CHECK(FrameAt(0.0, 60.0) == 0);
    CHECK(FrameAt(7.2, 60.0) == 432);
    CHECK(StillName("A_Devin_Special_01", 2.3, "tq") == "A_Devin_Special_01_t2.30_tq.png");
}

TEST_CASE("an FBX file's bone names, binary and ASCII")
{
    std::vector<std::uint8_t> bin;
    const std::string magic = "Kaydara FBX Binary  ";
    bin.insert(bin.end(), magic.begin(), magic.end());
    bin.insert(bin.end(), {0x00, 0x1a, 0x00, 0xe8, 0x1c, 0x00, 0x00});
    PutModel(bin, "root", "Null");
    PutModel(bin, "pelvis", "LimbNode");
    PutModel(bin, "body", "Mesh");
    PutModel(bin, "hand_r", "LimbNode");
    bin.insert(bin.end(), 16, 0);
    const std::vector<std::string> names = FbxLimbNames(bin.data(), bin.size());
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "pelvis");
    CHECK(names[1] == "hand_r");

    const std::string ascii = "; FBX 7.4.0 project file\nObjects:  {\n"
                              "\tModel: 11, \"Model::pelvis\", \"LimbNode\" {\n\t}\n"
                              "\tModel: 12, \"Model::Cube\", \"Mesh\" {\n\t}\n"
                              "\tModel: 13, \"Model::spine_01\", \"LimbNode\" {\n\t}\n}\n";
    const auto* a = reinterpret_cast<const std::uint8_t*>(ascii.data());
    const std::vector<std::string> an = FbxLimbNames(a, ascii.size());
    REQUIRE(an.size() == 2);
    CHECK(an[1] == "spine_01");

    CHECK(FbxLimbNames(nullptr, 0).empty());
}

TEST_CASE("missing bones and the track share")
{
    const std::vector<std::string> file = {"pelvis", "spine_01", "staff_tip"};
    const std::vector<std::string> skel = {"root", "pelvis", "spine_01"};
    const std::vector<std::string> missing = MissingNames(file, skel);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == "staff_tip");
    CHECK(MissingNames(skel, skel).empty());

    CHECK(TrackShareOk(341, 341, 0.9));
    CHECK(TrackShareOk(307, 341, 0.9));
    CHECK_FALSE(TrackShareOk(300, 341, 0.9));
    CHECK(TrackShareOk(0, 0, 0.9));
}
