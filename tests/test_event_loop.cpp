// Integration test for the epoll server (Linux only, like the server):
// forks a real kvstore process, then attacks it with concurrent clients.
// Threads are fine HERE — this is a client tool; the server stays 1-thread.
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static const int PORT = 7900;

// ---- tiny blocking client, RESP-aware ----

struct Client {
    int fd = -1;
    std::string buf;

    bool connect_to(int port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, (sockaddr*)&addr, sizeof addr) < 0) {
            close(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    bool send_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    bool fill(size_t need) {  // ensure buf holds at least `need` bytes
        while (buf.size() < need) {
            char chunk[4096];
            ssize_t n = recv(fd, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            buf.append(chunk, (size_t)n);
        }
        return true;
    }

    bool read_line(std::string& out) {
        size_t nl;
        while ((nl = buf.find('\n')) == std::string::npos) {
            if (!fill(buf.size() + 1)) return false;
        }
        out = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    // Reads one typed RESP reply, normalized to a comparable string:
    //   +OK -> "OK"   :2 -> "2"   $-1 -> "(nil)"   $N data -> data
    //   -ERR ... -> "-ERR ..." (kept prefixed so errors are recognizable)
    bool read_reply(std::string& out) {
        std::string line;
        if (!read_line(line) || line.empty()) return false;
        switch (line[0]) {
            case '+': out = line.substr(1); return true;
            case ':': out = line.substr(1); return true;
            case '-': out = line; return true;
            case '$': {
                long long len = atoll(line.c_str() + 1);
                if (len == -1) {
                    out = "(nil)";
                    return true;
                }
                if (!fill((size_t)len + 2)) return false;  // data + CRLF
                out = buf.substr(0, (size_t)len);
                buf.erase(0, (size_t)len + 2);
                return true;
            }
            default: return false;
        }
    }

    bool roundtrip(const std::string& cmd, std::string& reply) {
        return send_all(cmd + "\r\n") && read_reply(reply);
    }

    ~Client() {
        if (fd >= 0) close(fd);
    }
};

// Encodes args as a RESP command, exactly as redis-cli would send it.
static std::string resp_encode(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    }
    return out;
}

// ---- server process management ----

static pid_t start_server(int port, std::vector<std::string> extra_flags = {}) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: silence the per-connection logs, become the server.
        if (!freopen("/dev/null", "w", stdout)) _exit(126);
        const char* bin = getenv("KVSTORE_BIN");
        if (!bin) bin = "./build/kvstore";
        std::vector<std::string> args = {bin, "--port", std::to_string(port)};
        for (const auto& f : extra_flags) args.push_back(f);
        std::vector<char*> argv;  // execv wants a null-terminated char* array
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(bin, argv.data());
        _exit(127);  // exec failed
    }
    return pid;
}

