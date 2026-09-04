// RouteFlow — minimal dependency-free JSON.
//
// Insertion-ordered objects (trace records must stay readable), never-throwing
// accessors (a malformed record is skipped and counted, not fatal — see
// docs/TRACE-SCHEMA.md rule 4), and an explicit null distinct from 0.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rf {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    explicit Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    // Integers keep an exact int64 alongside the double. A double holds
    // integers exactly only below 2^53, and an epoch timestamp in nanoseconds
    // is ~1.8e18 — storing one as a double both loses precision and prints as
    // 1.78e+18, which is a number a consumer expecting an integer will not
    // thank us for.
    // `long long` is the primary; everything else delegates to it. Delegating
    // via int64_t would be a self-call on LP64, where int64_t *is* long.
    // Unsigned values above INT64_MAX would wrap, which nothing here produces:
    // these carry byte counts, token counts and epoch times.
    Json(long long v)
        : type_(Type::Number), num_(static_cast<double>(v)), int_(v), has_int_(true) {}
    Json(int v) : Json(static_cast<long long>(v)) {}
    Json(long v) : Json(static_cast<long long>(v)) {}
    Json(unsigned v) : Json(static_cast<long long>(v)) {}
    Json(unsigned long v) : Json(static_cast<long long>(v)) {}
    Json(unsigned long long v) : Json(static_cast<long long>(v)) {}
    Json(const char* v) : type_(Type::String), str_(v ? v : "") {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_num() const { return type_ == Type::Number; }
    bool is_str() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    // Accessors never throw; a type mismatch yields the supplied default.
    bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double as_num(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    int64_t as_i64(int64_t def = 0) const;
    uint64_t as_u64(uint64_t def = 0) const;
    uint32_t as_u32(uint32_t def = 0) const;
    std::string as_str(const std::string& def = std::string()) const {
        return type_ == Type::String ? str_ : def;
    }

    // --- object -------------------------------------------------------------
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;  // missing -> null Json
    Json& operator[](const std::string& key);              // missing -> created
    void set(const std::string& key, Json v) { (*this)[key] = std::move(v); }
    const std::vector<std::pair<std::string, Json>>& fields() const { return obj_; }

    // --- array --------------------------------------------------------------
    size_t size() const;
    // Deliberately not operator[]: overloading the subscript on both a string
    // key and an integer index is ambiguous for a literal 0, which is also a
    // null-pointer constant. at() also reads unmistakably at the call site.
    const Json& at(size_t i) const;  // out of range -> null Json
    void push_back(Json v);
    const std::vector<Json>& items() const { return arr_; }

    // indent < 0 => compact (one line, what the trace writer uses).
    std::string dump(int indent = -1) const;

    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;     // exact value when has_int_; num_ is the lossy view
    bool has_int_ = false;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace rf
