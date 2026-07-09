#pragma once

#include <string>
#include <vector>

#include "config.h"

class HashTable;

// AOF — append-only file persistence (Phase 7).
//
// The idea: the store's state IS the sum of the write commands that built
// it. So we log every successful write command to a file, in RESP format
// (the same bytes a client could have sent), and on startup we replay the
// file through the same parser + executor that serves the network. Nothing
// is serialized twice: one format on the wire, in the log, and in tests.
//
// TTLs are logged as PEXPIREAT <key> <absolute-ms>, never as EX/PX:
// "expire in 100s" is meaningless hours later at replay time, while an
// absolute wall-clock deadline replays exactly (the Phase 5 clock decision
// exists for this). Expired-at-load keys simply carry a past deadline and
// die via the normal lazy path — no special replay logic.
//
// Durability knob (--appendfsync): write() puts bytes in the kernel's page
// cache (survives a process crash, NOT a power cut); fsync() forces them to
// disk. always = fsync per command (lose nothing, slowest); everysec =
// fsync from the event-loop tick (lose <=1s, Redis's default); no = kernel
// flushes when it likes (lose ~30s, fastest).
class Aof {
public:
    Aof() = default;
    ~Aof();

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // Opens (creating if absent) the log and replays its contents into
    // store. A truncated tail (crash mid-append) is tolerated: everything
    // before it loads, and the file is cut back to the last whole command.
    // Corruption in the MIDDLE returns false — the history is untrustworthy
    // and the server must refuse to start rather than run on partial data.
    bool open_and_load(const std::string& path, FsyncPolicy policy,
                       HashTable& store);

    // Appends one already-executed write command. Serialization + write();
    // fsync too if policy is always.
    void append(const std::vector<std::string>& args);

    // Called from the event-loop tick: performs the everysec fsync.
    void tick(long long now);

private:
    int fd_ = -1;
    FsyncPolicy policy_ = FsyncPolicy::EverySec;
    bool dirty_ = false;        // unsynced bytes exist
    long long last_fsync_ = 0;  // ms, for everysec pacing
};
