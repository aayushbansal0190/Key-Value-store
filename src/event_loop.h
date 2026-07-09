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
        // Set on protocol errors: send what's queued (the error message),
        // then close. With framing broken we can't locate the next command,
        // so continuing would risk executing garbage — same policy as Redis.
        bool close_after_reply = false;
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
