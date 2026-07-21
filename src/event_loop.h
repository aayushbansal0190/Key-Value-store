#pragma once

#include <string>
#include <vector>

#include "commands.h"  // ServerStats
#include "hashtable.h"

class Aof;

// The heart of the server (Linux-only: epoll): one thread multiplexing every
// client. All fds are non-blocking; epoll_wait() is the ONLY place the
// process ever sleeps. Commands still execute one at a time, in the order
// events are handled — that serialization is what makes the store safe with
// zero locks.
class EventLoop {
public:
    // Takes ownership of listen_fd (must already be listening + non-blocking).
    // aof may be null (persistence disabled).
    EventLoop(int listen_fd, HashTable& store, Aof* aof = nullptr);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Runs forever (process exit is how the server stops in this phase).
    void run();

private:
    // Per-connection state. Two buffers absorb partial reads and writes:
    //  inbuf  - bytes received but not yet forming a complete command
    //  outbuf - reply bytes the kernel's send buffer couldn't take yet
    struct Conn {
        int fd = -1;
        std::string inbuf;
        std::string outbuf;
        // How many bytes at the FRONT of outbuf are already sent. We advance
        // this instead of erase(0, n)-ing after every send (erase shifts the
        // whole remaining string left — O(remaining) each call, quadratic for
        // a slow client drained in many small chunks). The already-sent prefix
        // is physically dropped only occasionally (see try_flush).
        size_t out_pos = 0;
        // Set on protocol errors: send what's queued (the error message),
        // then close. With framing broken we can't locate the next command,
        // so continuing would risk executing garbage — same policy as Redis.
        bool close_after_reply = false;

        // Unsent reply bytes still queued (outbuf past the already-sent prefix).
        size_t pending_output() const { return outbuf.size() - out_pos; }
    };

    int epfd_ = -1;
    int listen_fd_ = -1;
    HashTable& store_;
    Aof* aof_ = nullptr;  // null when --appendonly no
    ServerStats stats_;   // uptime, live client count, command counter — for INFO
    // fd -> connection, indexed directly: fds are small dense integers
    // (same trick as Redis). nullptr = no connection on that fd.
    std::vector<Conn*> conns_;

    void handle_accept();
    void handle_readable(Conn* c);
    void handle_writable(Conn* c);

    // Try to push outbuf into the kernel now; whatever doesn't fit stays,
    // and EPOLLOUT interest is toggled accordingly.
    bool try_flush(Conn* c);          // false = fatal error, caller must close
    void update_interest(Conn* c);    // EPOLLIN always; EPOLLOUT iff outbuf pending
    void close_conn(int fd);
};

// Installs SIGINT/SIGTERM handlers so a running EventLoop stops cleanly at the
// top of its next iteration (run() returns), letting main()'s destructors flush
// and fsync the AOF. Without this the default signal action kills the process
// outright — no destructors, so an everysec AOF can lose its last unsynced
// second even on a polite Ctrl-C / `kill`. Call once before loop.run().
void install_shutdown_handlers();
