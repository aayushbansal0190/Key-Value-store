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

#include "aof.h"
#include "clock.h"
#include "commands.h"
#include "resp.h"

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

    while (true) {
        // The single place the whole server sleeps — but now at most 100ms
        // at a time: even a completely idle server wakes up to run the tick
        // (active TTL sweep + finishing any pending rehash).
        int n = epoll_wait(epfd_, events, MAX_EVENTS, TICK_MS);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal: harmless
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

        // Every client socket must be non-blocking: one slow client's recv
        // or send must never be able to put the whole server to sleep.
        set_nonblocking(cfd);

        if (cfd >= (int)conns_.size()) conns_.resize(cfd + 1, nullptr);
        Conn* c = new Conn();
        c->fd = cfd;
        conns_[cfd] = c;
        stats_.connected_clients++;

        epoll_event ev{};
        ev.events = EPOLLIN;  // EPOLLOUT joins later, only while outbuf pends
        ev.data.fd = cfd;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, cfd, &ev);

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
    while (!c->inbuf.empty() && !c->close_after_reply) {
        if (c->inbuf[0] == '*') {
            size_t consumed = 0;
            std::vector<std::string> args;
            std::string perr;
            ParseResult r = parse_resp_command(c->inbuf, consumed, args, perr);
            if (r == ParseResult::Incomplete) break;  // rest still in flight
            if (r == ParseResult::Error) {
                c->outbuf += resp_error("ERR Protocol error: " + perr);
                c->close_after_reply = true;
                break;
            }
            c->inbuf.erase(0, consumed);
            if (!args.empty())
                c->outbuf += execute_command(store_, args, aof_, &stats_);
        } else {
            size_t nl = c->inbuf.find('\n');
            if (nl == std::string::npos) {
                // No terminator yet — but cap how long we'll wait for one:
                // an endless unterminated line is memory abuse, not a command.
                if (c->inbuf.size() > RESP_MAX_INLINE) {
                    c->outbuf += resp_error("ERR Protocol error: too big inline request");
                    c->close_after_reply = true;
                }
                break;
            }
            std::string line = c->inbuf.substr(0, nl);
            c->inbuf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::vector<std::string> args = split_inline(line);
            if (args.empty()) continue;  // blank line: ignore, like Redis
            c->outbuf += execute_command(store_, args, aof_, &stats_);
        }
    }

    if (!try_flush(c)) {
        close_conn(c->fd);
        return;
    }
    if (c->close_after_reply && c->outbuf.empty()) close_conn(c->fd);
}

void EventLoop::handle_writable(Conn* c) {
    // EPOLLOUT fired: the kernel made room in its send buffer — push more.
    if (!try_flush(c)) {
        close_conn(c->fd);
        return;
    }
    // A protocol-error reply may have finished draining only now.
    if (c->close_after_reply && c->outbuf.empty()) close_conn(c->fd);
}

bool EventLoop::try_flush(Conn* c) {
    while (!c->outbuf.empty()) {
        ssize_t n = send(c->fd, c->outbuf.data(), c->outbuf.size(), 0);
        if (n > 0) {
            c->outbuf.erase(0, (size_t)n);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // kernel full; wait for EPOLLOUT
        return false;  // peer vanished mid-reply etc.
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
    if (!c->outbuf.empty()) ev.events |= EPOLLOUT;
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
