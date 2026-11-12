#include "ftcore/GarmentFront.h"

#include <utility>

namespace ftcore::look {

namespace {
bool SameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool Names(const FrontPair& pair, std::string_view word) {
    return word.empty() || pair.closed.find(word) != std::string::npos ||
           (!pair.open.empty() && pair.open.find(word) != std::string::npos);
}
} // namespace

bool ParseFront(std::string_view text, Front& out) {
    if (SameNoCase(text, "open")) {
        out = Front::Open;
        return true;
    }
    if (SameNoCase(text, "closed")) {
        out = Front::Closed;
        return true;
    }
    return false;
}

bool ParseFrontAsks(std::string_view text, std::vector<FrontAsk>& out) {
    if (text.empty() || text.back() == '+') {
        return false;
    }
    std::vector<FrontAsk> asks;
    while (!text.empty()) {
        const std::size_t plus = text.find('+');
        const std::string_view item = text.substr(0, plus);
        text = plus == std::string_view::npos ? std::string_view() : text.substr(plus + 1);
        const std::size_t colon = item.find(':');
        FrontAsk ask;
        if (colon != std::string_view::npos) {
            ask.word = std::string(item.substr(0, colon));
        }
        const std::string_view state = colon == std::string_view::npos ? item : item.substr(colon + 1);
        if ((colon != std::string_view::npos && ask.word.empty()) || !ParseFront(state, ask.state)) {
            return false;
        }
        asks.push_back(std::move(ask));
    }
    out.insert(out.end(), asks.begin(), asks.end());
    return true;
}

Front FrontFor(const FrontPair& pair, const std::vector<FrontAsk>& asks) {
    Front state = pair.byDefault;
    for (const FrontAsk& ask : asks) {
        if (Names(pair, ask.word)) {
            state = ask.state;
        }
    }
    return state;
}

const std::string& WornPiece(const std::string& piece, const std::vector<FrontPair>& pairs,
                             const std::vector<FrontAsk>& asks) {
    for (const FrontPair& pair : pairs) {
        if (pair.open.empty() || (piece != pair.closed && piece != pair.open)) {
            continue;
        }
        return FrontFor(pair, asks) == Front::Open ? pair.open : pair.closed;
    }
    return piece;
}

} // namespace ftcore::look
