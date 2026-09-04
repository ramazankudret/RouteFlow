#include "json.h"

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rf {
namespace {

const Json& null_json() {
    static const Json kNull;
    return kNull;
}

// Shortest representation that round-trips. %.17g always round-trips but emits
// noise like 0.10000000000000001; try shorter precisions first.
void append_int(std::string& out, int64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    out += buf;
}

void append_number(std::string& out, double v) {
    if (!std::isfinite(v)) {  // JSON has no NaN/Inf; null is the honest encoding.
        out += "null";
        return;
    }
    if (v == static_cast<double>(static_cast<int64_t>(v)) && std::fabs(v) < 9.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld",
                      static_cast<long long>(static_cast<int64_t>(v)));
        out += buf;
        return;
    }
    char buf[40];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

void append_escaped(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 passes through
                }
        }
    }
    out += '"';
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    Parser(const std::string& s, std::string* err) : s_(s), err_(err) {}

    bool run(Json& out) {
        skip_ws();
        if (!value(out, 0)) return false;
        skip_ws();
        if (i_ != s_.size()) return fail("trailing content");
        return true;
    }

private:
    static constexpr int kMaxDepth = 200;

    bool fail(const char* msg) {
        if (err_) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s at byte %zu", msg, i_);
            *err_ = buf;
        }
        return false;
    }

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool lit(const char* word) {
        size_t n = std::strlen(word);
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (i_ >= s_.size()) return fail("unexpected end");
        switch (s_[i_]) {
            case 'n': if (!lit("null")) return fail("bad literal"); out = Json(); return true;
            case 't': if (!lit("true")) return fail("bad literal"); out = Json(true); return true;
            case 'f': if (!lit("false")) return fail("bad literal"); out = Json(false); return true;
            case '"': { std::string v; if (!string(v)) return false; out = Json(std::move(v)); return true; }
            case '[': return array(out, depth);
            case '{': return object(out, depth);
            default:  return number(out);
        }
    }

    bool string(std::string& out) {
        if (s_[i_] != '"') return fail("expected string");
        ++i_;
        out.clear();
        while (true) {
            if (i_ >= s_.size()) return fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') { ++i_; return true; }
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size()) return fail("unterminated escape");
                char e = s_[i_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        uint32_t cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                            if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                                i_ += 2;
                                uint32_t lo;
                                if (!hex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF)
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                else
                                    cp = 0xFFFD;
                            } else {
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            cp = 0xFFFD;  // lone low surrogate
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
                continue;
            }
            if (c < 0x20) return fail("raw control character in string");
            out += static_cast<char>(c);
            ++i_;
        }
    }

    bool hex4(uint32_t& out) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool number(Json& out) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        bool any = false;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) { ++i_; any = true; }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) { ++i_; any = true; }
        }
        if (any && i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (!any) return fail("expected value");
        const std::string text = s_.substr(start, i_ - start);
        // No fraction and no exponent: parse it as an integer so a value above
        // 2^53 survives a round trip through this parser unchanged.
        if (text.find('.') == std::string::npos &&
            text.find('e') == std::string::npos &&
            text.find('E') == std::string::npos) {
            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno != ERANGE && end && *end == 0) {
                out = Json(v);
                return true;
            }
        }
        out = Json(std::strtod(text.c_str(), nullptr));
        return true;
    }

    bool array(Json& out, int depth) {
        ++i_;  // '['
        out = Json::array();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            skip_ws();
            Json v;
            if (!value(v, depth + 1)) return false;
            out.push_back(std::move(v));
            skip_ws();
            if (i_ >= s_.size()) return fail("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Json& out, int depth) {
        ++i_;  // '{'
        out = Json::object();
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
            ++i_;
            skip_ws();
            Json v;
            if (!value(v, depth + 1)) return false;
            out[key] = std::move(v);
            skip_ws();
            if (i_ >= s_.size()) return fail("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    const std::string& s_;
    std::string* err_;
    size_t i_ = 0;
};

}  // namespace

int64_t Json::as_i64(int64_t def) const {
    if (type_ != Type::Number) return def;
    return has_int_ ? int_ : static_cast<int64_t>(num_);
}
uint64_t Json::as_u64(uint64_t def) const {
    if (type_ != Type::Number) return def;
    if (has_int_) return int_ < 0 ? def : static_cast<uint64_t>(int_);
    return num_ < 0 ? def : static_cast<uint64_t>(num_);
}
uint32_t Json::as_u32(uint32_t def) const {
    if (type_ != Type::Number) return def;
    if (has_int_) return int_ < 0 ? def : static_cast<uint32_t>(int_);
    return num_ < 0 ? def : static_cast<uint32_t>(num_);
}

bool Json::has(const std::string& key) const {
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return null_json();
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    for (auto& kv : obj_)
        if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const Json& Json::at(size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return null_json();
    return arr_[i];
}

Json& Json::at(size_t i) {
    assert(type_ == Type::Array && i < arr_.size());
    return arr_[i];
}

void Json::push_back(Json v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    auto newline_indent = [&](int d) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(indent * d), ' ');
    };

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number:
            if (has_int_) append_int(out, int_);
            else append_number(out, num_);
            break;
        case Type::String: append_escaped(out, str_); break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                newline_indent(depth + 1);
                arr_[i].dump_to(out, indent, depth + 1);
            }
            newline_indent(depth);
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                newline_indent(depth + 1);
                append_escaped(out, obj_[i].first);
                out += ':';
                if (pretty) out += ' ';
                obj_[i].second.dump_to(out, indent, depth + 1);
            }
            newline_indent(depth);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    Parser p(text, err);
    Json tmp;
    if (!p.run(tmp)) return false;
    out = std::move(tmp);
    return true;
}

}  // namespace rf
