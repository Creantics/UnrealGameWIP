#include "ftcore/StubScreen.h"

#include "ftcore/GameplayPlan.h"

namespace ftcore {

std::string_view stubTitle(std::string_view token) {
    if (token.empty()) {
        return "Step";
    }
    switch (gameplayTokenFromName(token)) {
    case GameplayToken::Chc: return "Choice";
    case GameplayToken::Bnt: return "Banter";
    case GameplayToken::Cg: return "Cutscene";
    case GameplayToken::He: return "Heart-to-heart";
    case GameplayToken::Qte:
    case GameplayToken::CoopQte: return "Quick-time event";
    case GameplayToken::Mini: return "Mini-game";
    default: return token;
    }
}

double StubScreen::speedFactor(bool fast, double worldDilation) {
    if (!fast) {
        return 1.0;
    }
    const double world = worldDilation > 0.001 ? worldDilation : 0.001;
    const double factor = kFastDilation / world;
    return factor > 1.0 ? factor : 1.0;
}

void StubScreen::begin(bool hasStep, bool autoPlay) {
    phase_ = hasStep ? StubPhase::Waiting : StubPhase::NoStep;
    autoPlay_ = hasStep && autoPlay;
    elapsed_ = 0.0;
}

void StubScreen::tick(double gameSeconds) {
    if (phase_ != StubPhase::Waiting || gameSeconds <= 0.0) {
        return;
    }
    elapsed_ += gameSeconds;
    if (autoPlay_ && elapsed_ >= kAutoPlayDelay) {
        phase_ = StubPhase::Done;
    }
}

bool StubScreen::press() {
    if (phase_ != StubPhase::Waiting || elapsed_ < kInputGrace) {
        return false;
    }
    phase_ = StubPhase::Done;
    return true;
}

} // namespace ftcore
