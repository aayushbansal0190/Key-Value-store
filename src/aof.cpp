#include "aof.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "clock.h"
#include "commands.h"
#include "hashtable.h"
#include "resp.h"

Aof::~Aof() {
    if (fd_ >= 0) {
        fsync(fd_);  // best-effort: don't lose the last everysec window
        close(fd_);
    }
}

// write() may accept fewer bytes than asked (signals, disk pressure) —
// same lesson as send_all() on sockets: "write a buffer" is always a loop.
static bool write_all(int fd, const char* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

bool Aof::open_and_load(const std::string& path, FsyncPolicy policy,
                        HashTable& store) {
    policy_ = policy;
    last_fsync_ = now_ms();

    // O_APPEND: every write() goes atomically to the current end of file,
    // even after the load below truncates a torn tail.
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (fd_ < 0) {
        fprintf(stderr, "kvstore: cannot open AOF %s: %s\n", path.c_str(),
                strerror(errno));
        return false;
    }

    // Read the whole log. Fine at our scale; a huge AOF would want streaming
    // (and, before that, rewrite/compaction — the known stretch goal).
    std::string log;
    char chunk[65536];
    ssize_t n;
    while ((n = read(fd_, chunk, sizeof chunk)) > 0) log.append(chunk, (size_t)n);
    if (n < 0) {
        fprintf(stderr, "kvstore: cannot read AOF: %s\n", strerror(errno));
        return false;
    }

    // Replay: parse commands off the front exactly like the event loop does
    // with a socket buffer, and run each through the SAME executor clients
    // use — replies discarded, aof=nullptr so replay never re-logs itself.
    size_t good_bytes = 0;  // how much of the file is whole commands
    long long replayed = 0;
    while (!log.empty()) {
        size_t consumed = 0;
        std::vector<std::string> args;
        std::string perr;
        // The parser reads the FRONT of its buffer, so peel consumed bytes
        // off as we go — the same idiom as the event loop's inbuf. (The
        // erase is O(remaining) per command — same known trade as inbuf.)
        ParseResult r = parse_resp_command(log, consumed, args, perr);

        if (r == ParseResult::Complete) {
            log.erase(0, consumed);
            good_bytes += consumed;
            if (!args.empty()) execute_command(store, args, nullptr);
            replayed++;
            continue;
        }
        if (r == ParseResult::Incomplete) {
            // Torn tail: the process died mid-append. Everything before this
            // point is intact history — keep it, CUT the partial bytes off
            // (appending after them would corrupt the file for the NEXT
            // load), and carry on. Redis's aof-load-truncated does the same.
            fprintf(stderr,
                    "kvstore: AOF has a truncated final command (%zu bytes) — "
                    "dropping it and continuing\n",
                    log.size());
            if (ftruncate(fd_, (off_t)good_bytes) < 0) {
                fprintf(stderr, "kvstore: cannot truncate AOF: %s\n",
                        strerror(errno));
                return false;
            }
            break;
        }
        // Error: garbage in the MIDDLE of the log. Unlike a torn tail there
        // is no way to know what was lost — running with silently partial
        // data is worse than refusing to start.
        fprintf(stderr, "kvstore: AOF corrupted (%s) — refusing to start\n",
                perr.c_str());
        return false;
    }

    if (replayed > 0) {
        printf("kvstore: AOF replayed %lld commands, %zu keys live\n",
               replayed, store.size());
    }
    return true;
}

void Aof::append(const std::vector<std::string>& args) {
    if (fd_ < 0) return;
    std::string data = resp_command(args);
    if (!write_all(fd_, data.data(), data.size())) {
        // Disk full / IO error: the command already executed in memory, so
        // report loudly. (Redis goes further: it starts REJECTING writes
        // until the AOF is writable again — noted simplification.)
        fprintf(stderr, "kvstore: AOF write failed: %s\n", strerror(errno));
        return;
    }
    dirty_ = true;
    if (policy_ == FsyncPolicy::Always) {
        fsync(fd_);
        dirty_ = false;
    }
    // everysec: fsync happens in tick(). no: never fsync, kernel decides.
}

void Aof::tick(long long now) {
    if (fd_ < 0 || !dirty_) return;
    if (policy_ == FsyncPolicy::EverySec && now - last_fsync_ >= 1000) {
        fsync(fd_);
        dirty_ = false;
        last_fsync_ = now;
    }
}
