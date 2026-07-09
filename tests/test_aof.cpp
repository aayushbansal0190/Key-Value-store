#include "../src/aof.h"

#include <unistd.h>

#include <cstdio>
#include <string>

#include "../src/clock.h"
#include "../src/commands.h"
#include "../src/hashtable.h"
#include "test.h"

// Every test writes its own fresh log file and deletes it first, so runs
// never contaminate each other.
static const char* PATH = "test_aof.tmp";

static void reset_file() { remove(PATH); }

static void write_raw(const std::string& bytes) {
    FILE* f = fopen(PATH, "wb");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

static size_t file_size() {
    FILE* f = fopen(PATH, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (size_t)n;
}

// The core promise: run commands with logging on, load the log into a FRESH
// store, get identical state — including TTLs, deletes, and dead keys.
static void test_roundtrip_replay() {
    reset_file();
    {
        HashTable s1;
        Aof aof;
        CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s1));
        CHECK(execute_command(s1, {"SET", "a", "1"}, &aof) == "+OK\r\n");
        CHECK(execute_command(s1, {"SET", "b", "2", "EX", "100"}, &aof) == "+OK\r\n");
        CHECK(execute_command(s1, {"SET", "c", "3"}, &aof) == "+OK\r\n");
        CHECK(execute_command(s1, {"DEL", "c"}, &aof) == ":1\r\n");
        CHECK(execute_command(s1, {"SET", "d", "4", "PX", "1"}, &aof) == "+OK\r\n");
    }  // aof destructor closes + fsyncs

    usleep(5 * 1000);  // let d's 1ms deadline pass (absolute, so it stays dead)

    HashTable s2;
    Aof aof2;
    CHECK(aof2.open_and_load(PATH, FsyncPolicy::Always, s2));
    CHECK(execute_command(s2, {"GET", "a"}) == "$1\r\n1\r\n");
    CHECK(execute_command(s2, {"GET", "b"}) == "$1\r\n2\r\n");
    CHECK(execute_command(s2, {"TTL", "b"}) == ":100\r\n");   // TTL survived
    CHECK(execute_command(s2, {"GET", "c"}) == "$-1\r\n");    // DEL replayed
    CHECK(execute_command(s2, {"GET", "d"}) == "$-1\r\n");    // died while down
}

// GETs and failed writes must leave no trace in the log.
static void test_reads_and_noops_not_logged() {
    reset_file();
    {
        HashTable s;
        Aof aof;
        CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s));
        size_t empty = file_size();
        execute_command(s, {"GET", "nope"}, &aof);
        execute_command(s, {"DEL", "nope"}, &aof);          // deleted 0 keys
        execute_command(s, {"EXPIRE", "nope", "10"}, &aof);  // no such key
        execute_command(s, {"PERSIST", "nope"}, &aof);
        execute_command(s, {"DBSIZE"}, &aof);
        CHECK_EQ(file_size(), empty);  // not one byte appended
    }
}

// EXPIRE/PERSIST/PEXPIREAT round-trips, including EXPIRE<=0 -> logged DEL.
static void test_ttl_commands_replay() {
    reset_file();
    {
        HashTable s1;
        Aof aof;
        CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s1));
        execute_command(s1, {"SET", "k1", "v"}, &aof);
        execute_command(s1, {"EXPIRE", "k1", "500"}, &aof);
        execute_command(s1, {"SET", "k2", "v", "EX", "500"}, &aof);
        execute_command(s1, {"PERSIST", "k2"}, &aof);
        execute_command(s1, {"SET", "k3", "v"}, &aof);
        execute_command(s1, {"EXPIRE", "k3", "0"}, &aof);   // delete-now
        long long future = now_ms() + 500000;
        CHECK(execute_command(s1, {"PEXPIREAT", "k1", std::to_string(future)},
                              &aof) == ":1\r\n");
    }

    HashTable s2;
    Aof aof2;
    CHECK(aof2.open_and_load(PATH, FsyncPolicy::Always, s2));
    CHECK(execute_command(s2, {"TTL", "k1"}) == ":500\r\n");  // PEXPIREAT won
    CHECK(execute_command(s2, {"TTL", "k2"}) == ":-1\r\n");   // PERSIST survived
    CHECK(execute_command(s2, {"GET", "k3"}) == "$-1\r\n");   // EXPIRE 0 = DEL
    CHECK_EQ(s2.size(), 2u);
}

// Crash mid-append: the torn tail is dropped AND cut from the file, and
// appending afterwards produces a log that loads cleanly again.
static void test_truncated_tail() {
    reset_file();
    std::string good = "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n";
    std::string torn = "*3\r\n$3\r\nSET\r\n$1\r\nb";  // cut mid-command
    write_raw(good + torn);

    HashTable s;
    Aof aof;
    CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s));
    CHECK_EQ(s.size(), 1u);                  // the whole command loaded
    CHECK(execute_command(s, {"GET", "a"}) == "$1\r\n1\r\n");
    CHECK_EQ(file_size(), good.size());      // torn bytes physically removed

    // New history appends after the cut — and the file is coherent again.
    execute_command(s, {"SET", "b", "2"}, &aof);
    HashTable s2;
    Aof aof2;
    CHECK(aof2.open_and_load(PATH, FsyncPolicy::Always, s2));
    CHECK_EQ(s2.size(), 2u);
}

// Garbage in the MIDDLE of the log is not a crash artifact but corruption:
// refuse to start rather than run on a silently partial history.
static void test_corrupt_middle_refuses() {
    reset_file();
    write_raw("*1\r\n#bogus\r\n*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n");
    HashTable s;
    Aof aof;
    CHECK(!aof.open_and_load(PATH, FsyncPolicy::Always, s));
}

// A binary-unsafe log would corrupt values containing \r\n — RESP framing
// must round-trip them exactly.
static void test_binary_value_survives_replay() {
    reset_file();
    std::string val("bin\0\r\nary", 8);
    {
        HashTable s1;
        Aof aof;
        CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s1));
        execute_command(s1, {"SET", "bin", val}, &aof);
    }
    HashTable s2;
    Aof aof2;
    CHECK(aof2.open_and_load(PATH, FsyncPolicy::Always, s2));
    const std::string* got = s2.get("bin");
    CHECK(got && *got == val);
}

// FLUSHALL is a write: it must be logged and replay to the same "wiped at
// this point" history — keys set BEFORE the flush stay gone, keys set AFTER
// survive.
static void test_flushall_replay() {
    reset_file();
    {
        HashTable s1;
        Aof aof;
        CHECK(aof.open_and_load(PATH, FsyncPolicy::Always, s1));
        execute_command(s1, {"SET", "before", "x"}, &aof);
        execute_command(s1, {"FLUSHALL"}, &aof);
        execute_command(s1, {"SET", "after", "y"}, &aof);
    }
    HashTable s2;
    Aof aof2;
    CHECK(aof2.open_and_load(PATH, FsyncPolicy::Always, s2));
    CHECK(execute_command(s2, {"GET", "before"}) == "$-1\r\n");  // wiped, stays gone
    CHECK(execute_command(s2, {"GET", "after"}) == "$1\r\ny\r\n");
    CHECK_EQ(s2.size(), 1u);
}

int main() {
    test_roundtrip_replay();
    test_reads_and_noops_not_logged();
    test_ttl_commands_replay();
    test_truncated_tail();
    test_corrupt_middle_refuses();
    test_binary_value_survives_replay();
    test_flushall_replay();
    reset_file();
    return test_summary("test_aof");
}
