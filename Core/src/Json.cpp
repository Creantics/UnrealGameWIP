#include "ftcore/Json.h"

#include <charconv>
#include <cstring>
#include <system_error>

namespace ftcore {

// Recursive descent over the source. Each container's children are gathered on a scratch stack
// and copied to kids_ in one block when it closes, so element / member lookup is a plain index.
class JsonParser {
public:
    JsonParser(JsonDocument& doc, std::string_view src) : doc_(doc), src_(src) {}

    bool run(std::string* error) {
        skipSpace();
        const std::uint32_t root = value(0);
        if (ok_) {
            skipSpace();
            if (pos_ != src_.size()) fail("unexpected text after the value");
        }
        if (!ok_) {
            if (error) *error = where() + error_;
            return false;
        }
        (void)root; // always node 0
        return true;
    }

private:
    using Node = JsonDocument::Node;
    using Kid = JsonDocument::Kid;

    void fail(const char* what) {
        if (!ok_) return;
        ok_ = false;
        error_ = what;
        errorPos_ = pos_;
    }

    std::string where() const {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < errorPos_ && i < src_.size(); ++i) {
            if (src_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": ";
    }

    bool atEnd() const { return pos_ >= src_.size(); }
    char peek() const { return atEnd() ? '\0' : src_[pos_]; }

    void skipSpace() {
        while (!atEnd()) {
            const char c = src_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (src_.substr(pos_, n) != word) return false;
        pos_ += n;
        return true;
    }

    std::uint32_t addNode(JsonType type, std::size_t start) {
        Node n;
        n.type = type;
        n.raw = static_cast<std::uint32_t>(start);
        doc_.nodes_.push_back(n);
        return static_cast<std::uint32_t>(doc_.nodes_.size() - 1);
    }

    void closeRaw(std::uint32_t node, std::size_t start) {
        doc_.nodes_[node].rawLength = static_cast<std::uint32_t>(pos_ - start);
    }

    std::uint32_t value(int depth) {
        if (depth > JsonDocument::kMaxDepth) {
            fail("nesting too deep");
            return 0;
        }
        const std::size_t start = pos_;
        const char c = peek();
        std::uint32_t node = 0;
        if (c == '{' || c == '[') {
            node = container(c == '{', depth);
        } else if (c == '"') {
            node = addNode(JsonType::String, start);
            std::uint32_t off = 0, len = 0;
            if (string(off, len)) {
                doc_.nodes_[node].str = off;
                doc_.nodes_[node].strLength = len;
            }
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            node = number(start);
        } else if (literal("true") || literal("false")) {
            node = addNode(JsonType::Bool, start);
            doc_.nodes_[node].flag = src_[start] == 't';
        } else if (literal("null")) {
            node = addNode(JsonType::Null, start);
        } else {
            fail(atEnd() ? "unexpected end of input" : "expected a value");
            return 0;
        }
        if (ok_) closeRaw(node, start);
        return node;
    }

    std::uint32_t container(bool object, int depth) {
        const std::size_t start = pos_;
        const std::uint32_t node = addNode(object ? JsonType::Object : JsonType::Array, start);
        const std::size_t mark = scratch_.size();
        ++pos_; // '{' or '['
        skipSpace();
        const char close = object ? '}' : ']';
        if (peek() == close) {
            ++pos_;
        } else {
            for (;;) {
                Kid kid;
                if (object) {
                    if (peek() != '"') {
                        fail("expected a member name");
                        return node;
                    }
                    if (!string(kid.key, kid.keyLength)) return node;
                    skipSpace();
                    if (peek() != ':') {
                        fail("expected ':'");
                        return node;
                    }
                    ++pos_;
                    skipSpace();
                }
                kid.node = value(depth + 1);
                if (!ok_) return node;
                scratch_.push_back(kid);
                skipSpace();
                if (peek() == ',') {
                    ++pos_;
                    skipSpace();
                    continue;
                }
                if (peek() == close) {
                    ++pos_;
                    break;
                }
                fail(object ? "expected ',' or '}'" : "expected ',' or ']'");
                return node;
            }
        }
        Node& n = doc_.nodes_[node];
        n.first = static_cast<std::uint32_t>(doc_.kids_.size());
        n.count = static_cast<std::uint32_t>(scratch_.size() - mark);
        doc_.kids_.insert(doc_.kids_.end(), scratch_.begin() + static_cast<std::ptrdiff_t>(mark), scratch_.end());
        scratch_.resize(mark);
        return node;
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(std::uint32_t& out) {
        if (src_.size() - pos_ < 4) {
            fail("bad \\u escape");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hexDigit(src_[pos_ + static_cast<std::size_t>(i)]);
            if (h < 0) {
                fail("bad \\u escape");
                return false;
            }
            out = out * 16 + static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    void appendUtf8(std::uint32_t cp) {
        std::string& t = doc_.text_;
        if (cp < 0x80) {
            t += static_cast<char>(cp);
        } else if (cp < 0x800) {
            t += static_cast<char>(0xC0 | (cp >> 6));
            t += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            t += static_cast<char>(0xE0 | (cp >> 12));
            t += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            t += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            t += static_cast<char>(0xF0 | (cp >> 18));
            t += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            t += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            t += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // At the opening quote. Appends the unescaped text to text_.
    bool string(std::uint32_t& offset, std::uint32_t& length) {
        ++pos_;
        std::string& t = doc_.text_;
        const std::size_t begin = t.size();
        for (;;) {
            if (atEnd()) {
                fail("unterminated string");
                return false;
            }
            const char c = src_[pos_];
            if (c == '"') {
                ++pos_;
                break;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                fail("control character in string");
                return false;
            }
            if (c != '\\') {
                // Copy the run up to the next quote or backslash in one go.
                std::size_t end = pos_ + 1;
                while (end < src_.size() && src_[end] != '"' && src_[end] != '\\' &&
                       static_cast<unsigned char>(src_[end]) >= 0x20)
                    ++end;
                t.append(src_.substr(pos_, end - pos_));
                pos_ = end;
                continue;
            }
            ++pos_;
            const char e = peek();
            ++pos_;
            switch (e) {
            case '"': t += '"'; break;
            case '\\': t += '\\'; break;
            case '/': t += '/'; break;
            case 'b': t += '\b'; break;
            case 'f': t += '\f'; break;
            case 'n': t += '\n'; break;
            case 'r': t += '\r'; break;
            case 't': t += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t lo = 0;
                    if (src_.substr(pos_, 2) != "\\u") {
                        fail("unpaired surrogate");
                        return false;
                    }
                    pos_ += 2;
                    if (!hex4(lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) {
                        fail("unpaired surrogate");
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("unpaired surrogate");
                    return false;
                }
                appendUtf8(cp);
                break;
            }
            default:
                --pos_;
                fail("bad escape");
                return false;
            }
        }
        offset = static_cast<std::uint32_t>(begin);
        length = static_cast<std::uint32_t>(t.size() - begin);
        return true;
    }

    std::uint32_t number(std::size_t start) {
        // Validate the RFC 8259 grammar first; from_chars is more lenient (leading zeros, "1.").
        std::size_t p = pos_;
        auto digits = [&] {
            const std::size_t from = p;
            while (p < src_.size() && src_[p] >= '0' && src_[p] <= '9') ++p;
            return p - from;
        };
        if (p < src_.size() && src_[p] == '-') ++p;
        const std::size_t intStart = p;
        const std::size_t intDigits = digits();
        if (intDigits == 0 || (intDigits > 1 && src_[intStart] == '0')) {
            fail("bad number");
            return 0;
        }
        bool integral = true;
        if (p < src_.size() && src_[p] == '.') {
            ++p;
            integral = false;
            if (digits() == 0) {
                pos_ = p;
                fail("bad number");
                return 0;
            }
        }
        if (p < src_.size() && (src_[p] == 'e' || src_[p] == 'E')) {
            ++p;
            integral = false;
            if (p < src_.size() && (src_[p] == '+' || src_[p] == '-')) ++p;
            if (digits() == 0) {
                pos_ = p;
                fail("bad number");
                return 0;
            }
        }
        const char* first = src_.data() + pos_;
        const char* last = src_.data() + p;
        const std::uint32_t node = addNode(JsonType::Number, start);
        Node& n = doc_.nodes_[node];
        if (integral) {
            std::int64_t v = 0;
            const auto r = std::from_chars(first, last, v);
            if (r.ec == std::errc{} && r.ptr == last) {
                n.integer = true;
                n.i = v;
                n.d = static_cast<double>(v);
            }
        }
        if (!n.integer) {
            double d = 0.0;
            const auto r = std::from_chars(first, last, d);
            if (r.ec != std::errc{} || r.ptr != last) {
                fail("number out of range");
                return node;
            }
            n.d = d;
        }
        pos_ = p;
        return node;
    }

    JsonDocument& doc_;
    std::string_view src_;
    std::size_t pos_ = 0;
    bool ok_ = true;
    const char* error_ = "";
    std::size_t errorPos_ = 0;
    std::vector<Kid> scratch_;
};

// ---------------------------------------------------------------------------------------------

bool JsonDocument::parse(std::string_view text, std::string* error) {
    clear();
    if (text.size() >= 0xFFFFFFFFu) {
        if (error) *error = "JSON text too large";
        return false;
    }
    raw_.assign(text.data(), text.size());
    JsonParser parser(*this, raw_);
    if (!parser.run(error)) {
        clear();
        return false;
    }
    return true;
}

void JsonDocument::clear() {
    nodes_.clear();
    kids_.clear();
    text_.clear();
    raw_.clear();
}

// ---------------------------------------------------------------------------------------------

JsonType JsonValue::type() const { return doc_ ? doc_->nodes_[node_].type : JsonType::Absent; }

bool JsonValue::isInteger() const { return isNumber() && doc_->nodes_[node_].integer; }

bool JsonValue::boolean(bool fallback) const { return isBool() ? doc_->nodes_[node_].flag : fallback; }

double JsonValue::number(double fallback) const { return isNumber() ? doc_->nodes_[node_].d : fallback; }

std::int64_t JsonValue::integer(std::int64_t fallback) const {
    return isInteger() ? doc_->nodes_[node_].i : fallback;
}

std::string_view JsonValue::string(std::string_view fallback) const {
    if (!isString()) return fallback;
    const auto& n = doc_->nodes_[node_];
    return std::string_view(doc_->text_).substr(n.str, n.strLength);
}

std::string_view JsonValue::raw() const {
    if (!doc_) return {};
    const auto& n = doc_->nodes_[node_];
    return std::string_view(doc_->raw_).substr(n.raw, n.rawLength);
}

std::uint32_t JsonValue::size() const { return isArray() || isObject() ? doc_->nodes_[node_].count : 0; }

JsonValue JsonValue::operator[](std::uint32_t index) const {
    if (!isArray() || index >= doc_->nodes_[node_].count) return {};
    return JsonValue(doc_, doc_->kids_[doc_->nodes_[node_].first + index].node);
}

JsonValue JsonValue::operator[](std::string_view k) const {
    if (!isObject()) return {};
    const auto& n = doc_->nodes_[node_];
    const std::string_view text(doc_->text_);
    for (std::uint32_t i = 0; i < n.count; ++i) {
        const auto& kid = doc_->kids_[n.first + i];
        if (text.substr(kid.key, kid.keyLength) == k) return JsonValue(doc_, kid.node);
    }
    return {};
}

std::string_view JsonValue::key(std::uint32_t index) const {
    if (!isObject() || index >= doc_->nodes_[node_].count) return {};
    const auto& kid = doc_->kids_[doc_->nodes_[node_].first + index];
    return std::string_view(doc_->text_).substr(kid.key, kid.keyLength);
}

JsonValue JsonValue::value(std::uint32_t index) const {
    if (!isObject() || index >= doc_->nodes_[node_].count) return {};
    return JsonValue(doc_, doc_->kids_[doc_->nodes_[node_].first + index].node);
}

} // namespace ftcore
