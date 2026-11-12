#include "ftcore/GarmentFront.h"

#include <algorithm>
#include <utility>

namespace ftcore::look {

namespace {
char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool SameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool HoldsNoCase(std::string_view text, std::string_view word) {
    if (word.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
        if (SameNoCase(text.substr(i, word.size()), word)) {
            return true;
        }
    }
    return false;
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

std::string PiecePathKey(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t dot = path.find('.', slash == std::string_view::npos ? 0 : slash + 1);
    if (dot != std::string_view::npos) {
        const std::size_t start = slash == std::string_view::npos ? 0 : slash + 1;
        if (SameNoCase(path.substr(start, dot - start), path.substr(dot + 1))) {
            path = path.substr(0, dot);
        }
    }
    std::string key(path);
    std::transform(key.begin(), key.end(), key.begin(), Lower);
    return key;
}

std::string_view PieceAssetName(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash != std::string_view::npos) {
        path = path.substr(slash + 1);
    }
    return path.substr(0, path.find('.'));
}

bool AskNames(const FrontPair& pair, std::string_view word) {
    return word.empty() || HoldsNoCase(PieceAssetName(pair.closed), word) ||
           (!pair.open.empty() && HoldsNoCase(PieceAssetName(pair.open), word));
}

Front FrontFor(const FrontPair& pair, const std::vector<FrontAsk>& asks) {
    if (!pair.openMade || pair.open.empty()) {
        return Front::Closed;
    }
    Front state = pair.byDefault;
    for (const FrontAsk& ask : asks) {
        if (AskNames(pair, ask.word)) {
            state = ask.state;
        }
    }
    return state;
}

FrontChoice WornChoice(std::string_view piece, const std::vector<FrontPair>& pairs, const std::vector<FrontAsk>& asks) {
    const std::string key = PiecePathKey(piece);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const FrontPair& pair = pairs[i];
        if (key == PiecePathKey(pair.closed) || (!pair.open.empty() && key == PiecePathKey(pair.open))) {
            return {static_cast<int>(i), FrontFor(pair, asks)};
        }
    }
    return {};
}

const std::string& WornPiece(const std::string& piece, const std::vector<FrontPair>& pairs,
                             const std::vector<FrontAsk>& asks) {
    const FrontChoice choice = WornChoice(piece, pairs, asks);
    if (choice.pair < 0) {
        return piece;
    }
    const FrontPair& pair = pairs[static_cast<std::size_t>(choice.pair)];
    return choice.half == Front::Open ? pair.open : pair.closed;
}

std::vector<std::string> UnmatchedAskWords(const std::vector<FrontPair>& pairs, const std::vector<FrontAsk>& asks) {
    std::vector<std::string> words;
    for (const FrontAsk& ask : asks) {
        if (ask.word.empty() || std::find(words.begin(), words.end(), ask.word) != words.end()) {
            continue;
        }
        const bool named = std::any_of(pairs.begin(), pairs.end(),
                                       [&ask](const FrontPair& pair) { return AskNames(pair, ask.word); });
        if (!named) {
            words.push_back(ask.word);
        }
    }
    return words;
}

} // namespace ftcore::look
