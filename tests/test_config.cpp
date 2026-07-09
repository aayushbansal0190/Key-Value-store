#include "../src/config.h"
#include "test.h"

#include <cstdint>
#include <vector>

// Builds a fake argv (argv[0] = program name) and parses it.
static bool parse(std::vector<const char*> args, Config& cfg, std::string& err) {
    args.insert(args.begin(), "kvstore");
    return parse_config(static_cast<int>(args.size()),
                        const_cast<char* const*>(args.data()), cfg, err);
}

static void test_defaults() {
    Config cfg;
    std::string err;
    CHECK(parse({}, cfg, err));
    CHECK_EQ(cfg.port, 6379);
    CHECK_EQ(cfg.maxmemory, 0u);
    CHECK_EQ(cfg.appendonly, false);
    CHECK(cfg.appendfsync == FsyncPolicy::EverySec);
    CHECK(cfg.dir == ".");
}

static void test_all_flags() {
    Config cfg;
    std::string err;
    CHECK(parse({"--port", "7000", "--maxmemory", "100mb", "--appendonly", "yes",
                 "--appendfsync", "always", "--dir", "/var/lib/kvstore"},
                cfg, err));
    CHECK_EQ(cfg.port, 7000);
    CHECK_EQ(cfg.maxmemory, 100ull * 1024 * 1024);
    CHECK_EQ(cfg.appendonly, true);
    CHECK(cfg.appendfsync == FsyncPolicy::Always);
    CHECK(cfg.dir == "/var/lib/kvstore");
}

static void test_bad_input_rejected() {
    Config cfg;
    std::string err;
    CHECK(!parse({"--port", "0"}, cfg, err));          // out of range
    CHECK(!parse({"--port", "70000"}, cfg, err));      // out of range
    CHECK(!parse({"--port", "6379x"}, cfg, err));      // trailing junk
    CHECK(!parse({"--port"}, cfg, err));               // missing value
    CHECK(!parse({"--appendonly", "maybe"}, cfg, err));
    CHECK(!parse({"--appendfsync", "sometimes"}, cfg, err));
    CHECK(!parse({"--dir", ""}, cfg, err));
    CHECK(!parse({"--no-such-flag", "x"}, cfg, err));
    CHECK(!err.empty());  // errors come with a message
}

static void test_memory_size_parsing() {
    uint64_t bytes = 0;
    CHECK(parse_memory_size("0", bytes) && bytes == 0);
    CHECK(parse_memory_size("1048576", bytes) && bytes == 1048576);
    CHECK(parse_memory_size("64kb", bytes) && bytes == 64 * 1024);
    CHECK(parse_memory_size("512MB", bytes) && bytes == 512ull * 1024 * 1024);
    CHECK(parse_memory_size("2gb", bytes) && bytes == 2ull * 1024 * 1024 * 1024);
    CHECK(parse_memory_size("10b", bytes) && bytes == 10);

    CHECK(!parse_memory_size("", bytes));
    CHECK(!parse_memory_size("abc", bytes));
    CHECK(!parse_memory_size("10tb", bytes));                    // unsupported suffix
    CHECK(!parse_memory_size("-5", bytes));                      // negative
    CHECK(!parse_memory_size("99999999999999999999", bytes));    // > UINT64_MAX
    CHECK(!parse_memory_size("18446744073709551615gb", bytes));  // multiplier overflow
}

int main() {
    test_defaults();
    test_all_flags();
    test_bad_input_rejected();
    test_memory_size_parsing();
    return test_summary("test_config");
}
