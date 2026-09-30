/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "json.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace OHOS {
namespace ScreenParser {
namespace json {

namespace {

const Value &NullRef() {
    static const Value kNull;
    return kNull;
}

const Array &EmptyArray() {
    static const Array kEmpty;
    return kEmpty;
}

const Object &EmptyObject() {
    static const Object kEmpty;
    return kEmpty;
}

void AppendEscaped(std::string &out, const std::string &raw) {
    out.push_back('"');
    for (unsigned char ch : raw) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
        }
    }
    out.push_back('"');
}

void AppendNumber(std::string &out, double number) {
    if (std::isnan(number) || std::isinf(number)) {
        out += "null";
        return;
    }
    double rounded = std::llround(number);
    if (number == rounded && std::fabs(number) < 1e15) {
        out += std::to_string(static_cast<long long>(rounded));
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", number);
    out += buf;
}

// Recursive-descent JSON parser.
class Parser {
public:
    Parser(const std::string &text) : text_(text), pos_(0) {}

    bool Parse(Value &out) {
        SkipWhitespace();
        if (!ParseValue(out)) {
            return false;
        }
        SkipWhitespace();
        if (pos_ != text_.size()) {
            error_ = "trailing characters at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

    const std::string &error() const { return error_; }

private:
    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Fail(const std::string &msg) {
        error_ = msg + " at offset " + std::to_string(pos_);
        return false;
    }

    bool ParseValue(Value &out) {
        if (pos_ >= text_.size()) {
            return Fail("unexpected end of input");
        }
        char ch = text_[pos_];
        switch (ch) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string s;
                if (!ParseString(s)) {
                    return false;
                }
                out = Value(std::move(s));
                return true;
            }
            case 't': return ParseLiteral("true", Value(true), out);
            case 'f': return ParseLiteral("false", Value(false), out);
            case 'n': return ParseLiteral("null", Value(), out);
            default: return ParseNumber(out);
        }
    }

    bool ParseLiteral(const char *word, Value value, Value &out) {
        size_t len = std::strlen(word);
        if (text_.compare(pos_, len, word) != 0) {
            return Fail("invalid literal");
        }
        pos_ += len;
        out = value;
        return true;
    }

    bool ParseObject(Value &out) {
        ++pos_;  // consume '{'
        Object obj;
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = Value(std::move(obj));
            return true;
        }
        while (true) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return Fail("expected object key");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return Fail("expected ':'");
            }
            ++pos_;
            SkipWhitespace();
            Value value;
            if (!ParseValue(value)) {
                return false;
            }
            obj.emplace(std::move(key), std::move(value));
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return Fail("unterminated object");
            }
            char ch = text_[pos_++];
            if (ch == ',') {
                continue;
            }
            if (ch == '}') {
                break;
            }
            return Fail("expected ',' or '}'");
        }
        out = Value(std::move(obj));
        return true;
    }

    bool ParseArray(Value &out) {
        ++pos_;  // consume '['
        Array arr;
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = Value(std::move(arr));
            return true;
        }
        while (true) {
            SkipWhitespace();
            Value value;
            if (!ParseValue(value)) {
                return false;
            }
            arr.push_back(std::move(value));
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return Fail("unterminated array");
            }
            char ch = text_[pos_++];
            if (ch == ',') {
                continue;
            }
            if (ch == ']') {
                break;
            }
            return Fail("expected ',' or ']'");
        }
        out = Value(std::move(arr));
        return true;
    }

    bool ParseString(std::string &out) {
        ++pos_;  // consume opening quote
        std::string result;
        while (true) {
            if (pos_ >= text_.size()) {
                return Fail("unterminated string");
            }
            char ch = text_[pos_++];
            if (ch == '"') {
                break;
            }
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                return Fail("unterminated escape");
            }
            char esc = text_[pos_++];
            switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    unsigned int code = 0;
                    if (!ParseHex4(code)) {
                        return false;
                    }
                    // Handle surrogate pairs.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned int low = 0;
                        if (!ParseHex4(low)) {
                            return false;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }
                    AppendUtf8(result, code);
                    break;
                }
                default:
                    return Fail("invalid escape sequence");
            }
        }
        out = std::move(result);
        return true;
    }

    bool ParseHex4(unsigned int &out) {
        if (pos_ + 4 > text_.size()) {
            return Fail("invalid unicode escape");
        }
        unsigned int value = 0;
        for (int i = 0; i < 4; ++i) {
            char ch = text_[pos_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return Fail("invalid hex digit");
            }
        }
        out = value;
        return true;
    }

    static void AppendUtf8(std::string &out, unsigned int code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool ParseNumber(Value &out) {
        size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
            ++pos_;
        }
        bool anyDigit = false;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            anyDigit = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                anyDigit = true;
            }
        }
        if (!anyDigit) {
            return Fail("invalid number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        std::string token = text_.substr(start, pos_ - start);
        out = Value(std::strtod(token.c_str(), nullptr));
        return true;
    }

    const std::string &text_;
    size_t pos_;
    std::string error_;
};

}  // namespace

