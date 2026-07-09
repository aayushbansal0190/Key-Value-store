// loadgen — a small threaded load generator for kvstore (Phase 8).
//
// This is a CLIENT, so unlike the server it MAY use threads: it spins up N
// connections, each on its own thread, and hammers the server so we can
// report ops/sec and tail-latency (p50/p99/max) numbers.
//
// Design notes:
//  - Each thread owns one TCP connection and a private latency vector, so
//    there is zero shared mutable state on the hot path (no locks, no false
//    sharing) — the merge happens once at the end.
//  - Requests are built as raw RESP, the same bytes redis-cli sends.
//  - --pipeline P sends P requests before reading P replies (like
//    redis-benchmark -P): it amortizes syscall + round-trip cost and shows
//    the server's real throughput ceiling. P=1 measures true per-op latency.
//
// Build: see the `bench` / `bench-run` targets in the Makefile.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 6379;
    int clients = 50;         // concurrent connections / threads
    int requests = 100000;    // TOTAL requests, split across clients
    int value_size = 32;      // bytes in the SET value
    int pipeline = 1;         // requests in flight before reading replies
    std::string test = "set"; // set | get | mix
};

using Clock = std::chrono::steady_clock;

double now_us() {
    return std::chrono::duration<double, std::micro>(
               Clock::now().time_since_epoch())
        .count();
}

// ---- tiny RESP client ----------------------------------------------------

int connect_to(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Latency benchmarks are meaningless with Nagle batching small writes.
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool write_all(int fd, const char* p, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) return false;
        done += (size_t)w;
    }
    return true;
}

std::string resp_cmd(const std::vector<std::string>& args) {
    std::string s = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        s += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return s;
}

// A minimal reply reader: consumes exactly one RESP reply from a buffered
// connection. Only the reply shapes our commands produce are handled
// (+simple, -error, :int, $bulk / $-1). Returns false on a closed socket.
struct Reader {
    int fd;
    std::string buf;
    size_t pos = 0;

    bool fill() {
        char tmp[16384];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        return true;
    }
    // Read up to and including the next CRLF, returning the line (without it).
    bool read_line(std::string& out) {
        while (true) {
            size_t nl = buf.find("\r\n", pos);
            if (nl != std::string::npos) {
                out = buf.substr(pos, nl - pos);
                pos = nl + 2;
                return true;
            }
            if (!fill()) return false;
        }
    }
    bool ensure(size_t n) {
        while (buf.size() - pos < n)
            if (!fill()) return false;
        return true;
    }
    // Consume one whole reply. Compacts the buffer afterwards so it can't grow
    // without bound across a pipeline run.
    bool read_reply() {
        std::string line;
        if (!read_line(line)) return false;
        if (!line.empty() && line[0] == '$') {
            long long len = atoll(line.c_str() + 1);
            if (len >= 0) {
                if (!ensure((size_t)len + 2)) return false;  // data + CRLF
                pos += (size_t)len + 2;
            }
        }
        // +, -, : are single lines — already consumed.
        if (pos > (1 << 20)) {  // keep the backing buffer small
            buf.erase(0, pos);
            pos = 0;
        }
        return true;
    }
};

// ---- worker --------------------------------------------------------------

struct Result {
    std::vector<double> latencies_us;  // one sample per pipeline batch/op
    long long ops = 0;
    bool ok = true;
};

