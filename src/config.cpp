#include "config.h"

#include <cctype>
#include <cstdlib>

const char* fsync_policy_name(FsyncPolicy p) {
    switch (p) {
        case FsyncPolicy::Always:   return "always";
        case FsyncPolicy::EverySec: return "everysec";
        case FsyncPolicy::No:       return "no";
    }
    return "?";  // unreachable, but keeps -Wall quiet
}

bool parse_memory_size(const std::string& text, uint64_t& out_bytes) {
    if (text.empty() || !isdigit(static_cast<unsigned char>(text[0]))) return false;

    // Parse the digit run ourselves rather than strtoull: strtoull silently
    // clamps on overflow, and we want overflow to be a hard config error.
    uint64_t value = 0;
    size_t i = 0;
    for (; i < text.size() && isdigit(static_cast<unsigned char>(text[i])); i++) {
        uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;  // would overflow
        value = value * 10 + digit;
    }

    std::string suffix;
    for (; i < text.size(); i++) {
        suffix += static_cast<char>(tolower(static_cast<unsigned char>(text[i])));
    }

    uint64_t multiplier = 1;
    if (suffix == "" || suffix == "b") multiplier = 1;
    else if (suffix == "kb") multiplier = 1024;
    else if (suffix == "mb") multiplier = 1024ull * 1024;
    else if (suffix == "gb") multiplier = 1024ull * 1024 * 1024;
    else return false;

    if (multiplier != 1 && value > UINT64_MAX / multiplier) return false;
    out_bytes = value * multiplier;
    return true;
}

// Parses "--port"-style int flags. Rejects trailing junk ("6379x") and range
// violations; plain atoi would accept both.
static bool parse_int(const std::string& text, int min, int max, int& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    long v = strtol(text.c_str(), &end, 10);
    if (*end != '\0') return false;
    if (v < min || v > max) return false;
    out = static_cast<int>(v);
    return true;
}

bool parse_config(int argc, char* const* argv, Config& cfg, std::string& err) {
    for (int i = 1; i < argc; i++) {
        std::string flag = argv[i];

        // Every flag takes exactly one value, so grab it up front.
        if (i + 1 >= argc) {
            err = "flag " + flag + " requires a value";
            return false;
        }
        std::string value = argv[++i];

        if (flag == "--port") {
            if (!parse_int(value, 1, 65535, cfg.port)) {
                err = "invalid --port '" + value + "' (want 1-65535)";
                return false;
            }
        } else if (flag == "--maxmemory") {
            if (!parse_memory_size(value, cfg.maxmemory)) {
                err = "invalid --maxmemory '" + value + "' (want e.g. 100mb, 1gb, 1048576)";
                return false;
            }
        } else if (flag == "--appendonly") {
            if (value == "yes")      cfg.appendonly = true;
            else if (value == "no")  cfg.appendonly = false;
            else {
                err = "invalid --appendonly '" + value + "' (want yes|no)";
                return false;
            }
        } else if (flag == "--appendfsync") {
            if (value == "always")        cfg.appendfsync = FsyncPolicy::Always;
            else if (value == "everysec") cfg.appendfsync = FsyncPolicy::EverySec;
            else if (value == "no")       cfg.appendfsync = FsyncPolicy::No;
            else {
                err = "invalid --appendfsync '" + value + "' (want always|everysec|no)";
                return false;
            }
        } else if (flag == "--dir") {
            if (value.empty()) {
                err = "--dir requires a non-empty path";
                return false;
            }
            cfg.dir = value;
        } else {
            err = "unknown flag " + flag;
            return false;
        }
    }
    return true;
}
