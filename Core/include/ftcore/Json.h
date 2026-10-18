#pragma once

// A small JSON reader for load-time data (story scripts, face presets). Parsing allocates; reading
// the parsed document does not.
//
//   ftcore::JsonDocument doc;
//   std::string error;
//   if (!doc.parse(text, &error)) ...;
//   const ftcore::JsonValue root = doc.root();
//   for (std::uint32_t i = 0; i < root["chapters"].size(); ++i) {
//       const ftcore::JsonValue ch = root["chapters"][i];
//       std::string_view id = ch["id"].string();
//   }
//
// Strict RFC 8259: one value, no comments, no trailing commas, no trailing text. Strings are
// unescaped into the document (\uXXXX and surrogate pairs become UTF-8). Lookups are forgiving:
// a missing member, an out-of-range element or a value of the wrong type reads as an absent value
// (isAbsent()), whose accessors return their defaults, so a chain like doc.root()["a"][2]["b"] is
// always safe. A JsonValue is a view: it is valid while its document lives and is not re-parsed.

#include "ftcore/Api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ftcore {

enum class JsonType : std::uint8_t { Absent, Null, Bool, Number, String, Array, Object };

class JsonDocument;

class JsonValue {
public:
    JsonValue() = default;

    FTCORE_API JsonType type() const;
    bool isAbsent() const { return type() == JsonType::Absent; }
    bool isNull() const { return type() == JsonType::Null; }
    bool isBool() const { return type() == JsonType::Bool; }
    bool isNumber() const { return type() == JsonType::Number; }
    // A number written without a fraction or exponent that fits in an int64.
    FTCORE_API bool isInteger() const;
    bool isString() const { return type() == JsonType::String; }
    bool isArray() const { return type() == JsonType::Array; }
    bool isObject() const { return type() == JsonType::Object; }

    // Each returns `fallback` when the value has another type.
    FTCORE_API bool boolean(bool fallback = false) const;
    FTCORE_API double number(double fallback = 0.0) const;
    FTCORE_API std::int64_t integer(std::int64_t fallback = 0) const; // integers only (see isInteger)
    FTCORE_API std::string_view string(std::string_view fallback = {}) const;
    // The value's source text, as written ("3", "1.50", "{\"a\": 1}"); "" when absent.
    FTCORE_API std::string_view raw() const;

    // Arrays: element count. Objects: member count. 0 otherwise.
    FTCORE_API std::uint32_t size() const;
    // Array element (absent when out of range or not an array).
    FTCORE_API JsonValue operator[](std::uint32_t index) const;
    JsonValue operator[](int index) const { return index < 0 ? JsonValue{} : (*this)[static_cast<std::uint32_t>(index)]; }
    // Object member by key (the first one with that key; absent when missing or not an object).
    FTCORE_API JsonValue operator[](std::string_view key) const;
    JsonValue operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    bool has(std::string_view key) const { return !(*this)[key].isAbsent(); }
    // Object members in document order.
    FTCORE_API std::string_view key(std::uint32_t index) const;
    FTCORE_API JsonValue value(std::uint32_t index) const;

private:
    friend class JsonDocument;
    JsonValue(const JsonDocument* doc, std::uint32_t node) : doc_(doc), node_(node) {}

    const JsonDocument* doc_ = nullptr;
    std::uint32_t node_ = 0;
};

class JsonDocument {
public:
    // Nesting deeper than this is rejected, so a hostile file cannot exhaust the stack.
    static constexpr int kMaxDepth = 200;

    // Parses `text` (UTF-8). The text is not referenced afterwards. False on bad JSON: the
    // document is then empty and `*error` says what and where ("line 3, column 7: ...").
    FTCORE_API bool parse(std::string_view text, std::string* error = nullptr);
    FTCORE_API void clear();
    bool empty() const { return nodes_.empty(); }
    // The top-level value (absent when nothing is parsed).
    JsonValue root() const { return nodes_.empty() ? JsonValue{} : JsonValue(this, 0); }

private:
    friend class JsonValue;
    friend class JsonParser;

    struct Node {
        JsonType type = JsonType::Null;
        bool integer = false;   // Number: written as an integer that fits in int64
        bool flag = false;      // Bool
        std::int64_t i = 0;     // Number (integer)
        double d = 0.0;         // Number
        std::uint32_t str = 0, strLength = 0; // String: unescaped text in text_
        std::uint32_t raw = 0, rawLength = 0; // source text in raw_
        std::uint32_t first = 0, count = 0;   // Array / Object: children in kids_
    };
    struct Kid {
        std::uint32_t key = 0, keyLength = 0; // objects: the member's key in text_
        std::uint32_t node = 0;
    };

    std::vector<Node> nodes_;
    std::vector<Kid> kids_;
    std::string text_; // unescaped strings and keys
    std::string raw_;  // the source text
};

} // namespace ftcore