void run_worker(const Options& opt, int my_requests, int worker_id, Result* out) {
    int fd = connect_to(opt.host, opt.port);
    if (fd < 0) {
        out->ok = false;
        return;
    }
    Reader reader{fd, {}, 0};
    std::string value(opt.value_size, 'x');
    out->latencies_us.reserve(my_requests / std::max(1, opt.pipeline) + 1);

    int done = 0;
    while (done < my_requests) {
        int batch = std::min(opt.pipeline, my_requests - done);

        // Build one batch of requests. Unique-ish keys per worker so GETs hit
        // and the keyspace is realistic rather than a single hot key.
        std::string out_bytes;
        for (int i = 0; i < batch; i++) {
            std::string key = "k:" + std::to_string(worker_id) + ":" +
                              std::to_string((done + i) & 0xffff);
            bool do_set = opt.test == "set" ||
                          (opt.test == "mix" && ((done + i) & 1));
            if (do_set)
                out_bytes += resp_cmd({"SET", key, value});
            else
                out_bytes += resp_cmd({"GET", key});
        }

        double t0 = now_us();
        if (!write_all(fd, out_bytes.data(), out_bytes.size())) {
            out->ok = false;
            break;
        }
        bool ok = true;
        for (int i = 0; i < batch; i++)
            if (!reader.read_reply()) {
                ok = false;
                break;
            }
        double t1 = now_us();
        if (!ok) {
            out->ok = false;
            break;
        }

        // One latency sample per batch (the round-trip the caller actually
        // waits on); divide by batch when reporting per-op figures.
        out->latencies_us.push_back((t1 - t0) / batch);
        out->ops += batch;
        done += batch;
    }
    close(fd);
}

// ---- reporting -----------------------------------------------------------

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = (size_t)(p / 100.0 * (v.size() - 1));
    return v[idx];
}

void parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") o.host = v;
        else if (k == "--port") o.port = atoi(v.c_str());
        else if (k == "--clients") o.clients = atoi(v.c_str());
        else if (k == "--requests") o.requests = atoi(v.c_str());
        else if (k == "--value-size") o.value_size = atoi(v.c_str());
        else if (k == "--pipeline") o.pipeline = atoi(v.c_str());
        else if (k == "--test") o.test = v;
        else {
            fprintf(stderr, "unknown option %s\n", k.c_str());
            exit(1);
        }
    }
    if (o.clients < 1) o.clients = 1;
    if (o.pipeline < 1) o.pipeline = 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    parse_args(argc, argv, opt);

    printf("loadgen: %s x%d requests, %d clients, pipeline %d, value %dB -> %s:%d\n",
           opt.test.c_str(), opt.requests, opt.clients, opt.pipeline,
           opt.value_size, opt.host.c_str(), opt.port);

    // For the mix/get tests, pre-populate the keyspace so GETs hit.
    if (opt.test != "set") {
        int fd = connect_to(opt.host, opt.port);
        if (fd < 0) {
            fprintf(stderr, "loadgen: cannot connect to seed keyspace\n");
            return 1;
        }
        Reader r{fd, {}, 0};
        std::string value(opt.value_size, 'x');
        for (int w = 0; w < opt.clients; w++)
            for (int i = 0; i <= 0xffff; i++) {
                std::string key = "k:" + std::to_string(w) + ":" + std::to_string(i);
                std::string cmd = resp_cmd({"SET", key, value});
                write_all(fd, cmd.data(), cmd.size());
                r.read_reply();
            }
        close(fd);
    }

    std::vector<Result> results(opt.clients);
    std::vector<std::thread> threads;
    int per = opt.requests / opt.clients;

    double start = now_us();
    for (int i = 0; i < opt.clients; i++)
        threads.emplace_back(run_worker, std::cref(opt), per, i, &results[i]);
    for (auto& t : threads) t.join();
    double elapsed_s = (now_us() - start) / 1e6;

    // Merge samples + totals.
    std::vector<double> all;
    long long total_ops = 0;
    bool ok = true;
    for (auto& r : results) {
        if (!r.ok) ok = false;
        total_ops += r.ops;
        all.insert(all.end(), r.latencies_us.begin(), r.latencies_us.end());
    }
    if (!ok) fprintf(stderr, "loadgen: WARNING some workers errored\n");
    std::sort(all.begin(), all.end());

    double throughput = total_ops / elapsed_s;
    printf("\n");
    printf("  completed:     %lld ops in %.2f s\n", total_ops, elapsed_s);
    printf("  throughput:    %.0f ops/sec\n", throughput);
    printf("  latency (per-op round-trip, us):\n");
    printf("    p50  %.1f\n", percentile(all, 50));
    printf("    p99  %.1f\n", percentile(all, 99));
    printf("    max  %.1f\n", all.empty() ? 0.0 : all.back());
    return ok ? 0 : 1;
}
