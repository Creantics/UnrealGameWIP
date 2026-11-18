#include <doctest/doctest.h>
#include "ftcore/TierDetect.h"

using ftcore::HardwareInfo;
using ftcore::Tier;

namespace {
HardwareInfo card(const char* adapter, unsigned vram = 12288, unsigned cores = 8, unsigned height = 1080) {
    HardwareInfo hw;
    hw.adapter = adapter;
    hw.vramMB = vram;
    hw.cores = cores;
    hw.displayHeight = height;
    return hw;
}
} // namespace

TEST_CASE("tier detect: the longest table name wins, case ignored") {
    const auto* e = ftcore::findGpu("NVIDIA GeForce RTX 4060 Laptop GPU", ftcore::kGpuTable);
    REQUIRE(e);
    CHECK((e->name == "rtx 4060 laptop"));
    e = ftcore::findGpu("NVIDIA GeForce RTX 4060", ftcore::kGpuTable);
    REQUIRE(e);
    CHECK((e->name == "rtx 4060"));
    e = ftcore::findGpu("NVIDIA GeForce GTX 1660 SUPER", ftcore::kGpuTable);
    REQUIRE(e);
    CHECK(e->factor == doctest::Approx(2.7f));
    CHECK(ftcore::findGpu("Some Unknown Adapter", ftcore::kGpuTable) == nullptr);
}

TEST_CASE("tier detect: the factor picks the tier, the display picks High or Medium") {
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3080 Ti")).tier == Tier::Top);
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3060", 12288, 6)).tier == Tier::High);
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3060", 12288, 6, 1440)).tier == Tier::Medium);
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3050", 8192, 6)).tier == Tier::Low);
    CHECK(ftcore::pickTier(card("NVIDIA GeForce GTX 1650", 4096, 4)).tier == Tier::VeryLow);
}

TEST_CASE("tier detect: VRAM caps the tier and CPU cores flag the CPU knobs") {
    // A fast card with 6 GB is held at Low with Low textures.
    auto p = ftcore::pickTier(card("NVIDIA GeForce RTX 3060", 6144, 6));
    CHECK(p.tier == Tier::Low);
    CHECK(p.lowTextures);
    // 4 GB: Very Low whatever the card.
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3080 Ti", 4096)).tier == Tier::VeryLow);
    p = ftcore::pickTier(card("NVIDIA GeForce RTX 3060", 12288, 4));
    CHECK(p.lowCpu);
    CHECK(p.tier == Tier::High);
    CHECK_FALSE(ftcore::pickTier(card("NVIDIA GeForce RTX 3060", 12288, 6)).lowCpu);
}

TEST_CASE("tier detect: hardware RT only on Top cards with RT hardware fast enough") {
    CHECK(ftcore::pickTier(card("NVIDIA GeForce RTX 3080 Ti")).hardwareRt);
    CHECK_FALSE(ftcore::pickTier(card("NVIDIA GeForce RTX 4060")).hardwareRt); // High, not Top
    CHECK_FALSE(ftcore::pickTier(card("AMD Radeon RX 7600")).hardwareRt);
}

TEST_CASE("tier detect: an unknown card uses the synth benchmark, or Low without it") {
    auto hw = card("Mystery GPU 9000");
    auto p = ftcore::pickTier(hw);
    CHECK_FALSE(p.known);
    CHECK(p.tier == Tier::Low); // no benchmark, no calibration: the safe pick
    ftcore::TierRules rules;
    rules.benchRefIndex = 300.f;
    hw.benchGpuIndex = 150.f; // half the reference: factor 2.0
    p = ftcore::pickTier(hw, rules, ftcore::kGpuTable);
    CHECK(p.fromBench);
    CHECK(p.factor == doctest::Approx(2.0f));
    CHECK(p.tier == Tier::Low);
    hw.benchGpuIndex = 290.f;
    CHECK(ftcore::pickTier(hw, rules, ftcore::kGpuTable).tier == Tier::Top);
}

TEST_CASE("tier detect: the load-screen check steps down at most twice") {
    ftcore::TierCheck c;
    c.tier = Tier::Top;
    CHECK(c.ran(18.f)); // over 15: High
    CHECK(c.tier == Tier::High);
    CHECK(c.ran(17.f)); // still over: Low
    CHECK(c.tier == Tier::Low);
    CHECK_FALSE(c.ran(16.f)); // steps used up: Low holds
    CHECK(c.done);
    CHECK(c.tier == Tier::Low);

    ftcore::TierCheck ok;
    ok.tier = Tier::High;
    CHECK_FALSE(ok.ran(12.f));
    CHECK(ok.tier == Tier::High);

    ftcore::TierCheck bottom;
    bottom.tier = Tier::VeryLow;
    CHECK_FALSE(bottom.ran(30.f));
    CHECK(bottom.tier == Tier::VeryLow);
    CHECK(ftcore::stepDown(Tier::Medium) == Tier::Low);
}

TEST_CASE("tier detect: the fingerprint changes with the adapter or the driver, not the case") {
    const auto a = ftcore::hardwareFingerprint("NVIDIA GeForce RTX 3060", "32.0.15.6094");
    CHECK(a == ftcore::hardwareFingerprint("nvidia geforce rtx 3060", "32.0.15.6094"));
    CHECK(a != ftcore::hardwareFingerprint("NVIDIA GeForce RTX 3060", "32.0.15.6100"));
    CHECK(a != ftcore::hardwareFingerprint("NVIDIA GeForce RTX 4060", "32.0.15.6094"));
    CHECK(ftcore::hardwareFingerprint("ab", "c") != ftcore::hardwareFingerprint("a", "bc"));
}