static bool wait_until_ready(int port) {
    for (int i = 0; i < 100; i++) {  // up to ~5s
        Client c;
        if (c.connect_to(port)) {
            std::string r;
            if (c.roundtrip("PING", r) && r == "PONG") return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

// ---- the tests ----

// THE MILESTONE: 50 clients in parallel, each doing its own SET/GET/DEL
// cycles on its own keys, verifying every single reply.
static void test_50_parallel_clients() {
    const int CLIENTS = 50;
    const int ROUNDS = 100;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < CLIENTS; t++) {
        threads.emplace_back([t, &failures]() {
            Client c;
            if (!c.connect_to(PORT)) {
                failures++;
                return;
            }
            for (int i = 0; i < ROUNDS; i++) {
                std::string key = "k:" + std::to_string(t) + ":" + std::to_string(i);
                std::string val = "v:" + std::to_string(t * 100000 + i);
                std::string r;

                // Alternate protocols: even rounds RESP, odd rounds inline —
                // both must work interleaved on one connection.
                bool ok;
                if (i % 2 == 0) {
                    ok = c.send_all(resp_encode({"SET", key, val})) && c.read_reply(r);
                } else {
                    ok = c.roundtrip("SET " + key + " " + val, r);
                }
                if (!ok || r != "OK") { failures++; continue; }

                if (!c.roundtrip("GET " + key, r) || r != val) { failures++; continue; }
                if (i % 10 == 0) {
                    if (!c.roundtrip("DEL " + key, r) || r != "1") { failures++; continue; }
                    if (!c.roundtrip("GET " + key, r) || r != "(nil)") { failures++; continue; }
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
}

// Pipelining: several commands in ONE send — mixed RESP and inline — must
// yield exactly that many replies, in order.
static void test_pipelined_commands() {
    Client c;
    CHECK(c.connect_to(PORT));
    std::string batch = resp_encode({"SET", "pk", "pv"}) + "PING\r\n" +
                        resp_encode({"GET", "pk"});
    CHECK(c.send_all(batch));
    std::string r;
    CHECK(c.read_reply(r) && r == "OK");
    CHECK(c.read_reply(r) && r == "PONG");
    CHECK(c.read_reply(r) && r == "pv");
}

// A RESP command delivered one byte at a time: the parser must answer
// Incomplete at every split and fire exactly once at the final byte —
// while a second client gets instant service between the bytes.
static void test_resp_split_across_recvs() {
    Client slow, normal;
    CHECK(slow.connect_to(PORT));
    CHECK(normal.connect_to(PORT));

    std::string cmd = resp_encode({"SET", "slowkey", "slow val\r\nwith crlf"});
    for (char ch : cmd) {
        CHECK(slow.send_all(std::string(1, ch)));
        std::string r;
        CHECK(normal.roundtrip("PING", r) && r == "PONG");
    }
    std::string r;
    CHECK(slow.read_reply(r) && r == "OK");

    // The stored value really contains \r\n — only RESP can carry that.
    CHECK(slow.send_all(resp_encode({"GET", "slowkey"})));
    CHECK(slow.read_reply(r) && r == "slow val\r\nwith crlf");
}

// Binary safety end to end: a value with an embedded NUL byte survives the
// round trip exactly.
static void test_binary_value_roundtrip() {
    Client c;
    CHECK(c.connect_to(PORT));
    std::string val("bin\0ary\r\ndata", 13);
    std::string r;
    CHECK(c.send_all(resp_encode({"SET", "binkey", val})));
    CHECK(c.read_reply(r) && r == "OK");
    CHECK(c.send_all(resp_encode({"GET", "binkey"})));
    CHECK(c.read_reply(r));
    CHECK_EQ(r.size(), val.size());
    CHECK(r == val);
}

// A 512 KB value: the reply cannot fit the kernel's send buffer in one
// go, forcing the outbuf + EPOLLOUT partial-write path to actually run.
static void test_large_value_partial_writes() {
    Client c;
    CHECK(c.connect_to(PORT));
    std::string big(512 * 1024, 'x');
    std::string r;
    CHECK(c.send_all(resp_encode({"SET", "big", big})));
    CHECK(c.read_reply(r) && r == "OK");
    CHECK(c.send_all(resp_encode({"GET", "big"})));
    CHECK(c.read_reply(r));
    CHECK_EQ(r.size(), big.size());
    CHECK(r == big);
    CHECK(c.roundtrip("DEL big", r) && r == "1");
}

// Protocol errors: the server must reply with -ERR and close THIS
// connection — while other connections keep working untouched.
static void test_protocol_error_closes_connection() {
    Client bad, good;
    CHECK(bad.connect_to(PORT));
    CHECK(good.connect_to(PORT));

    CHECK(bad.send_all("*2\r\n#oops\r\n"));  // '$' expected, '#' sent
    std::string r;
    CHECK(bad.read_reply(r));
    CHECK(r.rfind("-ERR Protocol error", 0) == 0);
    CHECK(!bad.read_line(r));  // then the server hangs up on us

    CHECK(good.roundtrip("PING", r) && r == "PONG");  // bystander unaffected
}

// TTL over the network, on a FRESH server (port 7901): the active sweep
// samples random buckets, so on the main server's thousands of keys the
// few expired ones could hide for a while — a small table makes every
// sweep round cover all buckets, making this test deterministic.
static void test_ttl_lazy_and_active_over_network() {
    const int TTL_PORT = 7901;
    pid_t server = start_server(TTL_PORT);
    if (!wait_until_ready(TTL_PORT)) {
        fprintf(stderr, "FAIL: ttl test server did not come up\n");
        kill(server, SIGKILL);
        g_checks_failed++;
        return;
    }

    Client c;
    CHECK(c.connect_to(TTL_PORT));
    std::string r;

    // LAZY path: an expired key reads as gone the moment it's touched.
    CHECK(c.roundtrip("SET lazy v PX 300", r) && r == "OK");
    CHECK(c.roundtrip("GET lazy", r) && r == "v");   // still alive
    usleep(450 * 1000);
    CHECK(c.roundtrip("GET lazy", r) && r == "(nil)");

    // ACTIVE path: keys expire WITHOUT ever being touched again — only the
    // 100ms tick's sweep can reap them, observable via DBSIZE shrinking.
    CHECK(c.roundtrip("SET stay1 v", r) && r == "OK");
    CHECK(c.roundtrip("SET stay2 v", r) && r == "OK");
    for (int i = 0; i < 5; i++) {
        CHECK(c.roundtrip("SET gone" + std::to_string(i) + " v PX 200", r) && r == "OK");
    }
    CHECK(c.roundtrip("DBSIZE", r) && r == "7");
    usleep(900 * 1000);  // several ticks after the 200ms deaths
    CHECK(c.roundtrip("DBSIZE", r) && r == "2");     // swept, untouched
    CHECK(c.roundtrip("GET stay1", r) && r == "v");  // the immortals live

    // TTL/EXPIRE/PERSIST round-trip sanity via RESP integers.
    CHECK(c.roundtrip("EXPIRE stay1 100", r) && r == "1");
    CHECK(c.roundtrip("TTL stay1", r) && r == "100");
    CHECK(c.roundtrip("PERSIST stay1", r) && r == "1");
    CHECK(c.roundtrip("TTL stay1", r) && r == "-1");

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
}

// Eviction over the network: a server with a real --maxmemory budget gets
// far more data than fits. True LRU makes the outcome predictable: the keys
// written first (and never touched again) die, the newest survive.
static void test_maxmemory_eviction_over_network() {
    const int EVICT_PORT = 7902;
    pid_t server = start_server(EVICT_PORT, {"--maxmemory", "16kb"});
    if (!wait_until_ready(EVICT_PORT)) {
        fprintf(stderr, "FAIL: eviction test server did not come up\n");
        kill(server, SIGKILL);
        g_checks_failed++;
        return;
    }

    Client c;
    CHECK(c.connect_to(EVICT_PORT));
    std::string r;
    std::string val(100, 'x');

    // ~200 bytes/entry x 200 keys ≈ 40kb into a 16kb budget.
    for (int i = 0; i < 200; i++) {
        CHECK(c.roundtrip("SET key:" + std::to_string(i) + " " + val, r) && r == "OK");
    }

    CHECK(c.roundtrip("DBSIZE", r));
    long long remaining = atoll(r.c_str());
    CHECK(remaining > 0 && remaining < 200);  // evictions happened, store lives

    CHECK(c.roundtrip("GET key:199", r) && r == val);      // newest survives
    CHECK(c.roundtrip("GET key:0", r) && r == "(nil)");    // oldest evicted

    // INFO must report the budget and a nonzero eviction count.
    CHECK(c.send_all(resp_encode({"INFO"})));
    CHECK(c.read_reply(r));
    CHECK(r.find("maxmemory:16384") != std::string::npos);
    CHECK(r.find("evicted_keys:") != std::string::npos);
    CHECK(r.find("evicted_keys:0\r\n") == std::string::npos);

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
}

// Phase 8 commands over the real event loop, plus the ServerStats plumbing
// (connected_clients) that only exists on the network path.
static void test_phase8_commands_over_network() {
    const int P8_PORT = 7903;
    pid_t server = start_server(P8_PORT);
    if (!wait_until_ready(P8_PORT)) {
        fprintf(stderr, "FAIL: phase8 test server did not come up\n");
        kill(server, SIGKILL);
        g_checks_failed++;
        return;
    }

    Client c;
    CHECK(c.connect_to(P8_PORT));
    std::string r;

    CHECK(c.roundtrip("SET a 1", r) && r == "OK");
    CHECK(c.roundtrip("SET b 2", r) && r == "OK");
    CHECK(c.roundtrip("EXISTS a", r) && r == "1");
    CHECK(c.roundtrip("EXISTS ghost", r) && r == "0");
    CHECK(c.roundtrip("EXISTS a b a", r) && r == "3");  // variadic, per-hit

    // INFO reports live connections (this one) and the AOF-disabled flag —
    // both come from ServerStats, which only the event loop populates.
    CHECK(c.send_all(resp_encode({"INFO"})));
    CHECK(c.read_reply(r));
    CHECK(r.find("connected_clients:1") != std::string::npos);
    CHECK(r.find("aof_enabled:0") != std::string::npos);
    CHECK(r.find("total_commands_processed:") != std::string::npos);

    // FLUSHALL wipes; the store keeps serving afterward.
    CHECK(c.roundtrip("FLUSHALL", r) && r == "OK");
    CHECK(c.roundtrip("DBSIZE", r) && r == "0");
    CHECK(c.roundtrip("GET a", r) && r == "(nil)");
    CHECK(c.roundtrip("SET c 3", r) && r == "OK");
    CHECK(c.roundtrip("DBSIZE", r) && r == "1");

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);
}

// THE PHASE 7 MILESTONE: kill -9 the server (no cleanup, no flush, the
// process just ceases), restart it on the same AOF, and the data is back.
// fsync=always so even the very last write is on disk when SIGKILL lands
// (with everysec this test could legitimately lose the final second).
static void test_aof_survives_kill9() {
    const int AOF_PORT = 7904;
    int rc = system("rm -rf aof_test_dir && mkdir -p aof_test_dir");
    CHECK_EQ(rc, 0);
    std::vector<std::string> flags = {"--appendonly", "yes", "--appendfsync",
                                      "always", "--dir", "aof_test_dir"};

    // ---- life 1: write some state, then die without warning ----
    pid_t s1 = start_server(AOF_PORT, flags);
    if (!wait_until_ready(AOF_PORT)) {
        fprintf(stderr, "FAIL: aof test server did not come up\n");
        kill(s1, SIGKILL);
        g_checks_failed++;
        return;
    }
    {
        Client c;
        CHECK(c.connect_to(AOF_PORT));
        std::string r;
        CHECK(c.roundtrip("SET durable hello", r) && r == "OK");
        CHECK(c.roundtrip("SET session x EX 1000", r) && r == "OK");
        CHECK(c.roundtrip("SET shortlived y PX 100", r) && r == "OK");
        CHECK(c.roundtrip("SET doomed z", r) && r == "OK");
        CHECK(c.roundtrip("DEL doomed", r) && r == "1");
    }
    kill(s1, SIGKILL);  // kill -9: no destructors, no fsync, nothing
    waitpid(s1, nullptr, 0);
    usleep(300 * 1000);  // let shortlived's 100ms deadline pass while DOWN

    // ---- life 2: replay, verify, write MORE, die again ----
    pid_t s2 = start_server(AOF_PORT, flags);
    CHECK(wait_until_ready(AOF_PORT));
    {
        Client c;
        CHECK(c.connect_to(AOF_PORT));
        std::string r;
        CHECK(c.roundtrip("GET durable", r) && r == "hello");   // survived
        CHECK(c.roundtrip("GET doomed", r) && r == "(nil)");    // DEL survived
        CHECK(c.roundtrip("GET shortlived", r) && r == "(nil)"); // died offline
        CHECK(c.roundtrip("TTL session", r));                   // TTL survived,
        long long ttl = atoll(r.c_str());                        // still counting
        CHECK(ttl > 900 && ttl <= 1000);
        CHECK(c.roundtrip("SET second-life ok", r) && r == "OK");
    }
    kill(s2, SIGKILL);
    waitpid(s2, nullptr, 0);

    // ---- life 3: appends made AFTER a replay must also persist ----
    pid_t s3 = start_server(AOF_PORT, flags);
    CHECK(wait_until_ready(AOF_PORT));
    {
        Client c;
        CHECK(c.connect_to(AOF_PORT));
        std::string r;
        CHECK(c.roundtrip("GET durable", r) && r == "hello");
        CHECK(c.roundtrip("GET second-life", r) && r == "ok");
    }
    kill(s3, SIGTERM);
    waitpid(s3, nullptr, 0);
    rc = system("rm -rf aof_test_dir");
    CHECK_EQ(rc, 0);
}

int main() {
    pid_t server = start_server(PORT);
    if (!wait_until_ready(PORT)) {
        fprintf(stderr, "FAIL test_event_loop: server did not come up\n");
        kill(server, SIGKILL);
        return 1;
    }

    test_pipelined_commands();
    test_resp_split_across_recvs();
    test_binary_value_roundtrip();
    test_large_value_partial_writes();
    test_protocol_error_closes_connection();
    test_50_parallel_clients();

    kill(server, SIGTERM);
    waitpid(server, nullptr, 0);

    test_ttl_lazy_and_active_over_network();
    test_maxmemory_eviction_over_network();
    test_phase8_commands_over_network();
    test_aof_survives_kill9();

    return test_summary("test_event_loop");
}
