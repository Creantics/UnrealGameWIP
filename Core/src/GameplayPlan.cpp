#include "ftcore/GameplayPlan.h"

#include <charconv>
#include <cstddef>

namespace ftcore {

namespace {

constexpr std::string_view kArrow = "\xE2\x86\x92"; // U+2192, UTF-8

struct TokenEntry {
    std::string_view name;
    GameplayToken token;
};
constexpr TokenEntry kTokens[] = {
    {"CMB", GameplayToken::Cmb}, {"ENG", GameplayToken::Eng}, {"PLAY", GameplayToken::Play},
    {"CHC", GameplayToken::Chc}, {"BNT", GameplayToken::Bnt}, {"CG", GameplayToken::Cg},
    {"HE", GameplayToken::He},   {"QTE", GameplayToken::Qte}, {"co-op QTE", GameplayToken::CoopQte},
    {"MINI", GameplayToken::Mini},
};

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isQuote(char c) { return c == '\'' || c == '"' || c == '`'; }

std::string_view trimSpace(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
    return s;
}

int parenBalance(std::string_view s) { // opens minus closes
    int n = 0;
    for (const char c : s) n += c == '(' ? 1 : c == ')' ? -1 : 0;
    return n;
}

// Whitespace, quotes, square brackets and unmatched parentheses at either end (left over from a
// list-shaped mode split on its commas).
std::string_view trimPiece(std::string_view s) {
    for (;;) {
        const std::size_t before = s.size();
        s = trimSpace(s);
        while (!s.empty() && (isQuote(s.front()) || s.front() == '[' || s.front() == ']')) s.remove_prefix(1);
        while (!s.empty() && (isQuote(s.back()) || s.back() == '[' || s.back() == ']')) s.remove_suffix(1);
        if (!s.empty() && s.front() == '(' && parenBalance(s) > 0) s.remove_prefix(1);
        if (!s.empty() && s.back() == ')' && parenBalance(s) < 0) s.remove_suffix(1);
        if (s.size() == before) return s;
    }
}

// The mode without a wrapper around all of it: "( ... )" or "[ ... ]" whose opener closes at the end.
std::string_view unwrap(std::string_view s) {
    for (;;) {
        s = trimSpace(s);
        if (s.size() < 2) return s;
        const char open = s.front();
        const char close = open == '(' ? ')' : open == '[' ? ']' : '\0';
        if (close == '\0' || s.back() != close) return s;
        int depth = 0;
        std::size_t closesAt = s.size();
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == open) ++depth;
            else if (s[i] == close && --depth == 0) {
                closesAt = i;
                break;
            }
        }
        if (closesAt != s.size() - 1) return s;
        s = s.substr(1, s.size() - 2);
    }
}

bool hasUpper(std::string_view s) {
    for (const char c : s)
        if (c >= 'A' && c <= 'Z') return true;
    return false;
}

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }

// Copies as much of `text` as fits in `cap` bytes after `length` bytes already in `buf`, cut at a
// UTF-8 character boundary.
void append(char* buf, std::uint8_t& length, std::size_t cap, std::string_view text) {
    std::size_t room = cap - length;
    if (text.size() > room) {
        while (room > 0 && isContinuation(text[room])) --room;
        text = text.substr(0, room);
    }
    for (const char c : text) buf[length++] = c;
}

void appendLabel(char* buf, std::uint8_t& length, std::string_view text) {
    if (text.empty()) return;
    if (length > 0) append(buf, length, GameplayStep::kLabelCapacity, ", ");
    append(buf, length, GameplayStep::kLabelCapacity, text);
}

} // namespace

std::string_view gameplayTokenName(GameplayToken token) {
    for (const TokenEntry& e : kTokens)
        if (e.token == token) return e.name;
    return {};
}

GameplayToken gameplayTokenFromName(std::string_view name) {
    for (const TokenEntry& e : kTokens)
        if (equalsNoCase(e.name, name)) return e.token;
    return GameplayToken::Unknown;
}

GameplayScreen gameplayTokenScreen(GameplayToken token) {
    switch (token) {
    case GameplayToken::Cmb: return GameplayScreen::Battle;
    case GameplayToken::Eng:
    case GameplayToken::Play: return GameplayScreen::Field;
    case GameplayToken::Chc: return GameplayScreen::Choice;
    case GameplayToken::Qte:
    case GameplayToken::CoopQte: return GameplayScreen::Qte;
    case GameplayToken::Cg:
    case GameplayToken::He: return GameplayScreen::Cutscene;
    case GameplayToken::Bnt:
    case GameplayToken::Mini:
    case GameplayToken::Unknown: break;
    }
    return GameplayScreen::Stub;
}

