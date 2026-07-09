#include "commands.h"

#include <cctype>
#include <climits>

#include "aof.h"
#include "clock.h"
#include "resp.h"

std::vector<std::string> split_inline(const std::string& line) {
    std::vector<std::string> args;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
        if (i > start) args.push_back(line.substr(start, i - start));
    }
    return args;
}

static std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)toupper((unsigned char)c);
    return out;
}

static std::string err_arity(const char* cmd) {
    return resp_error(std::string("ERR wrong number of arguments for '") + cmd +
                      "' command");
}

// Strict signed integer parse: whole string must be digits (one leading '-'
// allowed), overflow-checked. "10x" and "" are rejections, not 10 and 0.
static bool parse_int_arg(const std::string& s, long long& out) {
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') {
        neg = true;
        if (++i >= s.size()) return false;
    }
    long long v = 0;
    for (; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i])) return false;
        int d = s[i] - '0';
        if (v > (LLONG_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    out = neg ? -v : v;
    return true;
}

// Glob match for KEYS: '*' = any run of characters (including none),
// '?' = exactly one character. Classic two-pointer with backtracking:
// remember the position of the last '*', and on any dead end let that star
// swallow one more character and retry. Worst case O(len(pat)*len(str)) —
// acceptable for a documented debug-only command. (Redis's matcher also
// supports [a-z] classes and backslash escapes; deliberately omitted.)
static bool glob_match(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0;
    size_t star = std::string::npos;  // index of the last '*' seen
    size_t star_s = 0;                // where the string was when we saw it
    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            p++, s++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;    // try "star matches nothing" first
            star_s = s;
        } else if (star != std::string::npos) {
            p = star + 1;  // backtrack: the star eats one more character
            s = ++star_s;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;  // trailing stars match ""
    return p == pat.size();
}

// SET key value [EX seconds | PX milliseconds]
static std::string cmd_set(HashTable& store, const std::vector<std::string>& args,
                           Aof* aof) {
    if (args.size() < 3) return err_arity("set");

    long long ttl_ms = -1;
    for (size_t i = 3; i < args.size(); i++) {
        std::string opt = to_upper(args[i]);
        if (opt == "EX" || opt == "PX") {
            // One expiry option only, and it needs its number.
            if (ttl_ms != -1 || i + 1 >= args.size()) {
                return resp_error("ERR syntax error");
            }
            long long v;
            if (!parse_int_arg(args[i + 1], v)) {
                return resp_error("ERR value is not an integer or out of range");
            }
            // Zero/negative lifetimes are rejected here (Redis does too);
            // contrast with EXPIRE, where <=0 means "delete now".
            if (v <= 0 || (opt == "EX" && v > LLONG_MAX / 1000)) {
                return resp_error("ERR invalid expire time in 'set' command");
            }
            ttl_ms = (opt == "EX") ? v * 1000 : v;
            i++;  // consume the number
        } else {
            return resp_error("ERR syntax error");
        }
    }

    // Oversized-write guard: if this one entry can't fit the budget even in
    // an empty store, storing it would evict EVERY other key and still leave
    // us over the limit (our evict loop refuses to drop the just-written key).
    // Reject up front instead — cheaper and non-destructive. Redis does the
    // same, returning an OOM error. Only meaningful when maxmemory is set.
    if (store.maxmemory() > 0 &&
        HashTable::entry_size(args[1], args[2]) > store.maxmemory()) {
        return resp_error(
            "OOM command not allowed when used memory > 'maxmemory'");
    }

    store.set(args[1], args[2]);  // clears any previous TTL (Redis semantics)
    long long deadline = -1;
    if (ttl_ms > 0) {
        deadline = now_ms() + ttl_ms;
        // Second lookup — costs one extra hash. Fine: only SET-with-TTL pays,
        // and it keeps HashTable::set's signature simple.
        store.find(args[1])->expires_at = deadline;
    }
    if (aof) {
        // Logged in NORMALIZED form: the plain SET, then the TTL as an
        // ABSOLUTE deadline. Replaying "EX 100" hours later would grant a
        // fresh 100 seconds; PEXPIREAT replays to the exact same instant
        // (and a now-past deadline dies via ordinary lazy expiry). Redis
        // performs this same EX -> PEXPIREAT translation in its AOF.
        aof->append({"SET", args[1], args[2]});
        if (deadline >= 0) {
            aof->append({"PEXPIREAT", args[1], std::to_string(deadline)});
        }
    }
    return resp_simple("OK");
}

// Returns a COMPLETE RESP-serialized reply (typed + CRLF-terminated): the
// command layer owns reply formatting; transports just move the bytes.
std::string execute_command(HashTable& store, const std::vector<std::string>& args,
                            Aof* aof, ServerStats* stats) {
    if (stats) stats->total_commands++;
    // Command names are case-insensitive (like Redis); keys and values are not.
    std::string cmd = to_upper(args[0]);

    if (cmd == "PING") {
        // Like Redis: bare PING -> +PONG; PING msg echoes msg back.
        if (args.size() == 1) return resp_simple("PONG");
        if (args.size() == 2) return resp_bulk(args[1]);
        return err_arity("ping");
    }
    if (cmd == "ECHO") {
        return args.size() == 2 ? resp_bulk(args[1]) : err_arity("echo");
    }
    if (cmd == "SET") {
        return cmd_set(store, args, aof);
    }
    if (cmd == "GET") {
        if (args.size() != 2) return err_arity("get");
        const std::string* v = store.get(args[1]);
        // Typed nil: a MISS ($-1) is distinct from any stored value.
        return v ? resp_bulk(*v) : resp_nil();
    }
    if (cmd == "DEL") {
        // Variadic like Redis: DEL k1 k2 k3 -> how many actually existed.
        if (args.size() < 2) return err_arity("del");
        long long deleted = 0;
        for (size_t i = 1; i < args.size(); i++) {
            if (store.erase(args[i])) deleted++;
        }
        // Log only if something actually died. The original args are fine
        // as-is: replaying a DEL of a key that won't exist is a no-op.
        if (aof && deleted > 0) aof->append(args);
        return resp_integer(deleted);
    }
    if (cmd == "EXISTS") {
        // Variadic like Redis: EXISTS a b a counts each occurrence, so a
        // present key named twice contributes 2. find() applies lazy expiry,
        // so a logically-dead key correctly counts as absent.
        if (args.size() < 2) return err_arity("exists");
        long long count = 0;
        for (size_t i = 1; i < args.size(); i++) {
            if (store.find(args[i])) count++;
        }
        return resp_integer(count);
    }
    if (cmd == "KEYS") {
        // Documented O(n) DEBUG-ONLY command: walks the WHOLE keyspace and
        // glob-filters. Never use on a large production store (it also blocks
        // the single thread for the whole scan) — SCAN is the cursor-based
        // alternative Redis offers, out of scope here.
        if (args.size() != 2) return err_arity("keys");
        std::vector<std::string> out;
        for (const std::string& k : store.keys()) {
            if (glob_match(args[1], k)) out.push_back(k);
        }
        return resp_array(out);
    }
    if (cmd == "FLUSHALL") {
        // Wipe everything. A write, so it's logged — replaying FLUSHALL at
        // the right point rebuilds the same "cleared here" history.
        if (args.size() != 1) return err_arity("flushall");
        store.clear();
        if (aof) aof->append({"FLUSHALL"});
        return resp_simple("OK");
    }
    if (cmd == "EXPIRE") {
        if (args.size() != 3) return err_arity("expire");
        long long seconds;
        if (!parse_int_arg(args[2], seconds)) {
            return resp_error("ERR value is not an integer or out of range");
        }
        HashTable::Entry* e = store.find(args[1]);
        if (!e) return resp_integer(0);  // no such (live) key
        if (seconds <= 0) {
            // Already in the past: Redis deletes immediately and reports
            // success — the expiry "happened", just instantly.
            store.erase(args[1]);
            if (aof) aof->append({"DEL", args[1]});  // log what HAPPENED
            return resp_integer(1);
        }
        if (seconds > LLONG_MAX / 1000) {
            return resp_error("ERR invalid expire time in 'expire' command");
        }
        e->expires_at = now_ms() + seconds * 1000;
        // Relative "in 50s" is logged as the absolute instant it resolved to.
        if (aof) aof->append({"PEXPIREAT", args[1], std::to_string(e->expires_at)});
        return resp_integer(1);
    }
    if (cmd == "PEXPIREAT") {
        // PEXPIREAT key <unix-ms>: expiry as an ABSOLUTE wall-clock instant.
        // A real Redis command — and the form every TTL takes in our AOF,
        // because absolute deadlines replay exactly (relative ones can't).
        if (args.size() != 3) return err_arity("pexpireat");
        long long at_ms;
        if (!parse_int_arg(args[2], at_ms)) {
            return resp_error("ERR value is not an integer or out of range");
        }
        HashTable::Entry* e = store.find(args[1]);
        if (!e) return resp_integer(0);
        // A past deadline just makes the key instantly dead; lazy expiry
        // reaps it on next touch. (Redis deletes right here — observably
        // identical, since a dead key can never be read.)
        e->expires_at = at_ms;
        if (aof) aof->append({"PEXPIREAT", args[1], std::to_string(at_ms)});
        return resp_integer(1);
    }
    if (cmd == "TTL") {
        if (args.size() != 2) return err_arity("ttl");
        HashTable::Entry* e = store.find(args[1]);
        if (!e) return resp_integer(-2);                 // key doesn't exist
        if (e->expires_at < 0) return resp_integer(-1);  // exists, no expiry
        // Round to nearest second, like Redis (999ms left reads as 1, not 0).
        return resp_integer((e->expires_at - now_ms() + 500) / 1000);
    }
    if (cmd == "PERSIST") {
        if (args.size() != 2) return err_arity("persist");
        HashTable::Entry* e = store.find(args[1]);
        if (!e || e->expires_at < 0) return resp_integer(0);  // nothing to remove
        e->expires_at = -1;
        if (aof) aof->append({"PERSIST", args[1]});
        return resp_integer(1);
    }
    if (cmd == "INFO") {
        // One bulk string of "field:value" lines grouped in #-sections — the
        // same shape as Redis, so redis-cli renders it and monitoring habits
        // transfer. Store-level numbers are always present; the Server/Clients
        // sections need runtime state only the event loop has (stats), so they
        // appear only when it's wired in (omitted in unit tests).
        if (args.size() != 1) return err_arity("info");
        std::string out;

        out += "# Server\r\n";
        out += "kvstore_version:1.0.0\r\n";
        if (stats) {
            long long uptime = (now_ms() - stats->start_ms) / 1000;
            out += "uptime_in_seconds:" + std::to_string(uptime) + "\r\n";
        }
        out += "\r\n# Clients\r\n";
        if (stats) {
            out += "connected_clients:" +
                   std::to_string(stats->connected_clients) + "\r\n";
        }

        out += "\r\n# Memory\r\n";
        out += "used_memory:" + std::to_string(store.used_memory()) + "\r\n";
        out += "maxmemory:" + std::to_string(store.maxmemory()) + "\r\n";
        out += "maxmemory_policy:" +
               std::string(store.maxmemory() > 0 ? "allkeys-lru" : "noeviction") +
               "\r\n";

        out += "\r\n# Persistence\r\n";
        out += "aof_enabled:" +
               std::string((stats && stats->aof_enabled) ? "1" : "0") + "\r\n";

        out += "\r\n# Stats\r\n";
        if (stats) {
            out += "total_commands_processed:" +
                   std::to_string(stats->total_commands) + "\r\n";
        }
        out += "evicted_keys:" + std::to_string(store.evicted_keys()) + "\r\n";
        out += "expired_keys:" + std::to_string(store.expired_keys()) + "\r\n";

        out += "\r\n# Keyspace\r\n";
        if (store.size() > 0) {  // Redis omits empty databases too
            out += "db0:keys=" + std::to_string(store.size()) + "\r\n";
        }
        return resp_bulk(out);
    }
    if (cmd == "DBSIZE") {
        if (args.size() != 1) return err_arity("dbsize");
        // Counts expired-but-unreaped keys too, exactly like Redis: lazy and
        // active expiry shrink it, nothing else does.
        return resp_integer((long long)store.size());
    }
    return resp_error("ERR unknown command '" + args[0] + "'");
}
