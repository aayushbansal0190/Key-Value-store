#include "../src/clock.h"
#include "../src/commands.h"
#include "test.h"

#include <string>
#include <vector>

static void test_split_inline() {
    auto args = split_inline("SET name aayush");
    CHECK_EQ(args.size(), 3u);
    CHECK(args[0] == "SET" && args[1] == "name" && args[2] == "aayush");

    CHECK(split_inline("").empty());
    CHECK(split_inline("   \t  ").empty());              // only whitespace
    CHECK_EQ(split_inline("  GET   key  ").size(), 2u);  // extra spaces collapse
}

static void test_ping_echo() {
    HashTable store;
    CHECK(execute_command(store, {"PING"}) == "+PONG\r\n");
    CHECK(execute_command(store, {"ping"}) == "+PONG\r\n");        // case-insensitive
    CHECK(execute_command(store, {"PING", "hi"}) == "$2\r\nhi\r\n");  // like Redis
    CHECK(execute_command(store, {"Echo", "hello"}) == "$5\r\nhello\r\n");
    CHECK(execute_command(store, {"PING", "a", "b"}).rfind("-ERR", 0) == 0);
    CHECK(execute_command(store, {"ECHO"}).rfind("-ERR", 0) == 0);
}

static void test_set_get_del() {
    HashTable store;
    CHECK(execute_command(store, {"SET", "name", "aayush"}) == "+OK\r\n");
    CHECK(execute_command(store, {"GET", "name"}) == "$6\r\naayush\r\n");
    CHECK(execute_command(store, {"GET", "missing"}) == "$-1\r\n");

    CHECK(execute_command(store, {"SET", "name", "bansal"}) == "+OK\r\n");  // overwrite
    CHECK(execute_command(store, {"GET", "name"}) == "$6\r\nbansal\r\n");

    CHECK(execute_command(store, {"DEL", "name"}) == ":1\r\n");
    CHECK(execute_command(store, {"GET", "name"}) == "$-1\r\n");
    CHECK(execute_command(store, {"DEL", "name"}) == ":0\r\n");    // already gone

    // Variadic DEL counts only keys that existed.
    execute_command(store, {"SET", "a", "1"});
    execute_command(store, {"SET", "b", "2"});
    CHECK(execute_command(store, {"DEL", "a", "b", "ghost"}) == ":2\r\n");
}

static void test_typed_nil_fixes_ambiguity() {
    HashTable store;
    // A literal value "(nil)" vs an actual miss — indistinguishable in
    // Phase 2's text replies, cleanly distinct in RESP.
    execute_command(store, {"SET", "tricky", "(nil)"});
    CHECK(execute_command(store, {"GET", "tricky"}) == "$5\r\n(nil)\r\n");
    CHECK(execute_command(store, {"GET", "absent"}) == "$-1\r\n");

    // Empty string is a real, storable value — distinct from nil too.
    execute_command(store, {"SET", "empty", ""});
    CHECK(execute_command(store, {"GET", "empty"}) == "$0\r\n\r\n");
}

