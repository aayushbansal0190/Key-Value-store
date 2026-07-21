#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <string>

#include "aof.h"
#include "config.h"
#include "event_loop.h"
#include "hashtable.h"

static void print_usage(const char* prog) {
    fprintf(stderr,
            "usage: %s [--port N] [--maxmemory BYTES|100mb|1gb] "
            "[--appendonly yes|no] [--appendfsync always|everysec|no] [--dir PATH]\n",
            prog);
}

// socket() -> bind() -> listen(), non-blocking, ready for the event loop.
// Returns -1 on failure (already perror'd).
static int make_listen_socket(int port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("kvstore: socket");
        return -1;
    }

    // Allow rebinding while a previous socket is still in TIME_WAIT (restart).
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // trusted-network assumption
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (sockaddr*)&addr, sizeof addr) < 0) {
        perror("kvstore: bind");
        close(lfd);
        return -1;
    }
    if (listen(lfd, 511) < 0) {  // Redis's default backlog
        perror("kvstore: listen");
        close(lfd);
        return -1;
    }

    // The listener itself must be non-blocking too: epoll may report it
    // ready and a client may vanish before our accept() — blocking there
    // would hang the whole loop on a ghost.
    int flags = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
    return lfd;
}

int main(int argc, char** argv) {
    Config cfg;
    std::string err;
    if (!parse_config(argc, argv, cfg, err)) {
        fprintf(stderr, "kvstore: %s\n", err.c_str());
        print_usage(argv[0]);
        return 1;
    }

    // A peer-closed socket write must be an error code, not a process death.
    signal(SIGPIPE, SIG_IGN);

    // Ctrl-C / `kill` now stops the loop cleanly (run() returns) so the AOF is
    // flushed by main()'s destructors, instead of the process being killed
    // outright with unsynced writes still in the kernel's page cache.
    install_shutdown_handlers();

    int lfd = make_listen_socket(cfg.port);
    if (lfd < 0) return 1;

    printf("kvstore: listening on port %d (epoll event loop)\n", cfg.port);
    if (cfg.maxmemory > 0) {
        printf("kvstore: maxmemory %llu bytes (allkeys-lru eviction)\n",
               (unsigned long long)cfg.maxmemory);
    }

    HashTable store;
    store.set_maxmemory((size_t)cfg.maxmemory);

    // Persistence: replay history BEFORE serving traffic, so clients only
    // ever see the fully restored state.
    Aof aof;
    if (cfg.appendonly) {
        std::string path = cfg.dir + "/appendonly.aof";
        if (!aof.open_and_load(path, cfg.appendfsync, store)) return 1;
        printf("kvstore: AOF enabled (%s, fsync %s)\n", path.c_str(),
               fsync_policy_name(cfg.appendfsync));
    }

    EventLoop loop(lfd, store, cfg.appendonly ? &aof : nullptr);
    loop.run();
    return 0;
}