bool Value::as_bool(bool fallback) const {
    if (type_ == Type::Boolean) {
        return bool_;
    }
    if (type_ == Type::Number) {
        return number_ != 0.0;
    }
    return fallback;
}

double Value::as_double(double fallback) const {
    if (type_ == Type::Number) {
        return number_;
    }
    if (type_ == Type::Boolean) {
        return bool_ ? 1.0 : 0.0;
    }
    if (type_ == Type::String) {
        char *end = nullptr;
        double parsed = std::strtod(string_.c_str(), &end);
        if (end != string_.c_str()) {
            return parsed;
        }
    }
    return fallback;
}

int64_t Value::as_int(int64_t fallback) const {
    if (type_ == Type::Number) {
        return static_cast<int64_t>(std::llround(number_));
    }
    return static_cast<int64_t>(as_double(static_cast<double>(fallback)));
}

const std::string &Value::as_string() const {
    if (type_ == Type::String) {
        return string_;
    }
    static const std::string kEmpty;
    return kEmpty;
}

std::string Value::as_string_or(const std::string &fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

bool Value::contains(const std::string &key) const {
    return type_ == Type::Object && object_ && object_->find(key) != object_->end();
}

const Value &Value::at(const std::string &key) const {
    if (type_ == Type::Object && object_) {
        auto it = object_->find(key);
        if (it != object_->end()) {
            return it->second;
        }
    }
    return NullRef();
}

Value &Value::operator[](const std::string &key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_ = std::make_shared<Object>();
    }
    if (!object_) {
        object_ = std::make_shared<Object>();
    }
    return (*object_)[key];
}

void Value::set(const std::string &key, Value value) {
    (*this)[key] = std::move(value);
}

size_t Value::size() const {
    if (type_ == Type::Array && array_) {
        return array_->size();
    }
    if (type_ == Type::Object && object_) {
        return object_->size();
    }
    if (type_ == Type::String) {
        return string_.size();
    }
    return 0;
}

const Value &Value::operator[](size_t index) const {
    if (type_ == Type::Array && array_ && index < array_->size()) {
        return (*array_)[index];
    }
    return NullRef();
}

void Value::push_back(Value value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_ = std::make_shared<Array>();
    }
    if (!array_) {
        array_ = std::make_shared<Array>();
    }
    array_->push_back(std::move(value));
}

const Array &Value::items() const {
    if (type_ == Type::Array && array_) {
        return *array_;
    }
    return EmptyArray();
}

const Object &Value::members() const {
    if (type_ == Type::Object && object_) {
        return *object_;
    }
    return EmptyObject();
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_impl(out, indent, 0);
    return out;
}

void Value::dump_impl(std::string &out, int indent, int depth) const {
    auto newline = [&](int level) {
        if (indent < 0) {
            return;
        }
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * level), ' ');
    };
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Boolean:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            AppendNumber(out, number_);
            break;
        case Type::String:
            AppendEscaped(out, string_);
            break;
        case Type::Array: {
            const Array &arr = items();
            if (arr.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) {
                    out.push_back(',');
                }
                newline(depth + 1);
                arr[i].dump_impl(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const Object &obj = members();
            if (obj.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            bool first = true;
            for (const auto &kv : obj) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                newline(depth + 1);
                AppendEscaped(out, kv.first);
                out.push_back(':');
                if (indent >= 0) {
                    out.push_back(' ');
                }
                kv.second.dump_impl(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

bool Value::Parse(const std::string &text, Value &out, std::string &error) {
    Parser parser(text);
    if (parser.Parse(out)) {
        return true;
    }
    error = parser.error();
    out = Value();
    return false;
}

Value Value::ParseOrThrow(const std::string &text) {
    Value out;
    std::string error;
    if (!Parse(text, out, error)) {
        throw ParseError(error);
    }
    return out;
}

const Value &Value::Null() {
    return NullRef();
}

std::string EscapeString(const std::string &raw) {
    std::string out;
    AppendEscaped(out, raw);
    return out;
}

}  // namespace json
}  // namespace ScreenParser
}  // namespace OHOS
