/*
 * Copyright (c) 2026 OpenHarmony Contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

// Minimal self-contained JSON value / parser / serializer used by the
// pure-logic modules of the screen parser service.
//
// Design notes:
//   * Header + single translation unit, no external dependency, so the
//     schema / tokenizer / analyzer logic can be unit-tested off-device.
//   * The public surface intentionally mirrors the subset of nlohmann::json
//     used by this project, so it can be swapped for //third_party/json on
//     device if desired.

#ifndef FOUNDATION_SCREENPARSER_COMMON_JSON_H
#define FOUNDATION_SCREENPARSER_COMMON_JSON_H

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace OHOS {
namespace ScreenParser {
namespace json {

enum class Type {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
};

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

// A JSON value. Numbers are stored as double (sufficient for the coordinate
// and confidence fields produced by the model); integer values round-trip
// without loss within the 2^53 range.
class Value {
public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Boolean), bool_(b) {}
    Value(int v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(int64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Value(double v) : type_(Type::Number), number_(v) {}
    Value(const char *s) : type_(Type::String), string_(s) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), array_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : type_(Type::Object), object_(std::make_shared<Object>(std::move(o))) {}

    static Value MakeArray() { return Value(Array{}); }
    static Value MakeObject() { return Value(Object{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Boolean; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Value accessors with defensive fallbacks (never throw).
    bool as_bool(bool fallback = false) const;
    double as_double(double fallback = 0.0) const;
    int64_t as_int(int64_t fallback = 0) const;
    const std::string &as_string() const;
    std::string as_string_or(const std::string &fallback) const;

    // Object access.
    bool contains(const std::string &key) const;
    const Value &at(const std::string &key) const;   // returns NullSingleton if absent
    Value &operator[](const std::string &key);        // promotes to object
    void set(const std::string &key, Value value);

    // Array access.
    size_t size() const;
    const Value &operator[](size_t index) const;      // returns NullSingleton if OOB
    void push_back(Value value);                      // promotes to array
    const Array &items() const;
    const Object &members() const;

    // Serialization.
    std::string dump(int indent = -1) const;

    // Parsing: returns true on success. On failure, `out` is Null and
    // `error` describes the problem.
    static bool Parse(const std::string &text, Value &out, std::string &error);
    static Value ParseOrThrow(const std::string &text);

    // Shared null used for safe const access.
    static const Value &Null();

private:
    void dump_impl(std::string &out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::shared_ptr<Array> array_;
    std::shared_ptr<Object> object_;
};

// Thrown by ParseOrThrow.
class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string &msg) : std::runtime_error(msg) {}
};

// Escape a UTF-8 string into a JSON string body (without surrounding quotes).
std::string EscapeString(const std::string &raw);

}  // namespace json
}  // namespace ScreenParser
}  // namespace OHOS

#endif  // FOUNDATION_SCREENPARSER_COMMON_JSON_H
