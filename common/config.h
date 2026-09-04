// RouteFlow — JSON config file with command-line overrides.
//
// Precedence: defaults < config file < command line. Keys are dotted paths into
// the JSON tree, so any setting can be overridden from the command line without
// the config struct knowing about it: --scoring.evict_weight 0.25
#pragma once

#include <string>
#include <vector>

#include "json.h"

namespace rf {

class Config {
public:
    // Missing file is not an error: the caller decides whether that is fatal.
    bool load_file(const std::string& path, std::string* err);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    // --key value, --key=value, --flag (=> true). Unknown keys are kept, so a
    // typo shows up in `dump()` rather than being silently dropped.
    //
    // A bare --key consumes the next token as its value unless that token
    // starts with "--" or the arguments end. So `--verbose run.json` sets
    // verbose="run.json" and leaves no positional; write `--verbose=true
    // run.json` when you mean a flag followed by a positional. Everything not
    // consumed as a value is a positional.
    void apply_args(int argc, char** argv);
    const std::vector<std::string>& positionals() const { return positionals_; }

    bool has(const std::string& dotted_key) const;
    std::string get_str(const std::string& key, const std::string& def = {}) const;
    double get_num(const std::string& key, double def = 0) const;
    int64_t get_int(const std::string& key, int64_t def = 0) const;
    uint32_t get_u32(const std::string& key, uint32_t def = 0) const;
    bool get_bool(const std::string& key, bool def = false) const;
    const Json& get(const std::string& key) const;

    void set(const std::string& key, Json value);

    const Json& root() const { return root_; }
    std::string dump() const { return root_.dump(2); }

private:
    Json root_ = Json::object();
    std::string path_;
    bool loaded_ = false;
    std::vector<std::string> positionals_;
};

}  // namespace rf