void GameplayPlan::clear() {
    count_ = 0;
    unknown_ = dropped_ = minutes_ = 0;
    pendingLength_ = 0;
}

bool GameplayPlan::lower(std::string_view mode, std::string_view fallbackKind, std::string_view minutes) {
    clear();
    lowerChain(mode);
    if (count_ == 0 && dropped_ == 0) {
        unknown_ = 0;
        pendingLength_ = 0;
        lowerChain(fallbackKind);
    }
    minutes = trimSpace(minutes);
    std::uint32_t m = 0;
    const auto [end, ec] = std::from_chars(minutes.data(), minutes.data() + minutes.size(), m);
    (void)end;
    minutes_ = ec == std::errc() ? m : 0;
    return count_ > 0;
}

bool GameplayPlan::lower(const StoryDatabase& db, const Beat& gameplay) {
    if (gameplay.type != BeatType::Gameplay) {
        clear();
        return false;
    }
    return lower(db.param(gameplay, "mode"), db.view(gameplay.kindName), db.param(gameplay, "minutes"));
}

bool GameplayPlan::lower(const StoryRunner& runner) {
    const Beat* b = runner.beat();
    if (!runner.waitingForGameplay() || !b || !runner.database()) {
        clear();
        return false;
    }
    return lower(*runner.database(), *b);
}

std::uint32_t GameplayPlan::count(GameplayScreen screen) const {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < count_; ++i)
        if (steps_[i].screen == screen) ++n;
    return n;
}

void GameplayPlan::lowerChain(std::string_view mode) {
    mode = unwrap(mode);
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < mode.size();) {
        const char c = mode[i];
        if (c == '(') ++depth;
        else if (c == ')' && depth > 0) --depth;
        std::size_t sep = 0;
        if (depth == 0) {
            if (c == '+' || c == ',') sep = 1;
            else if (mode.substr(i, kArrow.size()) == kArrow) sep = kArrow.size();
            else if (mode.substr(i, 2) == "->") sep = 2;
        }
        if (sep > 0) {
            addPiece(mode.substr(start, i - start));
            i += sep;
            start = i;
        } else {
            ++i;
        }
    }
    addPiece(mode.substr(start));
}

void GameplayPlan::addPiece(std::string_view piece) {
    piece = trimPiece(piece);
    if (piece.empty()) return;
    const std::size_t open = piece.find('(');
    const std::string_view token = trimPiece(piece.substr(0, open));
    if (token.empty() || !hasUpper(token)) {
        // A qualifier on its own: "intercut", "(loop)".
        if (open == std::string_view::npos) {
            addQualifier(token);
            return;
        }
    }

    // Each parenthetical group after the token, joined with ", ".
    char label[GameplayStep::kLabelCapacity] = {};
    std::uint8_t labelLength = 0;
    std::size_t i = open;
    while (i != std::string_view::npos && i < piece.size()) {
        int depth = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t j = i; j < piece.size(); ++j) {
            if (piece[j] == '(') ++depth;
            else if (piece[j] == ')' && --depth == 0) {
                close = j;
                break;
            }
        }
        const std::string_view group = trimPiece(piece.substr(i + 1, close == std::string_view::npos ? piece.size() - i - 1 : close - i - 1));
        appendLabel(label, labelLength, group);
        if (close == std::string_view::npos) break;
        i = piece.find('(', close);
    }

    if (token.empty() || !hasUpper(token)) {
        addQualifier(token);
        addQualifier({label, labelLength});
        return;
    }
    addStep(token, {label, labelLength});
}

void GameplayPlan::addStep(std::string_view token, std::string_view label) {
    if (count_ >= kMaxSteps) {
        ++dropped_;
        return;
    }
    GameplayStep& s = steps_[count_++];
    s.token = gameplayTokenFromName(token);
    s.screen = gameplayTokenScreen(s.token);
    if (s.token == GameplayToken::Unknown) ++unknown_;
    const std::string_view name = s.token == GameplayToken::Unknown ? token : gameplayTokenName(s.token);
    s.nameLength = 0;
    append(s.name, s.nameLength, GameplayStep::kNameCapacity, name);
    s.labelLength = 0;
    appendLabel(s.labelText, s.labelLength, {pending_, pendingLength_});
    pendingLength_ = 0;
    appendLabel(s.labelText, s.labelLength, label);
}

void GameplayPlan::addQualifier(std::string_view qualifier) {
    if (qualifier.empty()) return;
    if (dropped_ > 0) return; // it belongs to a step that was not kept
    if (count_ == 0) {
        appendLabel(pending_, pendingLength_, qualifier);
        return;
    }
    GameplayStep& s = steps_[count_ - 1];
    appendLabel(s.labelText, s.labelLength, qualifier);
}

} // namespace ftcore
