#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The main store: a chaining hash table with incremental (gradual) rehash,
// modeled on Redis's dict, plus TTL support (lazy + sampled active expiry).
//
// Chaining over open addressing, deliberately: chain nodes are individually
// heap-allocated so their addresses are STABLE — Phase 6 threads an intrusive
// LRU list through these exact nodes, which open addressing (entries relocate
// on resize/probing) would break. Deletion is also a simple unlink; no
// tombstones. The cost we accept: pointer-chasing is less cache-friendly.
class HashTable {
public:
    struct Entry {
        std::string key;
        std::string value;
        Entry* next;              // collision chain within one bucket
        long long expires_at = -1;  // wall-clock ms; -1 = never expires

        // Intrusive LRU list: the recency list is threaded THROUGH the
        // entries themselves (no separate list nodes, no key duplication —
        // possible only because chaining gives every entry a stable address).
        // head = most recently used, tail = least recently used (= eviction
        // victim). This is true LRU; Redis instead samples a few keys and
        // evicts the oldest of the sample (approximate, but with zero list
        // maintenance on the hot path).
        Entry* lru_prev = nullptr;  // toward the head (more recent)
        Entry* lru_next = nullptr;  // toward the tail (less recent)
    };

    HashTable();
    ~HashTable();

    // Entries are raw heap pointers; copying would double-free. The store is
    // a singleton-ish object owned by the server, so copying is never needed.
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    // Insert or overwrite. Returns true if the key is new, false on overwrite.
    // Overwriting CLEARS any TTL (Redis's SET semantics): callers wanting a
    // TTL set entry->expires_at via find() afterwards.
    bool set(const std::string& key, const std::string& value);

    // Returns the value, or nullptr if absent. The pointer stays valid only
    // until the next set/erase (rehashing moves nothing — nodes are stable —
    // but an overwrite/erase of this key rewrites/frees it).
    const std::string* get(const std::string& key);

    // Returns the live entry, or nullptr. This is where LAZY EXPIRY lives:
    // every lookup path (get/set/erase all come through here) first checks
    // expires_at, and a dead entry is reaped on the spot and reported as
    // absent. Expiry enforced in ONE place ⇒ no command can forget it.
    Entry* find(const std::string& key);

    // Returns true if the key existed (and was live) and was removed.
    bool erase(const std::string& key);

    // All live (non-expired) keys, in no particular order. O(n) full walk —
    // exists only for the KEYS debug command; never used on a hot path.
    std::vector<std::string> keys() const;

    // Cursor-based, NON-blocking iteration (powers the SCAN command). One call
    // visits a bounded handful of buckets starting from `cursor`, appends their
    // live keys to `out`, and returns the NEXT cursor; a returned 0 means the
    // full pass is done. Unlike keys(), this never walks the whole table at
    // once, so it can't stall the single thread on a big store.
    //
    // The magic is the cursor scheme (Redis's): buckets are visited in
    // reverse-binary-increment order, which keeps the guarantee "every key
    // present for the whole scan is returned at least once" holding EVEN IF the
    // table doubles (rehashes) between calls. Keys may repeat and the count per
    // call only approximates `count` — both are fine and match Redis. Expired-
    // but-unreaped keys are skipped (like keys()); this is a read, it reaps
    // nothing.
    size_t scan(size_t cursor, size_t count, std::vector<std::string>& out) const;

    // Deletes everything (FLUSHALL): entries, LRU list, memory accounting,
    // any in-flight rehash. Lifetime stats (evicted/expired) survive — they
    // count events, not contents.
    void clear();

    // Number of entries — may include expired-but-not-yet-reaped keys,
    // exactly like Redis's DBSIZE. Lazy/active expiry shrinks it.
    size_t size() const { return used_; }

    // ACTIVE EXPIRY: called from the event-loop tick (~100ms). Samples
    // random buckets and reaps expired entries, with strictly bounded work
    // per call so the single thread can never stall on cleanup. Redis-style
    // adaptive: if a round finds >25% of sampled entries expired, it goes
    // again (capped) — a heavily-expired table gets cleaned faster.
    // Returns how many entries were reaped.
    int expire_cycle(long long now);

    // ---- maxmemory / LRU eviction (Phase 6) ----

    // Byte budget; 0 = unlimited. When a SET pushes used_memory() over it,
    // entries are evicted from the LRU tail until back under (or only the
    // just-written key remains). Policy is hard-coded allkeys-lru: ANY key
    // may be evicted, TTL or not. (Redis makes this configurable:
    // noeviction, volatile-lru, allkeys-random, ...)
    void set_maxmemory(size_t bytes) { maxmemory_ = bytes; }
    size_t maxmemory() const { return maxmemory_; }

    // Estimated bytes held by entries (struct + key + value bytes). An
    // ESTIMATE: real Redis asks its allocator (zmalloc) for the truth; we
    // don't count allocator overhead, string capacity slack, or the bucket
    // arrays (~8 bytes/entry at load factor 1 vs ~100+ for an entry — noise).
    size_t used_memory() const { return used_memory_; }

    // Total keys evicted since startup (INFO's evicted_keys stat).
    size_t evicted_keys() const { return evicted_; }

    // Total keys reaped by TTL expiry (lazy or active) since startup.
    size_t expired_keys() const { return expired_; }

    // What one entry would cost in used_memory() terms. Public + static so
    // the command layer can pre-check a write against maxmemory WITHOUT
    // storing it first (the oversized-write guard).
    static size_t entry_size(const std::string& key, const std::string& value) {
        return sizeof(Entry) + key.size() + value.size();
    }

    // Introspection for tests.
    bool is_rehashing() const { return rehash_idx_ != NOT_REHASHING; }
    size_t bucket_count() const { return ht_[0].size(); }

private:
    static constexpr size_t INITIAL_BUCKETS = 4;          // like Redis's DICT_HT_INITIAL_SIZE
    static constexpr size_t NOT_REHASHING = SIZE_MAX;

    // ht_[0] is the live table. During a rehash ht_[1] is the bigger table we
    // are migrating into; otherwise it's empty.
    std::vector<Entry*> ht_[2];
    size_t used_ = 0;                      // total entries across both tables
    size_t rehash_idx_ = NOT_REHASHING;    // next ht_[0] bucket to migrate

    // LRU list endpoints + memory accounting (Phase 6).
    Entry* lru_head_ = nullptr;            // most recently used
    Entry* lru_tail_ = nullptr;            // least recently used
    size_t used_memory_ = 0;
    size_t maxmemory_ = 0;                 // 0 = unlimited
    size_t evicted_ = 0;
    size_t expired_ = 0;

    void start_rehash();
    void rehash_step();
    Entry* find_entry(const std::string& key);   // raw: no expiry check
    bool erase_entry(const std::string& key);    // raw: no expiry check

    // LRU list surgery. touch = move to head (called on every live find()).
    void lru_push_front(Entry* e);
    void lru_unlink(Entry* e);
    void lru_touch(Entry* e);

    // The ONE place an entry dies: unlinks from the LRU list, subtracts its
    // memory, frees it, decrements used_. (The bucket-chain unlink stays with
    // the caller — it already holds the chain position.)
    void destroy_entry(Entry* e);

    // Evict from the LRU tail while over budget. Never evicts `just_written`
    // — so one value larger than the whole budget stays (Redis would instead
    // refuse the write with an OOM error; noted simplification).
    void evict_if_needed(const Entry* just_written);
};
