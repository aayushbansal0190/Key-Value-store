#include "event_loop.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <csignal>

#include "aof.h"
#include "clock.h"
#include "commands.h"
#include "resp.h"

// A client that stops reading its socket makes send() back up; its unsent
// replies pile in outbuf, in OUR memory, without bound. Past this ceiling we
// give up on that one client and close it, rather than let it OOM the whole
// server. Generous on purpose: normal clients never approach it (even a single
// max-size 512MB reply fits), but a stuck one is capped. Redis does the same
// with client-output-buffer-limit.
static constexpr size_t CLIENT_OUTBUF_LIMIT = 1024ull * 1024 * 1024;  // 1 GiB

// Ceiling on concurrent clients (Redis's `maxclients` default). Past this,
// new connections are refused with an error so a connection flood can't
// exhaust the process's file descriptors.
static constexpr int MAX_CLIENTS = 10000;

// Set by the signal handler, polled by run(). `volatile sig_atomic_t` is the
// one type the C++ standard allows a handler to touch safely — a plain bool
// could be torn or optimized into a register the loop never re-reads.
static volatile sig_atomic_t g_shutdown = 0;

static void on_shutdown_signal(int) { g_shutdown = 1; }

void install_shutdown_handlers() {
    signal(SIGINT, on_shutdown_signal);   // Ctrl-C
    signal(SIGTERM, on_shutdown_signal);  // `kill`, container stop
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

EventLoop::EventLoop(int listen_fd, HashTable& store, Aof* aof)
    : listen_fd_(listen_fd), store_(store), aof_(aof) {
    stats_.start_ms = now_ms();
    stats_.aof_enabled = (aof_ != nullptr);
    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        perror("kvstore: epoll_create1");
        _exit(1);
    }
    // Level-triggered (the default; no EPOLLET). LT re-notifies while data
    // remains, so not draining a socket completely is never fatal. The
    // edge-triggered alternative notifies once per arrival and REQUIRES
    // drain-until-EAGAIN — faster in theory, deadlock-prone in practice.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd_;
    epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev);
}

EventLoop::~EventLoop() {
    for (size_t fd = 0; fd < conns_.size(); fd++) {
        if (conns_[fd]) close_conn((int)fd);
    }
    if (epfd_ >= 0) close(epfd_);
    if (listen_fd_ >= 0) close(listen_fd_);
}

void EventLoop::run() {
    constexpr int MAX_EVENTS = 64;
    constexpr int TICK_MS = 100;
    epoll_event events[MAX_EVENTS];
    long long last_tick = now_ms();

    while (!g_shutdown) {
        // The single place the whole server sleeps — but now at most 100ms
        // at a time: even a completely idle server wakes up to run the tick
        // (active TTL sweep + finishing any pending rehash).
        int n = epoll_wait(epfd_, events, MAX_EVENTS, TICK_MS);
        if (n < 0) {
            // A shutdown signal interrupts epoll_wait with EINTR; the loop
            // condition above then sees g_shutdown and exits cleanly.
            if (errno == EINTR) continue;
            perror("kvstore: epoll_wait");
            return;
        }

        // The tick: time-based housekeeping. Checked every wakeup (busy
        // servers return from epoll_wait constantly, so gate on elapsed time).
        long long now = now_ms();
        if (now - last_tick >= TICK_MS) {
            last_tick = now;
            store_.expire_cycle(now);
            if (aof_) aof_->tick(now);  // the everysec fsync rides the tick
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                handle_accept();
                continue;
            }

            // Look the connection up by fd EVERY time, not via a stored
            // pointer: an earlier event in this same batch may have closed
            // this connection — a pointer would dangle; a lookup sees null.
            Conn* c = (fd < (int)conns_.size()) ? conns_[fd] : nullptr;
            if (!c) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close_conn(fd);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                handle_readable(c);
                c = (fd < (int)conns_.size()) ? conns_[fd] : nullptr;  // may have closed
                if (!c) continue;
            }
            if (events[i].events & EPOLLOUT) {
                handle_writable(c);
            }
        }
    }

    // Left the loop => a shutdown signal arrived. Returning unwinds main(),
    // whose ~EventLoop closes the sockets and whose ~Aof fsyncs the log.
    printf("kvstore: shutting down (signal received)\n");
}

void EventLoop::handle_accept() {
    // Drain ALL pending connections: one EPOLLIN on the listener may stand
    // for several queued clients, and with LT we'd be re-notified anyway —
    // but accepting in a loop serves everyone this pass instead of next.
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof peer;
        int cfd = accept(listen_fd_, (sockaddr*)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // queue empty
            perror("kvstore: accept");
            return;
        }

        // Cap concurrent clients so a flood of connections can't exhaust file
        // descriptors (and the memory each Conn holds). We reply with an error
        // and drop THIS socket, but keep draining the accept queue. Redis has
        // the same guard (its `maxclients`, default 10000).
        if (stats_.connected_clients >= MAX_CLIENTS) {
            const char* full = "-ERR max number of clients reached\r\n";
            (void)!write(cfd, full, strlen(full));  // best-effort; ignore result
            close(cfd);
            continue;
        }

        // Every client socket must be non-blocking: one slow client's recv
        // or send must never be able to put the whole server to sleep.
        set_nonblocking(cfd);

        epoll_event ev{};
        ev.events = EPOLLIN;  // EPOLLOUT joins later, only while outbuf pends
        ev.data.fd = cfd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            // Registration failed: this fd would never get events, so it'd be a
            // silent leak. Drop it now rather than track a dead connection.
            perror("kvstore: epoll_ctl add");
            close(cfd);
            continue;
        }

        // Registered successfully: NOW record the connection. (Doing this only
        // after epoll_ctl succeeds keeps conns_ and connected_clients honest.)
        if (cfd >= (int)conns_.size()) conns_.resize(cfd + 1, nullptr);
        Conn* c = new Conn();
        c->fd = cfd;
        conns_[cfd] = c;
        stats_.connected_clients++;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        printf("kvstore: client connected from %s:%d (fd=%d)\n",
               ip, ntohs(peer.sin_port), cfd);
    }
}

