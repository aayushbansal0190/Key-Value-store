#pragma once

#include <string>
#include <vector>

#include "hashtable.h"

class Aof;

// Server-wide runtime stats for INFO. Owned by the event loop (which knows
// about connections); nullptr in unit tests — INFO then simply omits the
// server/clients sections and reports store-level stats only.
struct ServerStats {
    long long start_ms = 0;          // server start time (uptime_in_seconds)
    size_t connected_clients = 0;    // live connections right now
    long long total_commands = 0;    // every command executed, reads included
    bool aof_enabled = false;
};

// Splits an inline command line ("SET name aayush") into whitespace-separated
// tokens. No quoting support: inline mode is a telnet/debug convenience;
// binary-safe arguments arrive with RESP in Phase 4.
std::vector<std::string> split_inline(const std::string& line);

// Executes one parsed command against the store and returns the reply text
// (no trailing newline; the transport adds its own framing).
// args must be non-empty. Deliberately knows nothing about sockets so every
// front-end (blocking loop now, epoll in Phase 3, RESP in Phase 4) can share
// it — and so tests can drive commands without a network.
//
// aof: successful WRITE commands are appended there (nullptr = no logging —
// used by tests and by AOF replay itself, which must not re-log history).
// Logging lives HERE, not in the storage layer, because only the command
// layer knows the normalized form to persist (e.g. SET..EX becomes SET +
// PEXPIREAT with an absolute deadline).
// stats: incremented/read by INFO when provided (the event loop passes its
// own; tests may pass nullptr).
std::string execute_command(HashTable& store, const std::vector<std::string>& args,
                            Aof* aof = nullptr, ServerStats* stats = nullptr);
