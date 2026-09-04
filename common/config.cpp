#include "config.h"

#include <cstdlib>

#include "util.h"

namespace rf {
namespace {

const Json& null_json() {
    static const Json kNull;
    return kNull;
}

// A command-line value is untyped text; infer the JSON type so that
// --scoring.evict_weight 0.25 lands as a number and --http.token abc as a
// string. Numeric-looking strings that are meant to stay strings can be forced
// by quoting them in the config file instead.
Json infer(const std::string& text) {
    if (text == "true") return Json(true);
    if (text == "false") return Json(false);
    if (text == "null") return Json();
    if (!text.empty()) {
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end && *end == '\0') return Json(v);
    }
    return Json(text);
}

}  // namespace

bool Config::load_file(const std::string& path, std::string* err) {
    path_ = path;
    std::string text;
    if (!read_file(path, text)) {
        if (err) *err = "cannot read " + path;
        return false;
    }
    Json parsed;
    std::string perr;
    if (!Json::parse(text, parsed, &perr)) {
        if (err) *err = path + ": " + perr;
        return false;
    }
    if (!parsed.is_object()) {
        if (err) *err = path + ": top level must be an object";
        return false;
    }
    root_ = std::move(parsed);
    loaded_ = true;
    return true;
}

void Config::apply_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!starts_with(arg, "--")) {
            positionals_.push_back(std::move(arg));
            continue;
        }
        arg = arg.substr(2);
        const size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            set(arg.substr(0, eq), infer(arg.substr(eq + 1)));
            continue;
        }
        // A bare --flag not followed by a value is a boolean true.
        if (i + 1 < argc && !starts_with(argv[i + 1], "--")) {
            set(arg, infer(argv[++i]));
        } else {
            set(arg, Json(true));
        }
    }
}

const Json& Config::get(const std::string& key) const {
    const Json* cur = &root_;
    for (const auto& part : split(key, '.')) {
        if (!cur->is_object() || !cur->has(part)) return null_json();
        cur = &(*cur)[part];
    }
    return *cur;
}

void Config::set(const std::string& key, Json value) {
    const auto parts = split(key, '.');
    Json* cur = &root_;
    for (size_t i = 0; i + 1 < parts.size(); ++i) cur = &(*cur)[parts[i]];
    (*cur)[parts.back()] = std::move(value);
}

bool Config::has(const std::string& key) const { return !get(key).is_null(); }

std::string Config::get_str(const std::string& key, const std::string& def) const {
    const Json& j = get(key);
    if (j.is_str()) return j.as_str();
    // A value that came from the command line may have been inferred as a
    // number ("--node.id 01" -> 1). Render it back rather than dropping it.
    if (j.is_num()) return j.dump();
    return def;
}

double Config::get_num(const std::string& key, double def) const {
    return get(key).as_num(def);
}

int64_t Config::get_int(const std::string& key, int64_t def) const {
    return get(key).as_i64(def);
}

uint32_t Config::get_u32(const std::string& key, uint32_t def) const {
    return get(key).as_u32(def);
}

bool Config::get_bool(const std::string& key, bool def) const {
    const Json& j = get(key);
    if (j.is_bool()) return j.as_bool();
    if (j.is_num()) return j.as_num() != 0;
    if (j.is_str()) {
        const std::string s = lower(j.as_str());
        if (s == "true" || s == "yes" || s == "1") return true;
        if (s == "false" || s == "no" || s == "0") return false;
    }
    return def;
}

}  // namespace rf