static void test_errors() {
    HashTable store;
    CHECK(execute_command(store, {"NOSUCHCMD"}).rfind("-ERR unknown", 0) == 0);
    CHECK(execute_command(store, {"SET", "onlykey"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(execute_command(store, {"GET"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(execute_command(store, {"DEL"}).rfind("-ERR wrong number", 0) == 0);

    // Commands are case-insensitive; keys are NOT.
    execute_command(store, {"SET", "Key", "v"});
    CHECK(execute_command(store, {"GET", "key"}) == "$-1\r\n");
    CHECK(execute_command(store, {"GET", "Key"}) == "$1\r\nv\r\n");
}

static void test_set_with_expiry_options() {
    HashTable store;
    // EX/PX accepted, case-insensitive, and TTL reports remaining seconds.
    CHECK(execute_command(store, {"SET", "s", "v", "EX", "100"}) == "+OK\r\n");
    CHECK(execute_command(store, {"TTL", "s"}) == ":100\r\n");
    CHECK(execute_command(store, {"SET", "p", "v", "px", "100000"}) == "+OK\r\n");
    CHECK(execute_command(store, {"TTL", "p"}) == ":100\r\n");

    // Plain SET clears an existing TTL (the classic Redis gotcha).
    CHECK(execute_command(store, {"SET", "s", "v2"}) == "+OK\r\n");
    CHECK(execute_command(store, {"TTL", "s"}) == ":-1\r\n");

    // Rejections, matching Redis's messages.
    CHECK(execute_command(store, {"SET", "k", "v", "EX", "0"}).rfind("-ERR invalid expire", 0) == 0);
    CHECK(execute_command(store, {"SET", "k", "v", "EX", "-5"}).rfind("-ERR invalid expire", 0) == 0);
    CHECK(execute_command(store, {"SET", "k", "v", "EX", "abc"}).rfind("-ERR value is not an integer", 0) == 0);
    CHECK(execute_command(store, {"SET", "k", "v", "EX"}) == "-ERR syntax error\r\n");           // number missing
    CHECK(execute_command(store, {"SET", "k", "v", "EX", "9", "PX", "9"}) == "-ERR syntax error\r\n");  // both options
    CHECK(execute_command(store, {"SET", "k", "v", "BOGUS"}) == "-ERR syntax error\r\n");
}

static void test_expire_ttl_persist() {
    HashTable store;
    execute_command(store, {"SET", "k", "v"});

    CHECK(execute_command(store, {"TTL", "k"}) == ":-1\r\n");        // exists, immortal
    CHECK(execute_command(store, {"TTL", "ghost"}) == ":-2\r\n");    // doesn't exist

    CHECK(execute_command(store, {"EXPIRE", "k", "50"}) == ":1\r\n");
    CHECK(execute_command(store, {"TTL", "k"}) == ":50\r\n");
    CHECK(execute_command(store, {"EXPIRE", "ghost", "50"}) == ":0\r\n");  // no key, no-op

    CHECK(execute_command(store, {"PERSIST", "k"}) == ":1\r\n");     // TTL removed
    CHECK(execute_command(store, {"TTL", "k"}) == ":-1\r\n");
    CHECK(execute_command(store, {"PERSIST", "k"}) == ":0\r\n");     // nothing to remove
    CHECK(execute_command(store, {"PERSIST", "ghost"}) == ":0\r\n");

    // EXPIRE with <=0 seconds = delete right now, report success.
    CHECK(execute_command(store, {"EXPIRE", "k", "0"}) == ":1\r\n");
    CHECK(execute_command(store, {"GET", "k"}) == "$-1\r\n");
    CHECK(execute_command(store, {"EXPIRE", "k", "-1"}) == ":0\r\n");  // already gone

    CHECK(execute_command(store, {"EXPIRE", "k"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(execute_command(store, {"EXPIRE", "k", "x"}).rfind("-ERR value is not an integer", 0) == 0);
}

static void test_dbsize() {
    HashTable store;
    CHECK(execute_command(store, {"DBSIZE"}) == ":0\r\n");
    execute_command(store, {"SET", "a", "1"});
    execute_command(store, {"SET", "b", "2"});
    CHECK(execute_command(store, {"DBSIZE"}) == ":2\r\n");
    execute_command(store, {"DEL", "a"});
    CHECK(execute_command(store, {"DBSIZE"}) == ":1\r\n");
}

static void test_pexpireat() {
    HashTable store;
    execute_command(store, {"SET", "k", "v"});
    long long future = now_ms() + 50000;
    CHECK(execute_command(store, {"PEXPIREAT", "k", std::to_string(future)}) == ":1\r\n");
    CHECK(execute_command(store, {"TTL", "k"}) == ":50\r\n");
    CHECK(execute_command(store, {"PEXPIREAT", "ghost", "123"}) == ":0\r\n");

    // A deadline in the past makes the key dead immediately (reaped on touch).
    CHECK(execute_command(store, {"PEXPIREAT", "k", "1"}) == ":1\r\n");
    CHECK(execute_command(store, {"GET", "k"}) == "$-1\r\n");

    CHECK(execute_command(store, {"PEXPIREAT", "k"}).rfind("-ERR wrong number", 0) == 0);
    CHECK(execute_command(store, {"PEXPIREAT", "k", "x"}).rfind("-ERR value is not", 0) == 0);
}

static void test_info() {
    HashTable store;
    std::string r = execute_command(store, {"INFO"});
    CHECK(r.rfind("$", 0) == 0);  // one RESP bulk string, like Redis
    CHECK(r.find("used_memory:") != std::string::npos);
    CHECK(r.find("maxmemory:0") != std::string::npos);
    CHECK(r.find("evicted_keys:0") != std::string::npos);
    CHECK(r.find("db0:") == std::string::npos);  // empty keyspace is omitted

    execute_command(store, {"SET", "a", "1"});
    r = execute_command(store, {"info"});        // case-insensitive
    CHECK(r.find("db0:keys=1") != std::string::npos);

    CHECK(execute_command(store, {"INFO", "x"}).rfind("-ERR wrong number", 0) == 0);
}

static void test_exists() {
    HashTable store;
    execute_command(store, {"SET", "a", "1"});
    execute_command(store, {"SET", "b", "2"});
    CHECK(execute_command(store, {"EXISTS", "a"}) == ":1\r\n");
    CHECK(execute_command(store, {"EXISTS", "ghost"}) == ":0\r\n");
    // Variadic, counts each occurrence (a present twice = 2), ghost = 0.
    CHECK(execute_command(store, {"EXISTS", "a", "b", "a", "ghost"}) == ":3\r\n");
    // A key with a past deadline reads as absent (lazy expiry through find()).
    execute_command(store, {"PEXPIREAT", "a", "1"});
    CHECK(execute_command(store, {"EXISTS", "a"}) == ":0\r\n");
    CHECK(execute_command(store, {"EXISTS"}).rfind("-ERR wrong number", 0) == 0);
}

static void test_keys_glob() {
    HashTable store;
    execute_command(store, {"SET", "user:1", "a"});
    execute_command(store, {"SET", "user:2", "b"});
    execute_command(store, {"SET", "session:1", "c"});

    // '*' matches everything: three keys back (order unspecified).
    std::string all = execute_command(store, {"KEYS", "*"});
    CHECK(all.rfind("*3\r\n", 0) == 0);

    // Prefix glob: only the two user: keys.
    std::string users = execute_command(store, {"KEYS", "user:*"});
    CHECK(users.rfind("*2\r\n", 0) == 0);
    CHECK(users.find("user:1") != std::string::npos);
    CHECK(users.find("user:2") != std::string::npos);
    CHECK(users.find("session:1") == std::string::npos);

    // '?' matches exactly one char.
    std::string q = execute_command(store, {"KEYS", "user:?"});
    CHECK(q.rfind("*2\r\n", 0) == 0);

    // No match: empty array.
    CHECK(execute_command(store, {"KEYS", "nope*"}) == "*0\r\n");
    CHECK(execute_command(store, {"KEYS"}).rfind("-ERR wrong number", 0) == 0);
}

static void test_flushall() {
    HashTable store;
    execute_command(store, {"SET", "a", "1"});
    execute_command(store, {"SET", "b", "2"});
    CHECK(execute_command(store, {"DBSIZE"}) == ":2\r\n");
    CHECK(execute_command(store, {"FLUSHALL"}) == "+OK\r\n");
    CHECK(execute_command(store, {"DBSIZE"}) == ":0\r\n");
    CHECK(execute_command(store, {"GET", "a"}) == "$-1\r\n");
    // Store is usable again after a flush.
    CHECK(execute_command(store, {"SET", "c", "3"}) == "+OK\r\n");
    CHECK(execute_command(store, {"DBSIZE"}) == ":1\r\n");
    CHECK(execute_command(store, {"FLUSHALL", "x"}).rfind("-ERR wrong number", 0) == 0);
}

static void test_oversized_write_rejected() {
    HashTable store;
    // Budget smaller than a single entry's fixed overhead: any SET is refused
    // with OOM, and nothing is stored (non-destructive).
    store.set_maxmemory(8);
    std::string r = execute_command(store, {"SET", "k", "value"});
    CHECK(r.rfind("-OOM", 0) == 0);
    CHECK(execute_command(store, {"DBSIZE"}) == ":0\r\n");
    // With no budget the same write succeeds.
    HashTable store2;
    CHECK(execute_command(store2, {"SET", "k", "value"}) == "+OK\r\n");
}

static void test_info_fields() {
    HashTable store;
    ServerStats stats;
    stats.start_ms = now_ms();
    stats.connected_clients = 3;
    stats.aof_enabled = true;
    execute_command(store, {"SET", "a", "1"}, nullptr, &stats);  // bumps total_commands
    std::string r = execute_command(store, {"INFO"}, nullptr, &stats);
    CHECK(r.find("uptime_in_seconds:") != std::string::npos);
    CHECK(r.find("connected_clients:3") != std::string::npos);
    CHECK(r.find("aof_enabled:1") != std::string::npos);
    CHECK(r.find("maxmemory_policy:noeviction") != std::string::npos);
    CHECK(r.find("expired_keys:0") != std::string::npos);
    CHECK(r.find("total_commands_processed:") != std::string::npos);

    // Without stats, the server/clients numbers are simply absent (unit-test
    // path) but the store-level lines still appear.
    std::string bare = execute_command(store, {"INFO"});
    CHECK(bare.find("connected_clients:") == std::string::npos);
    CHECK(bare.find("used_memory:") != std::string::npos);
}

static void test_eviction_visible_through_commands() {
    HashTable store;
    execute_command(store, {"SET", "k1", "v1"});
    execute_command(store, {"SET", "k2", "v2"});
    execute_command(store, {"SET", "k3", "v3"});
    store.set_maxmemory(store.used_memory());  // exactly three entries fit

    execute_command(store, {"GET", "k1"});     // touch: k2 becomes the LRU
    CHECK(execute_command(store, {"SET", "k4", "v4"}) == "+OK\r\n");

    CHECK(execute_command(store, {"DBSIZE"}) == ":3\r\n");
    CHECK(execute_command(store, {"GET", "k2"}) == "$-1\r\n");  // evicted
    CHECK(execute_command(store, {"GET", "k1"}) == "$2\r\nv1\r\n");
    std::string r = execute_command(store, {"INFO"});
    CHECK(r.find("evicted_keys:1") != std::string::npos);
}

int main() {
    test_split_inline();
    test_ping_echo();
    test_set_get_del();
    test_typed_nil_fixes_ambiguity();
    test_errors();
    test_set_with_expiry_options();
    test_expire_ttl_persist();
    test_pexpireat();
    test_dbsize();
    test_info();
    test_exists();
    test_keys_glob();
    test_flushall();
    test_oversized_write_rejected();
    test_info_fields();
    test_eviction_visible_through_commands();
    return test_summary("test_commands");
}