void EventLoop::handle_readable(Conn* c) {
    char chunk[4096];

    while (true) {
        ssize_t n = recv(c->fd, chunk, sizeof chunk, 0);
        if (n > 0) {
            c->inbuf.append(chunk, (size_t)n);
            continue;  // keep draining until EAGAIN
        }
        if (n == 0) {  // peer closed
            close_conn(c->fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained for now
        close_conn(c->fd);  // real error
        return;
    }

    // Peel as many COMPLETE commands as the buffer holds. Two protocols
    // coexist, dispatched by first byte exactly like Redis: '*' opens a
    // RESP array; anything else is an inline (telnet-style) line.
    //
    // We walk with a cursor `pos` and drop the consumed prefix ONCE at the end,
    // instead of erase(0, n) after every command. A client that pipelines N
    // commands in one packet used to cost O(N^2) (each erase shifts the whole
    // remaining buffer); now it's O(N) for the pass plus one erase.
    size_t pos = 0;
    while (pos < c->inbuf.size() && !c->close_after_reply) {
        if (c->inbuf[pos] == '*') {
            size_t consumed = 0;
            std::vector<std::string> args;
            std::string perr;
            // `consumed` comes back as the ABSOLUTE end index of this command.
            ParseResult r = parse_resp_command(c->inbuf, pos, consumed, args, perr);
            if (r == ParseResult::Incomplete) break;  // rest still in flight
            if (r == ParseResult::Error) {
                c->outbuf += resp_error("ERR Protocol error: " + perr);
                c->close_after_reply = true;
                break;
            }
            pos = consumed;
            if (!args.empty())
                c->outbuf += execute_command(store_, args, aof_, &stats_);
        } else {
            size_t nl = c->inbuf.find('\n', pos);
            if (nl == std::string::npos) {
                // No terminator yet — but cap how long we'll wait for one:
                // an endless unterminated line is memory abuse, not a command.
                if (c->inbuf.size() - pos > RESP_MAX_INLINE) {
                    c->outbuf += resp_error("ERR Protocol error: too big inline request");
                    c->close_after_reply = true;
                }
                break;
            }
            std::string line = c->inbuf.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::vector<std::string> args = split_inline(line);
            if (args.empty()) continue;  // blank line: ignore, like Redis
            c->outbuf += execute_command(store_, args, aof_, &stats_);
        }
    }
    if (pos > 0) c->inbuf.erase(0, pos);  // drop all consumed commands at once

    if (!try_flush(c)) {
        close_conn(c->fd);
        return;
    }
    if (c->close_after_reply && c->pending_output() == 0) close_conn(c->fd);
}

void EventLoop::handle_writable(Conn* c) {
    // EPOLLOUT fired: the kernel made room in its send buffer — push more.
    if (!try_flush(c)) {
        close_conn(c->fd);
        return;
    }
    // A protocol-error reply may have finished draining only now.
    if (c->close_after_reply && c->pending_output() == 0) close_conn(c->fd);
}

bool EventLoop::try_flush(Conn* c) {
    // Send from the cursor forward. out_pos advances instead of erasing the
    // front each time (see Conn::out_pos).
    while (c->out_pos < c->outbuf.size()) {
        ssize_t n = send(c->fd, c->outbuf.data() + c->out_pos,
                         c->outbuf.size() - c->out_pos, 0);
        if (n > 0) {
            c->out_pos += (size_t)n;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // kernel full; wait for EPOLLOUT
        return false;  // peer vanished mid-reply etc.
    }

    // Reclaim the already-sent prefix. If everything drained, just reset the
    // cursor (no copy). Otherwise physically drop the sent bytes so outbuf
    // can't grow forever behind a stuck cursor — but only past a threshold,
    // so a busy fast client isn't erasing tiny prefixes constantly.
    if (c->out_pos == c->outbuf.size()) {
        c->outbuf.clear();
        c->out_pos = 0;
    } else if (c->out_pos > 64 * 1024) {
        c->outbuf.erase(0, c->out_pos);
        c->out_pos = 0;
    }

    // Backpressure guard: a client that won't read makes pending replies pile
    // up unbounded. Past the ceiling, drop it rather than risk the whole server.
    if (c->pending_output() > CLIENT_OUTBUF_LIMIT) {
        fprintf(stderr, "kvstore: client fd=%d exceeded output buffer limit; closing\n",
                c->fd);
        return false;
    }

    update_interest(c);
    return true;
}

void EventLoop::update_interest(Conn* c) {
    // EPOLLOUT only while there's something to write. Sockets are writable
    // almost always — subscribing permanently would wake us in a busy loop
    // doing nothing. This toggle is the standard pattern.
    epoll_event ev{};
    ev.events = EPOLLIN;
    if (c->pending_output() > 0) ev.events |= EPOLLOUT;
    ev.data.fd = c->fd;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, c->fd, &ev);
}

void EventLoop::close_conn(int fd) {
    Conn* c = (fd < (int)conns_.size()) ? conns_[fd] : nullptr;
    if (!c) return;
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns_[fd] = nullptr;
    delete c;
    if (stats_.connected_clients > 0) stats_.connected_clients--;
    printf("kvstore: client disconnected (fd=%d)\n", fd);
}
