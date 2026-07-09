#pragma once

#include <cstdint>
#include <string>

// When to fsync the append-only file (Phase 7 uses this; parsed/validated now
// so a typo in the flag kills the server at startup, not at first write).
enum class FsyncPolicy { Always, EverySec, No };

struct Config {
    int port = 6379;               // Redis's default port, so redis-cli "just works"
    uint64_t maxmemory = 0;        // byte budget for eviction; 0 = unlimited
    bool appendonly = false;
    FsyncPolicy appendfsync = FsyncPolicy::EverySec;
    std::string dir = ".";         // where the AOF lives
};

// Parses command-line flags into cfg (fields not mentioned keep their defaults).
// Returns false and fills err on any unknown flag, missing value, or bad value.
// Deliberately hand-rolled instead of getopt_long: 5 flags don't justify its
// global state (optind/optarg), and this stays trivially unit-testable.
bool parse_config(int argc, char* const* argv, Config& cfg, std::string& err);

const char* fsync_policy_name(FsyncPolicy p);

// "100", "64kb", "512mb", "2gb" -> bytes. Returns false on garbage or overflow.
// Exposed for tests.
bool parse_memory_size(const std::string& text, uint64_t& out_bytes);
